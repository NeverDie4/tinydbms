#pragma once

#include "file_manager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tinydbms::storage::internal {

struct PageKey {
    TableId table_id;
    PageId page_id;
    bool operator==(const PageKey&) const = default;
};
struct PageKeyHash {
    std::size_t operator()(PageKey key) const noexcept;
};
using FrameId = std::size_t;
enum class ReplacementPolicy { kFifo, kLru };
enum class LifecycleState { kOpen, kClosing, kClosed };
enum class FrameState { kFree, kLoading, kReady };

// Only FrameId order; eligibility is supplied by BufferPool, never stored here.
class FifoReplacer {
public:
    explicit FifoReplacer(std::size_t capacity);
    void record_load(FrameId id) noexcept;
    void remove(FrameId id) noexcept;
    void clear() noexcept;
    template <typename Eligible>
    std::optional<FrameId> choose_victim(Eligible eligible) const {
        for (FrameId id : order_) if (eligible(id)) return id;
        return std::nullopt;
    }
private:
    friend struct BufferPoolTestAccess;
    std::vector<FrameId> order_;
};

enum class BufferPoolErrorKind { kInvalidArgument, kNoVictim, kIo, kCorrupt };
struct BufferPoolError {
    BufferPoolErrorKind kind;
    std::string message;
};
template <typename T> struct BufferPoolResult {
    std::optional<T> value;
    std::optional<BufferPoolError> error;
};
enum class LoadOrigin { kDemand, kPrefetch };
enum class PrefetchWorkerState { kRunning, kStopping, kStopped };
struct LoadCompletion {
    LoadOrigin origin = LoadOrigin::kDemand;
    bool foreground_arrived = false;
    std::uint64_t ticket = 0;
    bool done = false;
    std::optional<BufferPoolError> error;
    std::condition_variable cv;
    std::atomic<std::uint32_t> waiter_count = 0;
};
struct LoadReservation {
    FrameId victim_frame = 0;
    PageKey old_key{};
    std::uint64_t replacement_ticket = 0;
    std::uint64_t access_generation = 0;
    std::shared_ptr<LoadCompletion> completion;
};
struct Frame {
    bool prefetched_ready = false;
    bool prefetch_had_foreground = false;
    RawPage page;
    std::optional<PageKey> key;
    bool dirty = false;
    std::uint32_t pin_count = 0;
    FrameState state = FrameState::kFree;
    std::uint64_t ticket = 0;
    std::shared_ptr<LoadCompletion> completion;
    bool replacement_reserved = false;
    std::uint64_t replacement_ticket = 0;
    std::uint64_t access_generation = 0;
    bool try_pin() noexcept;
    bool try_unpin() noexcept;
};
struct BufferPoolStats {
    std::uint64_t prefetch_requested = 0;
    std::uint64_t prefetch_deduplicated = 0;
    std::uint64_t prefetch_dropped = 0;
    std::uint64_t prefetch_started = 0;
    std::uint64_t prefetch_read_success = 0;
    std::uint64_t prefetch_read_failure = 0;
    std::uint64_t prefetch_ready = 0;
    std::uint64_t prefetch_hit = 0;
    std::uint64_t useful_prefetch = 0;
    std::uint64_t unused_prefetch_evicted = 0;
    std::uint64_t foreground_wait_for_prefetch = 0;
    std::uint64_t prefetch_free_page_drop = 0;
    std::uint64_t fetch_count = 0;
    std::uint64_t hit_count = 0;
    std::uint64_t miss_count = 0;
    std::uint64_t dirty_flush_count = 0;
    std::uint64_t eviction_count = 0;
    std::uint64_t free_frame_miss_count = 0;
    std::uint64_t clean_victim_selection_count = 0;
    std::uint64_t clean_replacement_commit_count = 0;
    std::uint64_t clean_replacement_cancel_count = 0;
    std::uint64_t dirty_victim_selection_count = 0;
    std::uint64_t dirty_requested_read_count = 0;
    std::uint64_t dirty_write_attempt_count = 0;
    std::uint64_t dirty_write_success_count = 0;
    std::uint64_t dirty_replacement_commit_count = 0;
    std::uint64_t dirty_replacement_cancel_count = 0;
    double hit_rate() const noexcept;
};
// Benchmark-only snapshot. Frame ids are valid only until a later mutating
// pool operation; snapshot construction itself is metadata-synchronized.
struct DirtyFrameSnapshotEntry {
    FrameId frame_id;
    PageKey key;
};
struct DirtyFrameFlushResult {
    std::chrono::nanoseconds flush_io_time{};
};

class BufferPool;
class PageGuard {
public:
    PageGuard() noexcept = default;
    ~PageGuard() noexcept;
    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;
    PageGuard(PageGuard&& other) noexcept;
    PageGuard& operator=(PageGuard&& other) noexcept;

