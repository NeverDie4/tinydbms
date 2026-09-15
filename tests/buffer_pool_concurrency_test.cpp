#include "buffer_pool.h"

#include <algorithm>
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
    static LifecycleState lifecycle(const BufferPool& pool) { return pool.lifecycle_; }
    static std::uint32_t loading_waiters(const BufferPool& pool, PageKey key) {
        std::lock_guard lock(pool.metadata_mutex_);
        const auto found = pool.page_table_.find(key);
        if (found == pool.page_table_.end()) return 0;
        const auto& frame = pool.frames_.at(found->second);
        if (frame.state != FrameState::kLoading || !frame.completion) return 0;
        return frame.completion->waiter_count.load(std::memory_order_relaxed);
    }
    static std::uint32_t victim_loading_waiters(const BufferPool& pool, PageKey key) {
        std::lock_guard lock(pool.metadata_mutex_);
        const auto found = pool.loading_table_.find(key);
        if (found == pool.loading_table_.end() || !found->second || !found->second->completion) return 0;
        return found->second->completion->waiter_count.load(std::memory_order_relaxed);
    }
    static bool has_victim_loading(const BufferPool& pool, PageKey key) {
        std::lock_guard lock(pool.metadata_mutex_);
        return pool.loading_table_.contains(key);
    }
    static bool has_reservation(const BufferPool& pool) {
        std::lock_guard lock(pool.metadata_mutex_);
        return std::any_of(pool.frames_.begin(), pool.frames_.end(), [](const Frame& frame) {
            return frame.replacement_reserved;
        });
    }
};
} // namespace tinydbms::storage::internal

