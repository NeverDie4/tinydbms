#include "heap_table.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <source_location>
#include <stdexcept>
#include <thread>

namespace tinydbms::storage::internal {
struct PageFileTestAccess {
    static void gate(PageFile& file, std::function<void()> callback) {
        std::lock_guard lock(file.mutex_);
        file.before_raw_read_for_testing_ = std::move(callback);
    }
};
struct BufferPoolTestAccess {
    static std::uint64_t submission_drops(BufferPool& p) { return p.prefetch_submission_dropped_.load(); }
    static bool idle(BufferPool& p) {
        std::lock_guard lock(p.prefetch_mutex_);
        return p.prefetch_queue_.empty() && !p.prefetch_active_;
    }
    static bool stopping(BufferPool& p) {
        std::lock_guard lock(p.prefetch_mutex_);
        return p.worker_state_ != decltype(p.worker_state_)::kRunning;
    }
    static std::size_t queued(BufferPool& p) {
        std::lock_guard lock(p.prefetch_mutex_); return p.prefetch_queue_.size();
    }
    static unsigned waiters(BufferPool& p, PageKey key) {
        std::lock_guard lock(p.metadata_mutex_);
        if (auto it = p.loading_table_.find(key); it != p.loading_table_.end())
            return it->second->completion->waiter_count.load();
        if (auto it = p.page_table_.find(key); it != p.page_table_.end()) {
            const auto& f = p.frames_[it->second];
            if (f.completion) return f.completion->waiter_count.load();
        }
        return 0;
    }
    static bool resident(BufferPool& p, PageKey key) {
        std::lock_guard lock(p.metadata_mutex_);
        auto it = p.page_table_.find(key);
        return it != p.page_table_.end() && p.frames_[it->second].state == FrameState::kReady;
    }
};
}