    bool valid() const noexcept;
    // References expire on release/destruction. Invalid access throws logic_error.
    const RawPage& page() const;
    RawPage& mutable_page();
    void mark_dirty();
    std::uint32_t pin_count() const; // Read-only pin diagnostics, not a frame handle.
    void release() noexcept;

private:
    friend class BufferPool;
    PageGuard(BufferPool& pool, FrameId frame) noexcept;
    BufferPool* pool_ = nullptr;
    FrameId frame_ = 0;
};

class BufferPool {
public:
    // Narrow synchronous adapters obey PageFile result semantics and must not
    // reenter the pool or mutate its state. Empty adapters delegate to PageFile.
    using ReadPage = std::function<PageFileResult<RawPage>(PageKey)>;
    using WritePage = std::function<std::optional<PageFileError>(PageKey, const RawPage&)>;
    // Empty sink disables logging. Sink must not reenter/mutate the pool.
    using LogSink = std::function<void(std::string_view)>;
    static BufferPoolResult<std::unique_ptr<BufferPool>> create(
        FileManager& files, std::size_t capacity, ReadPage read = {}, WritePage write = {},
        ReplacementPolicy policy = ReplacementPolicy::kFifo, LogSink log = {},
        bool prefetch_enabled = false);
    // All guards must be released first. Destruction with outstanding pins is a
    // programmer lifetime violation (terminate); close() instead reports an error.
    // Destructor joins advisory work; explicit successful close persists dirty pages.
    ~BufferPool() noexcept;
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    BufferPool(BufferPool&&) = delete;
    BufferPool& operator=(BufferPool&&) = delete;

    BufferPoolResult<PageGuard> fetch_page(PageKey key);
    // Storage-private advisory submission: never waits for queue space or I/O.
    void prefetch_page(PageKey key) noexcept;
    std::optional<BufferPoolError> flush_page(PageKey key);
    std::optional<BufferPoolError> flush_all();
    BufferPoolResult<std::vector<DirtyFrameSnapshotEntry>> snapshot_dirty_frames_for_experiment() const;
    BufferPoolResult<DirtyFrameFlushResult> flush_dirty_frames_for_experiment(
        const std::vector<DirtyFrameSnapshotEntry>& snapshot);
    std::optional<BufferPoolError> release_table(TableId table_id);
    std::optional<BufferPoolError> close();
    BufferPoolStats stats() const noexcept;

private:
    friend class PageGuard;
    friend struct BufferPoolTestAccess; // Read-only invariant inspection in tests.
    BufferPool(FileManager& files, std::size_t capacity, ReadPage read, WritePage write,
               ReplacementPolicy policy, LogSink log, bool prefetch_enabled);
    BufferPoolResult<PageGuard> load_page(PageKey key, LoadOrigin origin);
    void start_prefetch_worker(); // worker_control_mutex_ held after construction
    void stop_prefetch_worker(); // never called with metadata_mutex_ held
    void prefetch_worker_loop() noexcept;
    void consume_prefetch_locked(Frame& frame, bool ready_hit) noexcept;
    void log_event(std::string_view event, PageKey key, std::optional<FrameId> frame = {},
                   std::string_view result = {}) const noexcept;
    bool is_open_locked() const noexcept;
    static void reset_to_free(Frame& frame) noexcept;
    std::optional<BufferPoolError> flush_frame_locked(Frame& frame);
    std::optional<BufferPoolError> flush_all_locked();
    void mark_dirty(FrameId frame) noexcept;
    void unpin(FrameId frame) noexcept;
    void unpin_locked(FrameId frame) noexcept;
    std::uint32_t pin_count(FrameId frame) const;
    FileManager& files_; // Must outlive pool. Files cannot close/reopen while cached.
    std::vector<Frame> frames_; // Sized once; never grows.
    std::unordered_map<PageKey, FrameId, PageKeyHash> page_table_;
    std::unordered_map<PageKey, std::shared_ptr<LoadReservation>, PageKeyHash> loading_table_;
    FifoReplacer fifo_;
    ReplacementPolicy policy_;
    LogSink log_;
    ReadPage read_;
    WritePage write_;
    mutable std::mutex metadata_mutex_;
    BufferPoolStats stats_;
    LifecycleState lifecycle_ = LifecycleState::kOpen;
    const bool prefetch_enabled_;
    std::mutex worker_control_mutex_; // serializes stop/start and close/release_table
    std::mutex prefetch_mutex_;
    std::condition_variable prefetch_cv_;
    std::vector<PageKey> prefetch_queue_; // capacity fixed at two; no submission allocation
    std::thread prefetch_worker_;
    PrefetchWorkerState worker_state_ = PrefetchWorkerState::kStopped;
    bool prefetch_active_ = false;
    std::optional<PageKey> prefetch_active_key_;
    std::atomic<std::uint64_t> prefetch_requested_{0};
    std::atomic<std::uint64_t> prefetch_submission_dropped_{0};
    std::atomic<std::uint64_t> prefetch_submission_deduplicated_{0};
};

} // namespace tinydbms::storage::internal