namespace {
using namespace tinydbms;
using namespace tinydbms::storage;
using namespace tinydbms::storage::internal;

void check(bool condition, std::source_location at = std::source_location::current()) {
    if (!condition) throw std::runtime_error("BufferPool concurrency contract at line " + std::to_string(at.line()));
}

struct Fixture {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("tinydbms-buffer-pool-concurrency-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::unique_ptr<FileManager> files;
    std::unique_ptr<BufferPool> pool;

    Fixture() {
        check(std::filesystem::create_directory(path));
        auto opened = FileManager::open(path); check(opened.value.has_value()); files = std::move(*opened.value);
        auto created = files->create_table_file(0); check(created.value.has_value());
        for (unsigned page = 1; page <= 4; ++page) check((*created.value)->allocate_page().value == page);
        auto made = BufferPool::create(*files, 4); check(made.value.has_value()); pool = std::move(*made.value);
    }
    ~Fixture() {
        pool.reset(); files.reset();
        std::error_code ignored; std::filesystem::remove_all(path, ignored);
    }
    void warm(PageKey key) {
        auto result = pool->fetch_page(key); check(result.value.has_value()); result.value->release();
    }
    void warm_all() { for (PageId page = 1; page <= 4; ++page) warm({0, page}); }
};

template <typename Work>
void run_concurrently(unsigned threads, Work work) {
    std::atomic<unsigned> ready = 0;
    std::atomic<bool> start = false;
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (unsigned index = 0; index < threads; ++index) {
        workers.emplace_back([&, index] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            work(index);
        });
    }
    while (ready.load(std::memory_order_acquire) != threads) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
}

void concurrent_resident_hits_and_stats() {
    Fixture f; f.warm({0, 1});
    constexpr unsigned kThreads = 8, kIterations = 100;
    const auto before = f.pool->stats();
    std::atomic<bool> failed = false;
    run_concurrently(kThreads, [&](unsigned) {
        for (unsigned iteration = 0; iteration < kIterations; ++iteration) {
            auto result = f.pool->fetch_page({0, 1});
            if (!result.value || result.value->page().bytes[0] != std::byte{0}) { failed = true; return; }
            result.value->release();
        }
    });
    check(!failed.load());
    const auto after = f.pool->stats();
    check(after.fetch_count == before.fetch_count + kThreads * kIterations);
    check(after.hit_count == before.hit_count + kThreads * kIterations);
    check(after.miss_count == before.miss_count);
    check(BufferPoolTestAccess::frames(*f.pool).at(0).pin_count == 0);
    check(!f.pool->close());
}

void concurrent_different_resident_pages() {
    Fixture f; f.warm_all();
    std::atomic<bool> failed = false;
    run_concurrently(4, [&](unsigned index) {
        const PageKey key{0, index + 1};
        for (unsigned iteration = 0; iteration < 100; ++iteration) {
            auto result = f.pool->fetch_page(key);
            if (!result.value || result.value->page().bytes[0] != std::byte{0}) { failed = true; return; }
            result.value->release();
        }
    });
    check(!failed.load());
    for (const auto& frame : BufferPoolTestAccess::frames(*f.pool)) check(frame.pin_count == 0);
    check(!f.pool->close());
}

void concurrent_distinct_dirty_release() {
    Fixture f; f.warm_all();
    std::atomic<bool> failed = false;
    run_concurrently(4, [&](unsigned index) {
        auto result = f.pool->fetch_page({0, index + 1});
        if (!result.value) { failed = true; return; }
        result.value->mark_dirty();
        result.value->release();
    });
    check(!failed.load());
    for (const auto& frame : BufferPoolTestAccess::frames(*f.pool)) check(frame.dirty && frame.pin_count == 0);
    check(!f.pool->close());
}

void close_lifecycle_and_retry() {
    Fixture f; f.warm({0, 1});
    check(BufferPoolTestAccess::lifecycle(*f.pool) == LifecycleState::kOpen);
    auto pinned = f.pool->fetch_page({0, 1}); check(pinned.value.has_value());
    check(f.pool->close().has_value());
    check(BufferPoolTestAccess::lifecycle(*f.pool) == LifecycleState::kOpen);
    pinned.value->release();
    check(!f.pool->close());
    check(BufferPoolTestAccess::lifecycle(*f.pool) == LifecycleState::kClosed);
    check(f.pool->fetch_page({0, 1}).error.has_value());
    check(f.pool->flush_page({0, 1}).has_value());
    check(f.pool->flush_all().has_value());
    check(f.pool->release_table(0).has_value());
    check(f.pool->snapshot_dirty_frames_for_experiment().error.has_value());
}

struct ReadGate {
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

void same_cold_miss_uses_one_read_and_shared_completion() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("tinydbms-buffer-pool-loading-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(path));
    auto opened = FileManager::open(path); check(opened.value.has_value());
    auto files = std::move(*opened.value);
    auto created = files->create_table_file(0); check(created.value.has_value());
    check((*created.value)->allocate_page().value == 1);

    ReadGate gate;
    std::atomic<unsigned> reads = 0;
    auto read = [&](PageKey key) -> PageFileResult<RawPage> {
        if (reads.fetch_add(1, std::memory_order_relaxed) == 0) gate.enter_and_wait();
        return files->find_table_file(key.table_id)->read_page(key.page_id);
    };
    auto made = BufferPool::create(*files, 2, read); check(made.value.has_value());
    auto pool = std::move(*made.value);
    const auto before = pool->stats();

    constexpr unsigned kCallers = 4;
    std::array<BufferPoolResult<PageGuard>, kCallers> results;
    std::thread creator([&] { results[0] = pool->fetch_page({0, 1}); });
    gate.wait_until_entered();
    std::vector<std::thread> followers;
    for (unsigned caller = 1; caller < kCallers; ++caller) {
        followers.emplace_back([&, caller] { results[caller] = pool->fetch_page({0, 1}); });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (BufferPoolTestAccess::loading_waiters(*pool, {0, 1}) != kCallers - 1 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool all_followers_waiting =
        BufferPoolTestAccess::loading_waiters(*pool, {0, 1}) == kCallers - 1;
    gate.release();
    creator.join();
    for (auto& follower : followers) follower.join();

    check(all_followers_waiting);
    check(reads.load() == 1);
    for (auto& result : results) {
        check(result.value.has_value() && !result.error.has_value());
        check(result.value->page().bytes[0] == std::byte{0});
        result.value->release();
    }
    const auto after = pool->stats();
    check(after.fetch_count == before.fetch_count + kCallers);
    check(after.miss_count == before.miss_count + kCallers);
    check(after.hit_count == before.hit_count);
    check(BufferPoolTestAccess::frames(*pool).at(0).pin_count == 0);
    check(!pool->close());
    pool.reset(); files.reset();
    std::error_code ignored; std::filesystem::remove_all(path, ignored);
}

void same_cold_miss_failure_is_shared_and_retryable() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("tinydbms-buffer-pool-load-failure-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(path));
    auto opened = FileManager::open(path); check(opened.value.has_value());
    auto files = std::move(*opened.value);
    auto created = files->create_table_file(0); check(created.value.has_value());
    check((*created.value)->allocate_page().value == 1);

    ReadGate gate;
    std::atomic<unsigned> reads = 0;
    std::atomic<bool> fail = true;
    auto read = [&](PageKey key) -> PageFileResult<RawPage> {
        if (reads.fetch_add(1, std::memory_order_relaxed) == 0) gate.enter_and_wait();
        if (fail.load(std::memory_order_relaxed))
            return {std::nullopt, PageFileError{PageFileErrorKind::kIo, "injected concurrent read failure"}};
        return files->find_table_file(key.table_id)->read_page(key.page_id);
    };
    auto made = BufferPool::create(*files, 1, read); check(made.value.has_value());
    auto pool = std::move(*made.value);

    constexpr unsigned kCallers = 3;
    std::array<BufferPoolResult<PageGuard>, kCallers> results;
    std::thread creator([&] { results[0] = pool->fetch_page({0, 1}); });
    gate.wait_until_entered();
    std::vector<std::thread> followers;
    for (unsigned caller = 1; caller < kCallers; ++caller)
        followers.emplace_back([&, caller] { results[caller] = pool->fetch_page({0, 1}); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (BufferPoolTestAccess::loading_waiters(*pool, {0, 1}) != kCallers - 1 &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const bool all_followers_waiting =
        BufferPoolTestAccess::loading_waiters(*pool, {0, 1}) == kCallers - 1;
    gate.release();
    creator.join();
    for (auto& follower : followers) follower.join();

    check(all_followers_waiting && reads.load() == 1);
    for (const auto& result : results) {
        check(!result.value && result.error && result.error->kind == BufferPoolErrorKind::kIo);
        check(result.error->message == "injected concurrent read failure");
    }
    check(BufferPoolTestAccess::table(*pool).empty());
    const auto& frame = BufferPoolTestAccess::frames(*pool).at(0);
    check(frame.state == FrameState::kFree && !frame.key && !frame.completion && !frame.dirty && frame.pin_count == 0);
    const std::uint64_t failed_ticket = frame.ticket;

    fail.store(false, std::memory_order_relaxed);
    auto retry = pool->fetch_page({0, 1});
    check(retry.value.has_value() && reads.load() == 2);
    check(BufferPoolTestAccess::frames(*pool).at(0).ticket == failed_ticket + 1);
    retry.value->release();
    check(!pool->close());
    pool.reset(); files.reset();
    std::error_code ignored; std::filesystem::remove_all(path, ignored);
}

void distinct_free_frame_reads_overlap_outside_metadata_lock() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("tinydbms-buffer-pool-overlap-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(path));
    auto opened = FileManager::open(path); check(opened.value.has_value());
    auto files = std::move(*opened.value);
    for (TableId table : {0U, 1U}) {
        auto created = files->create_table_file(table); check(created.value.has_value());
        check((*created.value)->allocate_page().value == 1);
    }
    ReadGate first_gate, second_gate;
    auto read = [&](PageKey key) -> PageFileResult<RawPage> {
        (key.table_id == 0 ? first_gate : second_gate).enter_and_wait();
        return files->find_table_file(key.table_id)->read_page(key.page_id);
    };
    auto made = BufferPool::create(*files, 2, read); check(made.value.has_value());
    auto pool = std::move(*made.value);
    BufferPoolResult<PageGuard> first, second;
    std::thread first_reader([&] { first = pool->fetch_page({0, 1}); });
    first_gate.wait_until_entered();
    std::thread second_reader([&] { second = pool->fetch_page({1, 1}); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool second_entered = false;
    {
        std::unique_lock lock(second_gate.mutex);
        second_entered = second_gate.changed.wait_until(lock, deadline, [&] { return second_gate.entered; });
    }
    first_gate.release();
    second_gate.release();
    first_reader.join();
    second_reader.join();
    check(second_entered && first.value && second.value);
    first.value->release(); second.value->release();
    check(!pool->close());
    pool.reset(); files.reset();
    std::error_code ignored; std::filesystem::remove_all(path, ignored);
}

void loading_does_not_block_ready_hit_close_or_victim_selection() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("tinydbms-buffer-pool-loading-isolation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(path));
    auto opened = FileManager::open(path); check(opened.value.has_value());
    auto files = std::move(*opened.value);
    auto created = files->create_table_file(0); check(created.value.has_value());
    for (PageId page = 1; page <= 3; ++page) check((*created.value)->allocate_page().value == page);
    ReadGate gate;
    auto read = [&](PageKey key) -> PageFileResult<RawPage> {
        if (key.page_id == 1) gate.enter_and_wait();
        return files->find_table_file(key.table_id)->read_page(key.page_id);
    };
    auto made = BufferPool::create(*files, 2, read); check(made.value.has_value());
    auto pool = std::move(*made.value);
    auto warm = pool->fetch_page({0, 2}); check(warm.value.has_value()); warm.value->release();

    BufferPoolResult<PageGuard> loading;
    std::thread loader([&] { loading = pool->fetch_page({0, 1}); });
    gate.wait_until_entered();
    auto hit = pool->fetch_page({0, 2});
    check(hit.value.has_value());
    const auto close_error = pool->close();
    check(close_error && BufferPoolTestAccess::lifecycle(*pool) == LifecycleState::kOpen);
    const auto no_victim = pool->fetch_page({0, 3});
    check(!no_victim.value && no_victim.error && no_victim.error->kind == BufferPoolErrorKind::kNoVictim);
    hit.value->release();

    gate.release();
    loader.join();
    check(loading.value.has_value()); loading.value->release();
    check(!pool->close());
    pool.reset(); files.reset();
    std::error_code ignored; std::filesystem::remove_all(path, ignored);
}

void clean_victim_read_does_not_block_unrelated_ready_hit() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("tinydbms-buffer-pool-clean-victim-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(path));
    auto opened = FileManager::open(path); check(opened.value.has_value());
    auto files = std::move(*opened.value);
    auto created = files->create_table_file(0); check(created.value.has_value());
    for (PageId page = 1; page <= 3; ++page) check((*created.value)->allocate_page().value == page);
    ReadGate gate;
    auto read = [&](PageKey key) -> PageFileResult<RawPage> {
        if (key.page_id == 3) gate.enter_and_wait();
        return files->find_table_file(key.table_id)->read_page(key.page_id);
    };
    auto made = BufferPool::create(*files, 2, read); check(made.value.has_value());
    auto pool = std::move(*made.value);
    for (PageId page : {1U, 2U}) {
        auto warm = pool->fetch_page({0, page}); check(warm.value.has_value()); warm.value->release();
    }

    BufferPoolResult<PageGuard> replacement, hit;
    std::thread creator([&] { replacement = pool->fetch_page({0, 3}); });
    gate.wait_until_entered();
    std::atomic<bool> hit_finished = false;
    std::thread reader([&] {
        hit = pool->fetch_page({0, 2});
        hit_finished.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!hit_finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool hit_completed_before_read_release = hit_finished.load(std::memory_order_acquire);
    gate.release();
    creator.join();
    reader.join();

    check(hit_completed_before_read_release);
    check(replacement.value.has_value() && hit.value.has_value());
    replacement.value->release(); hit.value->release();
    check(!pool->close());
    pool.reset(); files.reset();
    std::error_code ignored; std::filesystem::remove_all(path, ignored);
}

void clean_victim_same_new_key_shares_one_completion() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("tinydbms-buffer-pool-clean-dedup-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(path));
    auto opened = FileManager::open(path); check(opened.value.has_value());
    auto files = std::move(*opened.value);
    auto created = files->create_table_file(0); check(created.value.has_value());
    for (PageId page = 1; page <= 3; ++page) check((*created.value)->allocate_page().value == page);
    ReadGate gate;
    std::atomic<unsigned> reads = 0;
    auto read = [&](PageKey key) -> PageFileResult<RawPage> {
        if (key.page_id == 3) {
            ++reads;
            gate.enter_and_wait();
        }
        return files->find_table_file(key.table_id)->read_page(key.page_id);
    };
    auto made = BufferPool::create(*files, 2, read); check(made.value.has_value());
    auto pool = std::move(*made.value);
    for (PageId page : {1U, 2U}) {
        auto warm = pool->fetch_page({0, page}); check(warm.value.has_value()); warm.value->release();
    }
    const auto before = pool->stats();
    constexpr unsigned kCallers = 4;
    std::array<BufferPoolResult<PageGuard>, kCallers> results;
    std::thread creator([&] { results[0] = pool->fetch_page({0, 3}); });
    gate.wait_until_entered();
    std::vector<std::thread> followers;
    for (unsigned caller = 1; caller < kCallers; ++caller)
        followers.emplace_back([&, caller] { results[caller] = pool->fetch_page({0, 3}); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (BufferPoolTestAccess::victim_loading_waiters(*pool, {0, 3}) != kCallers - 1 &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const bool followers_waiting =
        BufferPoolTestAccess::victim_loading_waiters(*pool, {0, 3}) == kCallers - 1;
    gate.release();
    creator.join();
    for (auto& follower : followers) follower.join();

    check(followers_waiting && reads.load() == 1);
    for (auto& result : results) {
        check(result.value.has_value() && !result.error.has_value());
        result.value->release();
    }
    const auto after = pool->stats();
    check(after.fetch_count == before.fetch_count + kCallers);
    check(after.miss_count == before.miss_count + kCallers);
    check(after.hit_count == before.hit_count);
    check(!BufferPoolTestAccess::has_victim_loading(*pool, {0, 3}) &&
          !BufferPoolTestAccess::has_reservation(*pool));
    check(!pool->close());
    pool.reset(); files.reset();
    std::error_code ignored; std::filesystem::remove_all(path, ignored);
}

void clean_victim_old_hit_cancels_and_blocks_close_release() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("tinydbms-buffer-pool-clean-cancel-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(path));
    auto opened = FileManager::open(path); check(opened.value.has_value());
    auto files = std::move(*opened.value);
    auto created = files->create_table_file(0); check(created.value.has_value());
    for (PageId page = 1; page <= 3; ++page) check((*created.value)->allocate_page().value == page);
    ReadGate gate;
    auto read = [&](PageKey key) -> PageFileResult<RawPage> {
        if (key.page_id == 3) gate.enter_and_wait();
        return files->find_table_file(key.table_id)->read_page(key.page_id);
    };
    auto made = BufferPool::create(*files, 2, read); check(made.value.has_value());
    auto pool = std::move(*made.value);
    for (PageId page : {1U, 2U}) {
        auto warm = pool->fetch_page({0, page}); check(warm.value.has_value()); warm.value->release();
    }

    BufferPoolResult<PageGuard> replacement;
    std::thread creator([&] { replacement = pool->fetch_page({0, 3}); });
    gate.wait_until_entered();
    check(BufferPoolTestAccess::has_victim_loading(*pool, {0, 3}) && BufferPoolTestAccess::has_reservation(*pool));
    check(pool->close().has_value() && BufferPoolTestAccess::lifecycle(*pool) == LifecycleState::kOpen);
    check(pool->release_table(0).has_value());
    auto old = pool->fetch_page({0, 1}); check(old.value.has_value()); old.value->release();
    gate.release();
    creator.join();

    check(!replacement.value && replacement.error && replacement.error->kind == BufferPoolErrorKind::kNoVictim);
    check(BufferPoolTestAccess::table(*pool).contains({0, 1}) && BufferPoolTestAccess::table(*pool).contains({0, 2}) &&
          !BufferPoolTestAccess::table(*pool).contains({0, 3}));
    check(!BufferPoolTestAccess::has_victim_loading(*pool, {0, 3}) &&
          !BufferPoolTestAccess::has_reservation(*pool));
    check(!pool->close());
    pool.reset(); files.reset();
    std::error_code ignored; std::filesystem::remove_all(path, ignored);
}

void clean_victim_read_failure_preserves_replacement_order() {
    for (ReplacementPolicy policy : {ReplacementPolicy::kFifo, ReplacementPolicy::kLru}) {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("tinydbms-buffer-pool-clean-failure-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        check(std::filesystem::create_directory(path));
        auto opened = FileManager::open(path); check(opened.value.has_value());
        auto files = std::move(*opened.value);
        auto created = files->create_table_file(0); check(created.value.has_value());
        for (PageId page = 1; page <= 3; ++page) check((*created.value)->allocate_page().value == page);
        auto read = [&](PageKey key) -> PageFileResult<RawPage> {
            if (key.page_id == 3)
                return {std::nullopt, PageFileError{PageFileErrorKind::kIo, "injected clean victim read failure"}};
            return files->find_table_file(key.table_id)->read_page(key.page_id);
        };
        auto made = BufferPool::create(*files, 2, read, {}, policy); check(made.value.has_value());
        auto pool = std::move(*made.value);
        for (PageId page : {1U, 2U}) {
            auto warm = pool->fetch_page({0, page}); check(warm.value.has_value()); warm.value->release();
        }
        const auto before_frames = BufferPoolTestAccess::frames(*pool);
        const auto before_order = BufferPoolTestAccess::order(*pool);
        auto failed = pool->fetch_page({0, 3});
        check(!failed.value && failed.error && failed.error->kind == BufferPoolErrorKind::kIo);
        check(BufferPoolTestAccess::order(*pool) == before_order);
        const auto& after_frames = BufferPoolTestAccess::frames(*pool);
        for (std::size_t index = 0; index < after_frames.size(); ++index) {
            check(after_frames[index].key == before_frames[index].key &&
                  after_frames[index].page.bytes == before_frames[index].page.bytes &&
                  after_frames[index].dirty == before_frames[index].dirty &&
                  after_frames[index].pin_count == before_frames[index].pin_count);
        }
        check(!BufferPoolTestAccess::has_victim_loading(*pool, {0, 3}) &&
              !BufferPoolTestAccess::has_reservation(*pool));
        check(!pool->close());
        pool.reset(); files.reset();
        std::error_code ignored; std::filesystem::remove_all(path, ignored);
    }
}

void clean_victim_reads_on_distinct_files_overlap() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("tinydbms-buffer-pool-clean-overlap-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(path));
    auto opened = FileManager::open(path); check(opened.value.has_value());
    auto files = std::move(*opened.value);
    for (TableId table : {0U, 1U}) {
        auto created = files->create_table_file(table); check(created.value.has_value());
        for (PageId page = 1; page <= 2; ++page) check((*created.value)->allocate_page().value == page);
    }
    ReadGate first_gate, second_gate;
    auto read = [&](PageKey key) -> PageFileResult<RawPage> {
        if (key.page_id == 2) (key.table_id == 0 ? first_gate : second_gate).enter_and_wait();
        return files->find_table_file(key.table_id)->read_page(key.page_id);
    };
    auto made = BufferPool::create(*files, 2, read); check(made.value.has_value());
    auto pool = std::move(*made.value);
    for (PageKey key : {PageKey{0, 1}, PageKey{1, 1}}) {
        auto warm = pool->fetch_page(key); check(warm.value.has_value()); warm.value->release();
    }
    BufferPoolResult<PageGuard> first, second;
    std::thread first_reader([&] { first = pool->fetch_page({0, 2}); });
    first_gate.wait_until_entered();
    std::thread second_reader([&] { second = pool->fetch_page({1, 2}); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool second_entered = false;
    {
        std::unique_lock lock(second_gate.mutex);
        second_entered = second_gate.changed.wait_until(lock, deadline, [&] { return second_gate.entered; });
    }
    first_gate.release(); second_gate.release();
    first_reader.join(); second_reader.join();
    check(second_entered && first.value.has_value() && second.value.has_value());
    first.value->release(); second.value->release();
    check(!pool->close());
    pool.reset(); files.reset();
    std::error_code ignored; std::filesystem::remove_all(path, ignored);
}

void reserved_clean_victim_is_not_a_second_miss_candidate() {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("tinydbms-buffer-pool-clean-reserved-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(std::filesystem::create_directory(path));
    auto opened = FileManager::open(path); check(opened.value.has_value());
    auto files = std::move(*opened.value);
    auto created = files->create_table_file(0); check(created.value.has_value());
    for (PageId page = 1; page <= 3; ++page) check((*created.value)->allocate_page().value == page);
    ReadGate gate;
    auto read = [&](PageKey key) -> PageFileResult<RawPage> {
        if (key.page_id == 2) gate.enter_and_wait();
        return files->find_table_file(key.table_id)->read_page(key.page_id);
    };
    auto made = BufferPool::create(*files, 1, read); check(made.value.has_value());
    auto pool = std::move(*made.value);
    auto warm = pool->fetch_page({0, 1}); check(warm.value.has_value()); warm.value->release();
    BufferPoolResult<PageGuard> first;
    std::thread creator([&] { first = pool->fetch_page({0, 2}); });
    gate.wait_until_entered();
    const auto second = pool->fetch_page({0, 3});
    check(!second.value && second.error && second.error->kind == BufferPoolErrorKind::kNoVictim);
    gate.release(); creator.join();
    check(first.value.has_value()); first.value->release();
    check(!pool->close());
    pool.reset(); files.reset();
    std::error_code ignored; std::filesystem::remove_all(path, ignored);
}

void clean_victim_success_keeps_fifo_and_lru_commit_semantics() {
    for (ReplacementPolicy policy : {ReplacementPolicy::kFifo, ReplacementPolicy::kLru}) {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("tinydbms-buffer-pool-clean-policy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        check(std::filesystem::create_directory(path));
        auto opened = FileManager::open(path); check(opened.value.has_value());
        auto files = std::move(*opened.value);
        auto created = files->create_table_file(0); check(created.value.has_value());
        for (PageId page = 1; page <= 3; ++page) check((*created.value)->allocate_page().value == page);
        auto made = BufferPool::create(*files, 2, {}, {}, policy); check(made.value.has_value());
        auto pool = std::move(*made.value);
        for (PageId page : {1U, 2U}) {
            auto warm = pool->fetch_page({0, page}); check(warm.value.has_value()); warm.value->release();
        }
        if (policy == ReplacementPolicy::kLru) {
            auto renew = pool->fetch_page({0, 1}); check(renew.value.has_value()); renew.value->release();
        }
        auto replacement = pool->fetch_page({0, 3}); check(replacement.value.has_value()); replacement.value->release();
        const PageKey evicted = policy == ReplacementPolicy::kFifo ? PageKey{0, 1} : PageKey{0, 2};
        check(!BufferPoolTestAccess::table(*pool).contains(evicted) &&
              BufferPoolTestAccess::table(*pool).contains({0, 3}));
        const FrameId new_frame = BufferPoolTestAccess::table(*pool).at({0, 3});
        check(BufferPoolTestAccess::order(*pool).back() == new_frame);
        const auto stats = pool->stats();
        check(stats.clean_victim_selection_count == 1 && stats.clean_replacement_commit_count == 1 &&
              stats.clean_replacement_cancel_count == 0);
        check(!pool->close());
        pool.reset(); files.reset();
        std::error_code ignored; std::filesystem::remove_all(path, ignored);
    }
}
} // namespace

int main() {
    try {
        concurrent_resident_hits_and_stats();
        concurrent_different_resident_pages();
        concurrent_distinct_dirty_release();
        close_lifecycle_and_retry();
        for (unsigned iteration = 0; iteration < 20; ++iteration)
            same_cold_miss_uses_one_read_and_shared_completion();
        same_cold_miss_failure_is_shared_and_retryable();
        distinct_free_frame_reads_overlap_outside_metadata_lock();
        loading_does_not_block_ready_hit_close_or_victim_selection();
        clean_victim_read_does_not_block_unrelated_ready_hit();
        clean_victim_same_new_key_shares_one_completion();
        clean_victim_old_hit_cancels_and_blocks_close_release();
        clean_victim_read_failure_preserves_replacement_order();
        clean_victim_reads_on_distinct_files_overlap();
        reserved_clean_victim_is_not_a_second_miss_candidate();
        clean_victim_success_keeps_fifo_and_lru_commit_semantics();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
    return 0;
}