namespace {
using namespace tinydbms;
using namespace tinydbms::storage;
using namespace tinydbms::storage::internal;
void check(bool ok, std::source_location at = std::source_location::current()) {
    if (!ok) throw std::runtime_error("prefetch contract at line " + std::to_string(at.line()));
}
// Deadlines are failure watchdogs. Success requires observing the actual state.
template<class Predicate> bool await(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::yield();
    }
    return true;
}
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false;
    void block() {
        std::unique_lock lock(mutex); entered = true; cv.notify_all();
        cv.wait(lock, [&] { return released; });
    }
    void wait() {
        std::unique_lock lock(mutex);
        check(cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }));
    }
    void release() { std::lock_guard lock(mutex); released = true; cv.notify_all(); }
    ~Gate() { release(); }
};
struct Fixture {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("tinydbms-prefetch-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::unique_ptr<FileManager> files;
    std::unique_ptr<BufferPool> pool;
    Fixture(unsigned pages = 6) {
        check(std::filesystem::create_directory(path));
        auto f = FileManager::open(path); check(f.value.has_value()); files = std::move(*f.value);
        auto t = files->create_table_file(0); check(t.value.has_value());
        for (unsigned i = 1; i <= pages; ++i) check((*t.value)->allocate_page().value == i);
    }
    ~Fixture() {
        pool.reset(); files.reset();
        std::error_code ignored; std::filesystem::remove_all(path, ignored);
    }
    PageFile& file() { return *files->find_table_file(0); }
    void make(unsigned capacity, BufferPool::ReadPage read = {}, BufferPool::WritePage write = {},
              ReplacementPolicy policy = ReplacementPolicy::kFifo, bool enabled = true) {
        if (pool) { check(!pool->close()); pool.reset(); }
        auto p = BufferPool::create(*files, capacity, std::move(read), std::move(write), policy, {}, enabled);
        check(p.value.has_value()); pool = std::move(*p.value);
    }
    void warm(PageId id, bool dirty = false) {
        auto g = pool->fetch_page({0,id}); check(g.value.has_value());
        if (dirty) g.value->mark_dirty();
    }
    void submit(PageId id) {
        check(await([&] {
            const auto before = BufferPoolTestAccess::submission_drops(*pool);
            pool->prefetch_page({0,id});
            return BufferPoolTestAccess::submission_drops(*pool) == before;
        }));
    }
    void idle() { check(await([&] { return BufferPoolTestAccess::idle(*pool); })); }
};

void free_frame_overlap_and_same_key(bool fail) {
    Fixture f; Gate gate; std::atomic<unsigned> reads = 0;
    f.make(2, [&](PageKey k) -> PageFileResult<RawPage> {
        if (k.page_id == 2 && reads.fetch_add(1) == 0) {
            gate.block();
            if (fail) return {std::nullopt, PageFileError{PageFileErrorKind::kIo,"prefetch injected failure"}};
        }
        return f.file().read_page(k.page_id);
    });
    auto pinned = f.pool->fetch_page({0,1}); check(pinned.value.has_value());
    const auto before = f.pool->stats();
    f.submit(2); gate.wait();
    // Reading current N while N+1 is inside the read adapter proves CPU/I/O overlap.
    check(pinned.value->page().bytes[0] == std::byte{0});
    auto hit = f.pool->fetch_page({0,1}); check(hit.value.has_value()); hit.value->release();
    BufferPoolResult<PageGuard> result;
    std::thread demand([&] { result = f.pool->fetch_page({0,2}); });
    const bool waiting = await([&] { return BufferPoolTestAccess::waiters(*f.pool,{0,2}) == 1; });
    const auto during = reads.load(); gate.release(); demand.join();
    check(waiting && during == 1 && result.value.has_value()); result.value->release();
    pinned.value->release(); f.idle();
    auto stats = f.pool->stats();
    check(reads == (fail ? 2u : 1u));
    check(stats.fetch_count == before.fetch_count + 2 && stats.miss_count == before.miss_count + 1);
    check(stats.foreground_wait_for_prefetch == 1 && stats.prefetch_read_failure == (fail ? 1u : 0u));
    check(stats.useful_prefetch == (fail ? 0u : 1u) && stats.prefetch_hit == 0);
    check(!f.pool->close());
}

// Gate inside PageFile's mutex and measure its actual stream counters. A
// truncated fixture injects one physical short read; the adapter repairs only
// that test fixture before returning the error, allowing demand to retry.
void physical_read_gate_and_retry(bool clean_victim, bool fail) {
    Fixture f(2); Gate gate; std::atomic<unsigned> attempts = 0;
    PageFile* file = &f.file();
    f.make(clean_victim ? 1u : 2u, [&](PageKey key) {
        auto result = file->read_page(key.page_id);
        if (key.page_id == 2 && result.error && fail)
            check(!file->write_page(2, RawPage{}));
        return result;
    });
    f.warm(1);
    if (fail) std::filesystem::resize_file(f.files->table_file_path(0), 2 * kPageSize);
    const auto physical_before = file->stats();
    const auto api_before = f.pool->stats();
    PageFileTestAccess::gate(*file, [&] { if (attempts.fetch_add(1) == 0) gate.block(); });
    f.submit(2); gate.wait();
    // Metadata access succeeds while the actual PageFile read owns its mutex.
    check(f.pool->stats().prefetch_started == 1);
    if (!clean_victim) {
        auto current = f.pool->fetch_page({0,1});
        check(current.value && current.value->page().bytes[0] == std::byte{0});
    }
    const auto demand_before = f.pool->stats();
    BufferPoolResult<PageGuard> result;
    std::thread demand([&] { result = f.pool->fetch_page({0,2}); });
    const bool waiting = await([&] { return BufferPoolTestAccess::waiters(*f.pool,{0,2}) == 1; });
    gate.release(); demand.join();
    check(waiting && result.value.has_value()); result.value->release(); f.idle();
    const auto physical_after = file->stats();
    const auto stats = f.pool->stats();
    check(stats.fetch_count == demand_before.fetch_count + 1);
    check(stats.miss_count == api_before.miss_count + 1);
    check(stats.foreground_wait_for_prefetch == 1 && stats.prefetch_hit == 0);
    check(physical_after.physical_read_attempts - physical_before.physical_read_attempts == (fail ? 2u : 1u));
    check(physical_after.physical_read_failures - physical_before.physical_read_failures == (fail ? 1u : 0u));
    check(stats.prefetch_read_failure == (fail ? 1u : 0u));
    PageFileTestAccess::gate(*file, {});
    check(!f.pool->close());
}

void ready_hit_and_unused_eviction() {
    Fixture f; f.make(1);
    f.submit(1); f.idle();
    check(f.pool->stats().fetch_count == 0 && f.pool->stats().prefetch_ready == 1);
    f.warm(1); f.warm(1);
    check(f.pool->stats().prefetch_hit == 1 && f.pool->stats().useful_prefetch == 1);
    f.submit(2); f.idle();
    check(f.pool->stats().unused_prefetch_evicted == 0);
    f.warm(3);
    check(f.pool->stats().unused_prefetch_evicted == 1);
    check(!f.pool->close());
}

void bounded_queue_and_dedup() {
    Fixture f; Gate gate; std::atomic<unsigned> reads = 0;
    f.make(6,[&](PageKey k) { ++reads; if (k.page_id == 1) gate.block(); return f.file().read_page(k.page_id); });
    f.submit(1); gate.wait();
    const auto initial = f.pool->stats();
    f.pool->prefetch_page({0,1});
    f.pool->prefetch_page({0,2}); f.pool->prefetch_page({0,2});
    f.pool->prefetch_page({0,3}); f.pool->prefetch_page({0,4});
    const auto queued = BufferPoolTestAccess::queued(*f.pool);
    const auto stats = f.pool->stats();
    gate.release(); f.idle();
    check(queued == 2 && stats.prefetch_requested == initial.prefetch_requested + 5);
    check(stats.prefetch_deduplicated == 2 && stats.prefetch_dropped == initial.prefetch_dropped + 1 && reads == 3);
    check(!f.pool->close());
}

void dirty_forbidden_then_demand() {
    Fixture f; std::atomic<unsigned> reads = 0, writes = 0;
    f.make(1,[&](PageKey k) { ++reads; return f.file().read_page(k.page_id); },
        [&](PageKey k,const RawPage& page) { ++writes; return f.file().write_page(k.page_id,page); });
    f.warm(1,true); const auto before = f.pool->stats(); f.submit(2); f.idle();
    check(reads == 1 && writes == 0 && BufferPoolTestAccess::resident(*f.pool,{0,1}));
    check(f.pool->stats().dirty_flush_count == 0);
    check(f.pool->stats().prefetch_dropped >= before.prefetch_dropped + 1);
    f.warm(2); check(reads == 2 && writes == 1);
    check(!f.pool->close());
}

void pinned_victim_forbidden() {
    Fixture f; std::atomic<unsigned> reads = 0;
    f.make(1,[&](PageKey k) { ++reads; return f.file().read_page(k.page_id); });
    auto guard = f.pool->fetch_page({0,1}); check(guard.value.has_value());
    f.submit(2); f.idle();
    check(reads == 1 && guard.value->page().bytes[0] == std::byte{0});
    check(BufferPoolTestAccess::resident(*f.pool,{0,1}));
    guard.value->release(); f.warm(2); check(reads == 2); check(!f.pool->close());
}

void clean_victim(ReplacementPolicy policy, bool touch) {
    Fixture f; Gate gate;
    f.make(1,[&](PageKey k) { if (k.page_id == 2) gate.block(); return f.file().read_page(k.page_id); },{},policy);
    f.warm(1); f.submit(2); gate.wait();
    if (touch) f.warm(1);
    gate.release(); f.idle();
    check(BufferPoolTestAccess::resident(*f.pool,{0,touch ? 1u : 2u}));
    check(!BufferPoolTestAccess::resident(*f.pool,{0,touch ? 2u : 1u}));
    check(f.pool->stats().prefetch_ready == (touch ? 0u : 1u));
    check(!f.pool->close());
}

void replacement_order(ReplacementPolicy policy) {
    Fixture f; f.make(2, {}, {}, policy);
    f.warm(1); f.warm(2); f.warm(1);
    f.submit(3); f.idle();
    check(BufferPoolTestAccess::resident(*f.pool,{0,3}));
    check(BufferPoolTestAccess::resident(*f.pool,{0,policy == ReplacementPolicy::kFifo ? 2u : 1u}));
    check(!BufferPoolTestAccess::resident(*f.pool,{0,policy == ReplacementPolicy::kFifo ? 1u : 2u}));
    check(!f.pool->close());
}

void demand_origin_dedup(bool replacement) {
    Fixture f; Gate gate; std::atomic<unsigned> reads = 0;
    f.make(replacement ? 1u : 2u, [&](PageKey key) {
        if (key.page_id == 2) { ++reads; gate.block(); }
        return f.file().read_page(key.page_id);
    });
    if (replacement) f.warm(1);
    BufferPoolResult<PageGuard> result;
    std::thread demand([&] { result = f.pool->fetch_page({0,2}); });
    gate.wait();
    f.submit(2); f.idle();
    const auto stats = f.pool->stats();
    gate.release(); demand.join();
    check(result.value && reads == 1 && stats.prefetch_started == 0 && stats.prefetch_deduplicated == 1);
    result.value->release();
    f.submit(2); f.idle(); // READY lookup dedup, independent of the queue.
    check(reads == 1 && f.pool->stats().prefetch_deduplicated == 2);
    check(!f.pool->close());
}

void close_inflight_and_queued(bool destroy) {
    Fixture f; Gate gate; std::atomic<unsigned> reads = 0; std::atomic<bool> done = false;
    f.make(3,[&](PageKey k) { ++reads; gate.block(); return f.file().read_page(k.page_id); });
    f.submit(1); gate.wait(); f.pool->prefetch_page({0,2});
    BufferPool* raw = f.pool.get(); std::optional<BufferPoolError> error;
    std::thread closer([&] { if (destroy) f.pool.reset(); else error = f.pool->close(); done = true; });
    // The gated adapter keeps the object alive until shutdown has entered join.
    const bool stopping = await([&] { return BufferPoolTestAccess::stopping(*raw); });
    const bool blocked = !done.load(); gate.release(); closer.join();
    check(stopping && blocked && !error && reads == 1 && done);
    if (!destroy) check(f.pool->fetch_page({0,1}).error.has_value());
}

void busy_close_restart_and_free_drop() {
    Fixture f; f.make(2);
    auto pinned = f.pool->fetch_page({0,1}); check(pinned.value.has_value());
    check(f.pool->close().has_value());
    f.submit(2); f.idle();
    check(f.pool->stats().prefetch_ready == 1); pinned.value->release();
    check(!f.file().free_page(3));
    f.submit(3); f.idle();
    check(f.pool->stats().prefetch_free_page_drop == 1);
    check(!BufferPoolTestAccess::resident(*f.pool,{0,3}));
    check(!f.pool->close());
}

void heap_scan_equivalence() {
    Fixture f(0); f.make(3,{}, {},ReplacementPolicy::kFifo,false);
    TableMeta meta{0,"prefetch_scan",{{"s",Type::kVarchar}}};
    HeapTable writer(meta,*f.files,*f.pool);
    for (unsigned i = 0; i < 18; ++i) {
        auto r = writer.insert_record({Value{std::string(1024,static_cast<char>('a' + i))}});
        check(r.value.has_value());
    }
    check(!f.pool->close()); f.pool.reset();
    check(!f.file().free_page(2)); // Gap prepared only after all cached pages were released.
    std::vector<std::pair<std::uint64_t,std::string>> expected;
    HeapScanPosition expected_end;
    for (bool enabled : {false,true}) {
        f.make(3,{}, {},ReplacementPolicy::kFifo,enabled);
        HeapTable reader(meta,*f.files,*f.pool);
        auto start = reader.begin_scan(); check(start.value.has_value()); auto position = *start.value;
        std::vector<std::pair<std::uint64_t,std::string>> rows;
        while (true) {
            auto result = reader.next_record(position); check(!result.error);
            if (!result.value) break;
            rows.emplace_back(result.value->rid.value,std::get<std::string>(result.value->values[0].data));
        }
        f.idle();
        if (!enabled) { expected = rows; expected_end = position; check(f.pool->stats().prefetch_started == 0); }
        else { check(rows == expected && position == expected_end); check(f.pool->stats().prefetch_requested > 0); }
        check(rows.size() == 15);
        auto eof = reader.next_record(position); check(!eof.error && !eof.value && position == expected_end);
        check(!f.pool->close());
    }
}

void heap_captured_boundary() {
    Fixture f(0); f.make(3,{}, {},ReplacementPolicy::kFifo,false);
    TableMeta meta{0,"prefetch_boundary",{{"s",Type::kVarchar}}};
    HeapTable writer(meta,*f.files,*f.pool);
    for (unsigned i = 0; i < 6; ++i) check(writer.insert_record({Value{std::string(1024,'x')}}).value.has_value());
    const auto boundary = f.file().page_count();
    std::atomic<bool> outside = false;
    f.make(3,[&](PageKey key) {
        if (key.page_id >= boundary) outside = true;
        return f.file().read_page(key.page_id);
    });
    HeapTable reader(meta,*f.files,*f.pool);
    auto start = reader.begin_scan(); check(start.value.has_value()); auto position = *start.value;
    check(position.page_end_exclusive == boundary);
    check(f.file().allocate_page().value == boundary); // Deliberately invalid record page beyond captured EOF.
    unsigned count = 0;
    while (true) {
        auto result = reader.next_record(position); check(!result.error);
        if (!result.value) break;
        ++count;
    }
    f.idle(); check(count == 6 && !outside && position.page_end_exclusive == boundary);
    check(!f.pool->close());
}
}

int main() {
    try {
        free_frame_overlap_and_same_key(false); free_frame_overlap_and_same_key(true);
        for (bool clean : {false,true}) for (bool fail : {false,true}) physical_read_gate_and_retry(clean,fail);
        ready_hit_and_unused_eviction(); bounded_queue_and_dedup(); dirty_forbidden_then_demand(); pinned_victim_forbidden();
        for (auto p : {ReplacementPolicy::kFifo,ReplacementPolicy::kLru}) {
            clean_victim(p,false); clean_victim(p,true);
            replacement_order(p);
        }
        demand_origin_dedup(false); demand_origin_dedup(true);
        close_inflight_and_queued(false); close_inflight_and_queued(true);
        busy_close_restart_and_free_drop(); heap_scan_equivalence(); heap_captured_boundary();
        std::cout << "PREF-1 deterministic contracts passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

