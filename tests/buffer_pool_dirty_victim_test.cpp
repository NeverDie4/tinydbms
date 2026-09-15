#include "buffer_pool.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <source_location>
#include <stdexcept>
#include <thread>
#include <vector>

namespace tinydbms::storage::internal {
struct BufferPoolTestAccess {
    static const auto& frames(const BufferPool& pool) { return pool.frames_; }
    static const auto& table(const BufferPool& pool) { return pool.page_table_; }
    static const auto& order(const BufferPool& pool) { return pool.fifo_.order_; }
    static bool has_reservation(const BufferPool& pool) {
        std::lock_guard lock(pool.metadata_mutex_);
        return std::any_of(pool.frames_.begin(), pool.frames_.end(), [](const Frame& frame) {
            return frame.replacement_reserved;
        });
    }
    static std::uint32_t waiters(const BufferPool& pool, PageKey key) {
        std::lock_guard lock(pool.metadata_mutex_);
        const auto found = pool.loading_table_.find(key);
        if (found == pool.loading_table_.end() || !found->second || !found->second->completion) return 0;
        return found->second->completion->waiter_count.load(std::memory_order_relaxed);
    }
};
} // namespace tinydbms::storage::internal

namespace {
using namespace tinydbms;
using namespace tinydbms::storage::internal;

void check(bool condition, std::source_location at = std::source_location::current()) {
    if (!condition)
        throw std::runtime_error("dirty victim contract at line " + std::to_string(at.line()));
}

struct Gate {
    void enter_and_wait() {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    }
    void wait_until_entered() {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return entered; });
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
};

struct TemporaryPool {
    explicit TemporaryPool(std::string_view suffix) {
        path = std::filesystem::temp_directory_path() /
            ("tinydbms-buffer-pool-dirty-" + std::string(suffix) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        check(std::filesystem::create_directory(path));
        auto opened = FileManager::open(path); check(opened.value.has_value());
        files = std::move(*opened.value);
        auto created = files->create_table_file(0); check(created.value.has_value());
        file = *created.value;
        for (PageId page = 1; page <= 3; ++page) check(file->allocate_page().value == page);
    }
    ~TemporaryPool() {
        pool.reset();
        files.reset();
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    void make(BufferPool::ReadPage read = {}, BufferPool::WritePage write = {},
              ReplacementPolicy policy = ReplacementPolicy::kFifo) {
        auto made = BufferPool::create(*files, 2, std::move(read), std::move(write), policy);
        check(made.value.has_value());
        pool = std::move(*made.value);
    }
    void warm_and_dirty_first() {
        for (PageId page : {1U, 2U}) {
            auto warm = pool->fetch_page({0, page}); check(warm.value.has_value()); warm.value->release();
        }
        auto dirty = pool->fetch_page({0, 1}); check(dirty.value.has_value());
        dirty.value->mutable_page().bytes[0] = std::byte{41};
        dirty.value->mark_dirty();
        dirty.value->release();
    }

    std::filesystem::path path;
    std::unique_ptr<FileManager> files;
    PageFile* file = nullptr;
    std::unique_ptr<BufferPool> pool;
};

void dirty_victim_requested_read_does_not_block_unrelated_ready_hit() {
    const auto path = std::filesystem::temp_directory_path() /
        ("tinydbms-buffer-pool-dirty-victim-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(path));
    auto opened = FileManager::open(path); check(opened.value.has_value());
    auto files = std::move(*opened.value);
    auto created = files->create_table_file(0); check(created.value.has_value());
    PageFile* file = *created.value;
    for (PageId page = 1; page <= 3; ++page) check(file->allocate_page().value == page);

    Gate read_gate;
    auto read = [&](PageKey key) -> PageFileResult<RawPage> {
        if (key == PageKey{0, 3}) read_gate.enter_and_wait();
        return file->read_page(key.page_id);
    };
    auto made = BufferPool::create(*files, 2, read); check(made.value.has_value());
    auto pool = std::move(*made.value);
    for (PageId page : {1U, 2U}) {
        auto warm = pool->fetch_page({0, page}); check(warm.value.has_value()); warm.value->release();
    }
    auto dirty = pool->fetch_page({0, 1}); check(dirty.value.has_value());
    dirty.value->mutable_page().bytes[0] = std::byte{41};
    dirty.value->mark_dirty();
    dirty.value->release();

    BufferPoolResult<PageGuard> replacement, hit;
    std::thread creator([&] { replacement = pool->fetch_page({0, 3}); });
    read_gate.wait_until_entered();
    std::atomic<bool> hit_finished = false;
    std::thread reader([&] {
        hit = pool->fetch_page({0, 2});
        hit_finished.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!hit_finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool hit_finished_before_read_release = hit_finished.load(std::memory_order_acquire);
    read_gate.release();
    creator.join();
    reader.join();

    check(hit_finished_before_read_release);
    check(replacement.value.has_value() && hit.value.has_value());
    replacement.value->release();
    hit.value->release();
    check(!pool->close());
    pool.reset(); files.reset();
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}

void dirty_victim_same_new_key_deduplicates_read_and_write() {
    TemporaryPool fixture("dedup");
    Gate gate;
    std::atomic<unsigned> reads = 0, writes = 0;
    fixture.make(
        [&](PageKey key) -> PageFileResult<RawPage> {
            if (key == PageKey{0, 3}) {
                ++reads;
                gate.enter_and_wait();
            }
            return fixture.file->read_page(key.page_id);
        },
        [&](PageKey key, const RawPage& page) -> std::optional<PageFileError> {
            if (key == PageKey{0, 1}) ++writes;
            return fixture.file->write_page(key.page_id, page);
        });
    fixture.warm_and_dirty_first();
    const auto before = fixture.pool->stats();
    constexpr unsigned kCallers = 4;
    std::array<BufferPoolResult<PageGuard>, kCallers> results;
    std::thread creator([&] { results[0] = fixture.pool->fetch_page({0, 3}); });
    gate.wait_until_entered();
    std::vector<std::thread> followers;
    for (unsigned caller = 1; caller < kCallers; ++caller)
        followers.emplace_back([&, caller] { results[caller] = fixture.pool->fetch_page({0, 3}); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (BufferPoolTestAccess::waiters(*fixture.pool, {0, 3}) != kCallers - 1 &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const bool followers_waiting = BufferPoolTestAccess::waiters(*fixture.pool, {0, 3}) == kCallers - 1;
    gate.release();
    creator.join();
    for (auto& follower : followers) follower.join();

    check(followers_waiting && reads.load() == 1 && writes.load() == 1);
    for (auto& result : results) {
        check(result.value.has_value() && !result.error.has_value());
        result.value->release();
    }
    const auto after = fixture.pool->stats();
    check(after.fetch_count == before.fetch_count + kCallers);
    check(after.miss_count == before.miss_count + kCallers);
    check(after.hit_count == before.hit_count);
    check(after.dirty_flush_count == before.dirty_flush_count + 1);
    check(after.eviction_count == before.eviction_count + 1);
    check(!BufferPoolTestAccess::has_reservation(*fixture.pool));
    check(!fixture.pool->close());
}

void dirty_victim_read_failure_preserves_old_page() {
    TemporaryPool fixture("read-failure");
    std::atomic<unsigned> writes = 0;
    fixture.make(
        [&](PageKey key) -> PageFileResult<RawPage> {
            if (key == PageKey{0, 3})
                return {std::nullopt, PageFileError{PageFileErrorKind::kIo, "injected requested read failure"}};
            return fixture.file->read_page(key.page_id);
        },
        [&](PageKey key, const RawPage& page) -> std::optional<PageFileError> {
            ++writes;
            return fixture.file->write_page(key.page_id, page);
        });
    fixture.warm_and_dirty_first();
    const auto before_frames = BufferPoolTestAccess::frames(*fixture.pool);
    const auto before_order = BufferPoolTestAccess::order(*fixture.pool);
    const auto before = fixture.pool->stats();
    const auto result = fixture.pool->fetch_page({0, 3});
    check(!result.value && result.error && result.error->kind == BufferPoolErrorKind::kIo);
    check(writes.load() == 0 && BufferPoolTestAccess::order(*fixture.pool) == before_order);
    const auto& after_frames = BufferPoolTestAccess::frames(*fixture.pool);
    check(after_frames[0].key == before_frames[0].key &&
          after_frames[0].page.bytes == before_frames[0].page.bytes && after_frames[0].dirty);
    const auto after = fixture.pool->stats();
    check(after.dirty_flush_count == before.dirty_flush_count && after.eviction_count == before.eviction_count);
    check(!BufferPoolTestAccess::has_reservation(*fixture.pool));
    check(!fixture.pool->close());
}

void dirty_victim_write_failure_preserves_old_page() {
    TemporaryPool fixture("write-failure");
    std::atomic<unsigned> reads = 0, writes = 0;
    fixture.make(
        [&](PageKey key) -> PageFileResult<RawPage> {
            if (key == PageKey{0, 3}) ++reads;
            return fixture.file->read_page(key.page_id);
        },
        [&](PageKey key, const RawPage&) -> std::optional<PageFileError> {
            if (key == PageKey{0, 1}) {
                ++writes;
                return PageFileError{PageFileErrorKind::kIo, "injected dirty write failure"};
            }
            return std::nullopt;
        });
    fixture.warm_and_dirty_first();
    const auto before_frames = BufferPoolTestAccess::frames(*fixture.pool);
    const auto before_order = BufferPoolTestAccess::order(*fixture.pool);
    const auto before = fixture.pool->stats();
    const auto result = fixture.pool->fetch_page({0, 3});
    check(!result.value && result.error && result.error->kind == BufferPoolErrorKind::kIo);
    check(reads.load() == 1 && writes.load() == 1 && BufferPoolTestAccess::order(*fixture.pool) == before_order);
    const auto& after_frames = BufferPoolTestAccess::frames(*fixture.pool);
    check(after_frames[0].key == before_frames[0].key &&
          after_frames[0].page.bytes == before_frames[0].page.bytes && after_frames[0].dirty);
    const auto after = fixture.pool->stats();
    check(after.dirty_flush_count == before.dirty_flush_count && after.eviction_count == before.eviction_count);
    check(!BufferPoolTestAccess::table(*fixture.pool).contains({0, 3}) &&
          !BufferPoolTestAccess::has_reservation(*fixture.pool));
}

void old_key_hit_during_requested_read_cancels_before_dirty_write() {
    TemporaryPool fixture("read-cancel");
    Gate gate;
    std::atomic<unsigned> writes = 0;
    fixture.make(
        [&](PageKey key) -> PageFileResult<RawPage> {
            if (key == PageKey{0, 3}) gate.enter_and_wait();
            return fixture.file->read_page(key.page_id);
        },
        [&](PageKey key, const RawPage& page) -> std::optional<PageFileError> {
            if (key == PageKey{0, 1}) ++writes;
            return fixture.file->write_page(key.page_id, page);
        });
    fixture.warm_and_dirty_first();
    BufferPoolResult<PageGuard> replacement;
    std::thread creator([&] { replacement = fixture.pool->fetch_page({0, 3}); });
    gate.wait_until_entered();
    auto old = fixture.pool->fetch_page({0, 1}); check(old.value.has_value()); old.value->release();
    gate.release();
    creator.join();

    check(!replacement.value && replacement.error && replacement.error->kind == BufferPoolErrorKind::kNoVictim);
    check(writes.load() == 0 && BufferPoolTestAccess::table(*fixture.pool).contains({0, 1}) &&
          !BufferPoolTestAccess::table(*fixture.pool).contains({0, 3}));
    check(BufferPoolTestAccess::frames(*fixture.pool)[0].dirty && !BufferPoolTestAccess::has_reservation(*fixture.pool));
    check(!fixture.pool->close());
}

void old_key_hit_during_dirty_write_cancels_but_records_physical_write() {
    TemporaryPool fixture("write-cancel");
    Gate gate;
    std::atomic<unsigned> writes = 0;
    fixture.make(
        [&](PageKey key) { return fixture.file->read_page(key.page_id); },
        [&](PageKey key, const RawPage& page) -> std::optional<PageFileError> {
            if (key == PageKey{0, 1}) {
                ++writes;
                gate.enter_and_wait();
            }
            return fixture.file->write_page(key.page_id, page);
        });
    fixture.warm_and_dirty_first();
    const auto before_pool = fixture.pool->stats();
    const auto before_file = fixture.file->stats();
    BufferPoolResult<PageGuard> replacement;
    std::thread creator([&] { replacement = fixture.pool->fetch_page({0, 3}); });
    gate.wait_until_entered();
    auto old = fixture.pool->fetch_page({0, 1}); check(old.value.has_value());
    old.value->mutable_page().bytes[0] = std::byte{99};
    old.value->mark_dirty();
    old.value->release();
    gate.release();
    creator.join();

    check(!replacement.value && replacement.error && replacement.error->kind == BufferPoolErrorKind::kNoVictim);
    check(writes.load() == 1 && fixture.file->stats().physical_write_successes ==
          before_file.physical_write_successes + 1);
    const auto after_pool = fixture.pool->stats();
    check(after_pool.dirty_flush_count == before_pool.dirty_flush_count &&
          after_pool.eviction_count == before_pool.eviction_count);
    check(BufferPoolTestAccess::table(*fixture.pool).contains({0, 1}) &&
          !BufferPoolTestAccess::table(*fixture.pool).contains({0, 3}) &&
          BufferPoolTestAccess::frames(*fixture.pool)[0].dirty &&
          BufferPoolTestAccess::frames(*fixture.pool)[0].page.bytes[0] == std::byte{99} &&
          !BufferPoolTestAccess::has_reservation(*fixture.pool));
    const auto persisted = fixture.file->read_page(1);
    check(persisted.value.has_value() && persisted.value->bytes[0] == std::byte{41});
    check(!fixture.pool->close());
}

void old_key_hit_after_physical_write_before_commit_cancels() {
    TemporaryPool fixture("post-write-cancel");
    Gate gate;
    fixture.make(
        [&](PageKey key) { return fixture.file->read_page(key.page_id); },
        [&](PageKey key, const RawPage& page) -> std::optional<PageFileError> {
            const auto error = fixture.file->write_page(key.page_id, page);
            if (key == PageKey{0, 1}) gate.enter_and_wait();
            return error;
        });
    fixture.warm_and_dirty_first();
    const auto before_pool = fixture.pool->stats();
    const auto before_file = fixture.file->stats();
    BufferPoolResult<PageGuard> replacement;
    std::thread creator([&] { replacement = fixture.pool->fetch_page({0, 3}); });
    gate.wait_until_entered();
    check(fixture.file->stats().physical_write_successes == before_file.physical_write_successes + 1);
    auto old = fixture.pool->fetch_page({0, 1}); check(old.value.has_value()); old.value->release();
    gate.release();
    creator.join();

    check(!replacement.value && replacement.error && replacement.error->kind == BufferPoolErrorKind::kNoVictim);
    const auto after_pool = fixture.pool->stats();
    check(after_pool.dirty_flush_count == before_pool.dirty_flush_count &&
          after_pool.eviction_count == before_pool.eviction_count &&
          BufferPoolTestAccess::frames(*fixture.pool)[0].dirty &&
          BufferPoolTestAccess::table(*fixture.pool).contains({0, 1}) &&
          !BufferPoolTestAccess::table(*fixture.pool).contains({0, 3}) &&
          !BufferPoolTestAccess::has_reservation(*fixture.pool));
    check(!fixture.pool->close());
}

void explicit_flush_during_dirty_replacement_cancels_without_restoring_dirty() {
    TemporaryPool fixture("explicit-flush");
    Gate gate;
    std::atomic<unsigned> writes = 0;
    fixture.make(
        [&](PageKey key) { return fixture.file->read_page(key.page_id); },
        [&](PageKey key, const RawPage& page) -> std::optional<PageFileError> {
            const unsigned write_number = writes.fetch_add(1, std::memory_order_relaxed);
            if (key == PageKey{0, 1} && write_number == 0) gate.enter_and_wait();
            return fixture.file->write_page(key.page_id, page);
        });
    fixture.warm_and_dirty_first();
    const auto before_pool = fixture.pool->stats();
    const auto before_file = fixture.file->stats();
    BufferPoolResult<PageGuard> replacement;
    std::thread creator([&] { replacement = fixture.pool->fetch_page({0, 3}); });
    gate.wait_until_entered();
    check(!fixture.pool->flush_page({0, 1}));
    check(!BufferPoolTestAccess::frames(*fixture.pool)[0].dirty);
    gate.release();
    creator.join();

    check(!replacement.value && replacement.error && replacement.error->kind == BufferPoolErrorKind::kNoVictim);
    const auto after_pool = fixture.pool->stats();
    check(writes.load() == 2 && fixture.file->stats().physical_write_successes ==
          before_file.physical_write_successes + 2);
    check(after_pool.dirty_flush_count == before_pool.dirty_flush_count + 1 &&
          after_pool.eviction_count == before_pool.eviction_count &&
          !BufferPoolTestAccess::frames(*fixture.pool)[0].dirty &&
          BufferPoolTestAccess::table(*fixture.pool).contains({0, 1}) &&
          !BufferPoolTestAccess::table(*fixture.pool).contains({0, 3}) &&
          !BufferPoolTestAccess::has_reservation(*fixture.pool));
    check(!fixture.pool->close());
}

void dirty_reservation_keeps_close_and_release_table_retryable() {
    TemporaryPool fixture("lifecycle");
    Gate gate;
    fixture.make([&](PageKey key) -> PageFileResult<RawPage> {
        if (key == PageKey{0, 3}) gate.enter_and_wait();
        return fixture.file->read_page(key.page_id);
    });
    fixture.warm_and_dirty_first();
    BufferPoolResult<PageGuard> replacement;
    std::thread creator([&] { replacement = fixture.pool->fetch_page({0, 3}); });
    gate.wait_until_entered();
    std::atomic<bool> close_done = false, release_done = false;
    std::optional<BufferPoolError> close_error, release_error;
    std::thread closer([&] {
        close_error = fixture.pool->close();
        close_done.store(true, std::memory_order_release);
    });
    std::thread releaser([&] {
        release_error = fixture.pool->release_table(0);
        release_done.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!close_done.load(std::memory_order_acquire) || !release_done.load(std::memory_order_acquire)) &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const bool returned_during_read = close_done.load(std::memory_order_acquire) &&
        release_done.load(std::memory_order_acquire);
    gate.release();
    creator.join();
    closer.join();
    releaser.join();

    check(returned_during_read && close_error.has_value() && release_error.has_value());
    check(replacement.value.has_value());
    replacement.value->release();
    check(!fixture.pool->close());
}

void dirty_write_overlaps_cold_read_on_another_page_file() {
    TemporaryPool fixture("cross-file-overlap");
    auto created = fixture.files->create_table_file(1); check(created.value.has_value());
    PageFile* second_file = *created.value;
    for (PageId page = 1; page <= 2; ++page) check(second_file->allocate_page().value == page);
    Gate write_gate, cold_read_gate;
    auto read = [&](PageKey key) -> PageFileResult<RawPage> {
        if (key == PageKey{1, 2}) cold_read_gate.enter_and_wait();
        return (key.table_id == 0 ? fixture.file : second_file)->read_page(key.page_id);
    };
    auto write = [&](PageKey key, const RawPage& page) -> std::optional<PageFileError> {
        if (key == PageKey{0, 1}) write_gate.enter_and_wait();
        return (key.table_id == 0 ? fixture.file : second_file)->write_page(key.page_id, page);
    };
    fixture.make(read, write);
    fixture.warm_and_dirty_first();
    BufferPoolResult<PageGuard> dirty_replacement, cold_replacement;
    std::thread writer([&] { dirty_replacement = fixture.pool->fetch_page({1, 1}); });
    write_gate.wait_until_entered();
    std::thread cold_reader([&] { cold_replacement = fixture.pool->fetch_page({1, 2}); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool cold_read_entered = false;
    {
        std::unique_lock lock(cold_read_gate.mutex);
        cold_read_entered = cold_read_gate.changed.wait_until(lock, deadline, [&] {
            return cold_read_gate.entered;
        });
    }
    cold_read_gate.release();
    write_gate.release();
    writer.join();
    cold_reader.join();

    check(cold_read_entered && dirty_replacement.value.has_value() && cold_replacement.value.has_value());
    dirty_replacement.value->release();
    cold_replacement.value->release();
    check(!fixture.pool->close());
}

void dirty_victim_success_preserves_fifo_and_lru_commit_rules() {
    for (const ReplacementPolicy policy : {ReplacementPolicy::kFifo, ReplacementPolicy::kLru}) {
        TemporaryPool fixture(policy == ReplacementPolicy::kFifo ? "fifo" : "lru");
        fixture.make({}, {}, policy);
        auto first = fixture.pool->fetch_page({0, 1}); check(first.value.has_value());
        first.value->mutable_page().bytes[0] = std::byte{7};
        first.value->mark_dirty();
        auto second = fixture.pool->fetch_page({0, 2}); check(second.value.has_value());
        second.value->release();
        first.value->release();
        const auto before = fixture.pool->stats();
        auto replacement = fixture.pool->fetch_page({0, 3}); check(replacement.value.has_value());
        replacement.value->release();
        const auto after = fixture.pool->stats();
        check(!BufferPoolTestAccess::table(*fixture.pool).contains({0, 1}) &&
              BufferPoolTestAccess::table(*fixture.pool).contains({0, 3}));
        check(after.dirty_flush_count == before.dirty_flush_count + 1 &&
              after.eviction_count == before.eviction_count + 1);
        check(after.dirty_victim_selection_count == before.dirty_victim_selection_count + 1 &&
              after.dirty_requested_read_count == before.dirty_requested_read_count + 1 &&
              after.dirty_write_attempt_count == before.dirty_write_attempt_count + 1 &&
              after.dirty_write_success_count == before.dirty_write_success_count + 1 &&
              after.dirty_replacement_commit_count == before.dirty_replacement_commit_count + 1 &&
              after.dirty_replacement_cancel_count == before.dirty_replacement_cancel_count);
        const FrameId loaded = BufferPoolTestAccess::table(*fixture.pool).at({0, 3});
        check(BufferPoolTestAccess::order(*fixture.pool).back() == loaded);
        check(!fixture.pool->close());
    }
}
} // namespace

int main() {
    try {
        dirty_victim_requested_read_does_not_block_unrelated_ready_hit();
        dirty_victim_same_new_key_deduplicates_read_and_write();
        dirty_victim_read_failure_preserves_old_page();
        dirty_victim_write_failure_preserves_old_page();
        old_key_hit_during_requested_read_cancels_before_dirty_write();
        old_key_hit_during_dirty_write_cancels_but_records_physical_write();
        old_key_hit_after_physical_write_before_commit_cancels();
        explicit_flush_during_dirty_replacement_cancels_without_restoring_dirty();
        dirty_reservation_keeps_close_and_release_table_retryable();
        dirty_write_overlaps_cold_read_on_another_page_file();
        dirty_victim_success_preserves_fifo_and_lru_commit_rules();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
