#include "buffer_pool.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace tinydbms::storage::internal {
namespace {
BufferPoolError invalid(std::string message) {
    return {BufferPoolErrorKind::kInvalidArgument, std::move(message)};
}
BufferPoolError map_error(const PageFileError& error) {
    switch (error.kind) {
        case PageFileErrorKind::kIo: return {BufferPoolErrorKind::kIo, error.message};
        case PageFileErrorKind::kCorrupt: return {BufferPoolErrorKind::kCorrupt, error.message};
        case PageFileErrorKind::kInvalidArgument: return invalid(error.message);
    }
    return invalid("unknown PageFile error");
}
}

FifoReplacer::FifoReplacer(std::size_t capacity) { order_.reserve(capacity); }
void FifoReplacer::remove(FrameId id) noexcept {
    const auto entry = std::find(order_.begin(), order_.end(), id);
    if (entry != order_.end()) order_.erase(entry);
}
void FifoReplacer::record_load(FrameId id) noexcept {
    remove(id);
    if (order_.size() == order_.capacity()) std::terminate();
    order_.push_back(id); // Capacity reserved at construction; no allocation at commit.
}
void FifoReplacer::clear() noexcept { order_.clear(); }

std::size_t PageKeyHash::operator()(PageKey key) const noexcept {
    constexpr unsigned page_bits = std::numeric_limits<PageId>::digits;
    static_assert(page_bits + std::numeric_limits<TableId>::digits <= 64);
    return std::hash<std::uint64_t>{}((std::uint64_t{key.table_id} << page_bits) | key.page_id);
}
bool Frame::try_pin() noexcept {
    if (pin_count == std::numeric_limits<std::uint32_t>::max()) return false;
    ++pin_count;
    return true;
}
bool Frame::try_unpin() noexcept {
    if (pin_count == 0) return false;
    --pin_count;
    return true;
}
double BufferPoolStats::hit_rate() const noexcept {
    return fetch_count == 0 ? 0.0 : static_cast<double>(hit_count) / static_cast<double>(fetch_count);
}

PageGuard::PageGuard(BufferPool& pool, FrameId frame) noexcept : pool_(&pool), frame_(frame) {}
PageGuard::~PageGuard() noexcept { release(); }
PageGuard::PageGuard(PageGuard&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)), frame_(other.frame_) {}
PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
    if (this != &other) {
        release();
        pool_ = std::exchange(other.pool_, nullptr);
        frame_ = other.frame_;
    }
    return *this;
}
bool PageGuard::valid() const noexcept { return pool_ != nullptr; }
const RawPage& PageGuard::page() const {
    if (!valid()) throw std::logic_error("access to released PageGuard");
    return pool_->frames_[frame_].page;
}
std::uint32_t PageGuard::pin_count() const {
    if (!valid()) throw std::logic_error("access to released PageGuard");
    return pool_->pin_count(frame_);
}
void PageGuard::release() noexcept {
    if (auto* pool = std::exchange(pool_, nullptr)) pool->unpin(frame_);
}

BufferPool::BufferPool(FileManager& files, std::size_t capacity, ReadPage read, WritePage write,
                       ReplacementPolicy policy, LogSink log, bool prefetch_enabled)
    : files_(files), frames_(capacity), fifo_(capacity), policy_(policy), log_(std::move(log)),
      read_(std::move(read)), write_(std::move(write)), prefetch_enabled_(prefetch_enabled) {
    // One extra entry permits node-handle insertion before removing the old key.
    page_table_.reserve(capacity + 1);
    loading_table_.reserve(capacity);
    prefetch_queue_.reserve(2);
    start_prefetch_worker();
}
BufferPoolResult<std::unique_ptr<BufferPool>> BufferPool::create(
    FileManager& files, std::size_t capacity, ReadPage read, WritePage write,
    ReplacementPolicy policy, LogSink log, bool prefetch_enabled) {
    if (capacity == 0 || capacity == std::numeric_limits<std::size_t>::max())
        return {std::nullopt, invalid("BufferPool capacity is not representable or is zero")};
    if (policy != ReplacementPolicy::kFifo && policy != ReplacementPolicy::kLru)
        return {std::nullopt, invalid("unknown replacement policy")};
    return {std::unique_ptr<BufferPool>(new BufferPool(files, capacity, std::move(read), std::move(write), policy, std::move(log), prefetch_enabled)), std::nullopt};
}
void BufferPool::log_event(std::string_view event, PageKey key, std::optional<FrameId> frame,
                           std::string_view result) const noexcept {
    if (!log_) return;
    try {
        std::string message = std::string(event) + " table=" + std::to_string(key.table_id) +
            " page=" + std::to_string(key.page_id);
        if (frame) message += " frame=" + std::to_string(*frame);
        message += policy_ == ReplacementPolicy::kFifo ? " policy=FIFO" : " policy=LRU";
        if (!result.empty()) message += " result=" + std::string(result);
        log_(message);
    } catch (...) { /* Diagnostics must never alter storage success/failure semantics. */ }
}
void BufferPool::start_prefetch_worker() {
    if (!prefetch_enabled_) return;
    std::lock_guard lock(prefetch_mutex_);
    if (worker_state_ != PrefetchWorkerState::kStopped) return;
    // A resource shortage disables only advisory work. A retryable close can
    // retry thread creation; demand operations retain their normal semantics.
    try {
        prefetch_worker_ = std::thread([this] { prefetch_worker_loop(); });
        worker_state_ = PrefetchWorkerState::kRunning;
    } catch (const std::system_error&) {
        worker_state_ = PrefetchWorkerState::kStopped;
    }
}
void BufferPool::stop_prefetch_worker() {
    {
        std::lock_guard lock(prefetch_mutex_);
        if (worker_state_ == PrefetchWorkerState::kRunning)
            worker_state_ = PrefetchWorkerState::kStopping;
        prefetch_submission_dropped_.fetch_add(prefetch_queue_.size(), std::memory_order_relaxed);
        prefetch_queue_.clear();
    }
    prefetch_cv_.notify_all();
    if (prefetch_worker_.joinable()) prefetch_worker_.join();
    std::lock_guard lock(prefetch_mutex_);
    worker_state_ = PrefetchWorkerState::kStopped;
}
void BufferPool::prefetch_page(PageKey key) noexcept {
    prefetch_requested_.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lock(prefetch_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || worker_state_ != PrefetchWorkerState::kRunning || key.page_id == 0) {
        prefetch_submission_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (prefetch_active_key_ == key ||
        std::find(prefetch_queue_.begin(), prefetch_queue_.end(), key) != prefetch_queue_.end()) {
        prefetch_submission_deduplicated_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (prefetch_queue_.size() == 2) {
        prefetch_submission_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    prefetch_queue_.push_back(key);
    lock.unlock();
    prefetch_cv_.notify_one();
}
void BufferPool::prefetch_worker_loop() noexcept {
    for (;;) {
        PageKey key{};
        {
            std::unique_lock lock(prefetch_mutex_);
            prefetch_cv_.wait(lock, [this] {
                return worker_state_ != PrefetchWorkerState::kRunning || !prefetch_queue_.empty();
            });
            if (worker_state_ != PrefetchWorkerState::kRunning) break;
            key = prefetch_queue_.front();
            prefetch_queue_.erase(prefetch_queue_.begin());
            prefetch_active_ = true;
            prefetch_active_key_ = key;
        }
        // File lifetime and allocation traversal are outside metadata_mutex_.
        // No frame reservation exists for FREE or inaccessible candidates.
        auto lease = files_.acquire_file(key.table_id);
        bool allocated = false;
        bool free_page = false;
        if (lease.value) {
            auto allocation = (**lease.value).page_allocation_state(key.page_id);
            allocated = allocation.value && *allocation.value == PageAllocationState::kAllocated;
            free_page = allocation.value && *allocation.value == PageAllocationState::kFree;
        }
        if (allocated) {
            auto result = load_page(key, LoadOrigin::kPrefetch);
            if (result.error) {
                std::lock_guard lock(metadata_mutex_);
                ++stats_.prefetch_dropped;
            }
        } else {
            std::lock_guard lock(metadata_mutex_);
            ++stats_.prefetch_dropped;
            if (free_page) ++stats_.prefetch_free_page_drop;
        }
        // Release the allocation lease before advertising idle or exiting.
        lease.value.reset();
        {
            std::lock_guard lock(prefetch_mutex_);
            prefetch_active_ = false;
            prefetch_active_key_.reset();
        }
        prefetch_cv_.notify_all();
    }
}
RawPage& PageGuard::mutable_page() {
    if (!valid()) throw std::logic_error("access to released PageGuard");
    return pool_->frames_[frame_].page;
}
void PageGuard::mark_dirty() {
    if (!valid()) throw std::logic_error("access to released PageGuard");
    pool_->mark_dirty(frame_);
}
bool BufferPool::is_open_locked() const noexcept { return lifecycle_ == LifecycleState::kOpen; }
void BufferPool::reset_to_free(Frame& frame) noexcept {
    frame.page = RawPage{};
    frame.key.reset();
    frame.dirty = false;
    frame.prefetched_ready = false;
    frame.prefetch_had_foreground = false;
    frame.pin_count = 0;
    frame.state = FrameState::kFree;
    frame.completion.reset();
    frame.replacement_reserved = false;
}
std::optional<BufferPoolError> BufferPool::flush_frame_locked(Frame& frame) {
    if (frame.state != FrameState::kReady || !frame.key || !frame.dirty) return std::nullopt;
    if (stats_.dirty_flush_count == std::numeric_limits<std::uint64_t>::max())
        return invalid("BufferPool dirty flush counter exhausted");
    auto lease = files_.acquire_file(frame.key->table_id);
    if (!lease.value || !(**lease.value).is_open()) {
        log_event("Flush dirty", *frame.key, {}, "failure");
        return invalid("table PageFile is not open");
    }
    const auto error = write_ ? write_(*frame.key, frame.page)
                              : (**lease.value).write_page(frame.key->page_id, frame.page);
    if (error) {
        log_event("Flush dirty", *frame.key, {}, "failure");
        return map_error(*error);
    }
    frame.dirty = false;
    ++stats_.dirty_flush_count;
    log_event("Flush dirty", *frame.key, {}, "success");
    return std::nullopt;
}
std::optional<BufferPoolError> BufferPool::flush_page(PageKey key) {
    std::lock_guard lock(metadata_mutex_);
    if (!is_open_locked()) return invalid("BufferPool is closed");
    const auto found = page_table_.find(key);
    if (found == page_table_.end()) return std::nullopt;
    return flush_frame_locked(frames_[found->second]);
}
std::optional<BufferPoolError> BufferPool::flush_all_locked() {
    for (auto& frame : frames_) {
        if (auto error = flush_frame_locked(frame)) return error;
    }
    return std::nullopt;
}
std::optional<BufferPoolError> BufferPool::flush_all() {
    std::lock_guard lock(metadata_mutex_);
    if (!is_open_locked()) return invalid("BufferPool is closed");
    return flush_all_locked();
}
BufferPoolResult<std::vector<DirtyFrameSnapshotEntry>>
BufferPool::snapshot_dirty_frames_for_experiment() const {
    std::lock_guard lock(metadata_mutex_);
    if (!is_open_locked()) return {std::nullopt, invalid("BufferPool is closed")};
    std::vector<DirtyFrameSnapshotEntry> snapshot;
    for (FrameId id = 0; id < frames_.size(); ++id) {
        const Frame& frame = frames_[id];
        if (frame.state != FrameState::kReady) continue;
        if (!frame.dirty) continue;
        if (!frame.key) return {std::nullopt, invalid("dirty Frame has no PageKey")};
        if (frame.pin_count != 0)
            return {std::nullopt, invalid("cannot snapshot pinned dirty Frame")};
        snapshot.push_back({id, *frame.key});
    }
    return {std::move(snapshot), std::nullopt};
}
BufferPoolResult<DirtyFrameFlushResult> BufferPool::flush_dirty_frames_for_experiment(
    const std::vector<DirtyFrameSnapshotEntry>& snapshot) {
    std::lock_guard lock(metadata_mutex_);
    if (!is_open_locked()) return {std::nullopt, invalid("BufferPool is closed")};
    for (const auto& entry : snapshot) {
        if (entry.frame_id >= frames_.size())
            return {std::nullopt, invalid("dirty Frame snapshot is out of range")};
        const Frame& frame = frames_[entry.frame_id];
        if (frame.state != FrameState::kReady || !frame.key || *frame.key != entry.key || !frame.dirty)
            return {std::nullopt, invalid("dirty Frame snapshot is stale")};
        if (frame.pin_count != 0)
            return {std::nullopt, invalid("cannot flush pinned dirty Frame snapshot")};
    }
    DirtyFrameFlushResult result;
    for (const auto& entry : snapshot) {
        const auto started = std::chrono::steady_clock::now();
        if (auto error = flush_frame_locked(frames_[entry.frame_id])) return {std::nullopt, std::move(error)};
        result.flush_io_time += std::chrono::steady_clock::now() - started;
    }
    return {result, std::nullopt};
}
std::optional<BufferPoolError> BufferPool::release_table(TableId table_id) {
    std::lock_guard control(worker_control_mutex_);
    stop_prefetch_worker();
    // Scope exit after the metadata lock releases, including all error paths.
    bool reopen = false;
    const auto restart = [this, &reopen](BufferPool*) { if (reopen) start_prefetch_worker(); };
    std::unique_ptr<BufferPool, decltype(restart)> resume(this, restart);
    std::lock_guard lock(metadata_mutex_);
    if (!is_open_locked()) return invalid("BufferPool is closed");
    reopen = true;
    const auto belongs = [table_id](const Frame& frame) {
        return frame.key && frame.key->table_id == table_id;
    };
    for (const auto& [requested_key, reservation] : loading_table_) {
        if (!reservation || requested_key.table_id == table_id || reservation->old_key.table_id == table_id)
            return invalid("cannot release table with active speculative replacement");
    }
    // Complete prepare pass before performing any I/O or removal.
    for (const auto& frame : frames_) {
        if (belongs(frame) && frame.pin_count != 0)
            return invalid("cannot release table with active PageGuards");
    }
    for (auto& frame : frames_) {
        if (belongs(frame)) {
            if (auto error = flush_frame_locked(frame)) return error;
        }
    }
    // Commit only after every target write succeeded; unrelated frames stay resident.
    for (auto& frame : frames_) {
        if (belongs(frame)) {
            page_table_.erase(*frame.key);
            fifo_.remove(static_cast<FrameId>(&frame - frames_.data()));
            reset_to_free(frame);
        }
    }
    return std::nullopt;
}
BufferPool::~BufferPool() noexcept {
    stop_prefetch_worker();
    // Violating the owner-before-guard lifetime rule must not become a dangling pointer.
    std::lock_guard lock(metadata_mutex_);
    if (!loading_table_.empty() || std::any_of(frames_.begin(), frames_.end(), [](const Frame& f) {
        return f.pin_count != 0 || f.replacement_reserved;
    }))
        std::terminate();
}
void BufferPool::unpin(FrameId frame) noexcept {
    std::lock_guard lock(metadata_mutex_);
    unpin_locked(frame);
}
void BufferPool::unpin_locked(FrameId frame) noexcept {
    if (frame >= frames_.size() || frames_[frame].state != FrameState::kReady ||
        !frames_[frame].key || !frames_[frame].try_unpin())
        std::terminate(); // Internal invariant failure; guard destruction cannot throw.
}
void BufferPool::mark_dirty(FrameId frame) noexcept {
    std::lock_guard lock(metadata_mutex_);
    if (frame >= frames_.size() || frames_[frame].state != FrameState::kReady ||
        !frames_[frame].key || frames_[frame].pin_count == 0)
        std::terminate();
    if (frames_[frame].replacement_reserved) {
        if (frames_[frame].access_generation == std::numeric_limits<std::uint64_t>::max())
            std::terminate();
        ++frames_[frame].access_generation;
    }
    frames_[frame].dirty = true;
}
std::uint32_t BufferPool::pin_count(FrameId frame) const {
    std::lock_guard lock(metadata_mutex_);
    if (frame >= frames_.size() || frames_[frame].state != FrameState::kReady || !frames_[frame].key)
        throw std::logic_error("access to released PageGuard");
    return frames_[frame].pin_count;
}
BufferPoolResult<PageGuard> BufferPool::fetch_page(PageKey key) {
    return load_page(key, LoadOrigin::kDemand);
}
void BufferPool::consume_prefetch_locked(Frame& frame, bool ready_hit) noexcept {
    if (!frame.prefetched_ready) return;
    frame.prefetched_ready = false;
    ++stats_.useful_prefetch;
    if (ready_hit && !frame.prefetch_had_foreground) ++stats_.prefetch_hit;
}
BufferPoolResult<PageGuard> BufferPool::load_page(PageKey key, LoadOrigin origin) {
    const bool prefetch = origin == LoadOrigin::kPrefetch;
    std::unique_lock lock(metadata_mutex_);
    if (!is_open_locked() || key.page_id == 0)
        return {std::nullopt, invalid("pool is closed or PageId is zero")};
    if (!prefetch) {
        if (stats_.fetch_count == std::numeric_limits<std::uint64_t>::max())
            return {std::nullopt, invalid("BufferPool fetch counter exhausted")};
        ++stats_.fetch_count;
    }
    bool classified = false;
    bool waited_prefetch = false;
    // Advisory failure and a pin=0 publication evicted before wakeup both retry
    // here, retaining the original API accounting (never recurse into fetch).
    for (;;) {
        if (!is_open_locked()) return {std::nullopt, invalid("pool is closed")};
        const auto found = page_table_.find(key);
        const auto loading = loading_table_.find(key);
        if (prefetch && (found != page_table_.end() || loading != loading_table_.end())) {
            ++stats_.prefetch_deduplicated;
            return {};
        }
        if (found != page_table_.end() && frames_[found->second].state == FrameState::kReady) {
            Frame& frame = frames_[found->second];
            if (!classified) ++stats_.hit_count;
            if (frame.replacement_reserved) {
                if (frame.access_generation == std::numeric_limits<std::uint64_t>::max())
                    return {std::nullopt, invalid("BufferPool Frame access generation exhausted")};
                ++frame.access_generation;
            }
            if (!frame.try_pin()) return {std::nullopt, invalid("Frame pin count overflow")};
            consume_prefetch_locked(frame, !classified);
            if (policy_ == ReplacementPolicy::kLru) fifo_.record_load(found->second);
            log_event("Buffer HIT", key, found->second);
            return {PageGuard(*this, found->second), std::nullopt};
        }
        if (!prefetch && !classified) { ++stats_.miss_count; classified = true; }
        std::shared_ptr<LoadCompletion> completion;
        if (found != page_table_.end()) {
            Frame& frame = frames_[found->second];
            if (frame.state != FrameState::kLoading || !frame.completion)
                return {std::nullopt, invalid("BufferPool PageTable has invalid Frame state")};
            completion = frame.completion;
        } else if (loading != loading_table_.end()) {
            if (!loading->second || !loading->second->completion)
                return {std::nullopt, invalid("BufferPool loading table has invalid reservation")};
            completion = loading->second->completion;
        }
        if (completion) {
            const bool advisory = completion->origin == LoadOrigin::kPrefetch;
            if (advisory) {
                completion->foreground_arrived = true;
                if (!waited_prefetch) { ++stats_.foreground_wait_for_prefetch; waited_prefetch = true; }
            }
            completion->waiter_count.fetch_add(1, std::memory_order_relaxed);
            completion->cv.wait(lock, [&] { return completion->done; });
            completion->waiter_count.fetch_sub(1, std::memory_order_relaxed);
            if (completion->error) {
                if (advisory) continue;
                return {std::nullopt, *completion->error};
            }
            const auto ready = page_table_.find(key);
            if (ready == page_table_.end() || frames_[ready->second].state != FrameState::kReady ||
                !frames_[ready->second].key || *frames_[ready->second].key != key ||
                frames_[ready->second].ticket != completion->ticket) {
                if (advisory) continue;
                return {std::nullopt, invalid("BufferPool load completion is stale")};
            }
            Frame& published = frames_[ready->second];
            if (published.replacement_reserved) {
                if (published.access_generation == std::numeric_limits<std::uint64_t>::max())
                    return {std::nullopt, invalid("BufferPool Frame access generation exhausted")};
                ++published.access_generation;
            }
            if (!published.try_pin()) return {std::nullopt, invalid("Frame pin count overflow")};
            consume_prefetch_locked(published, false);
            if (policy_ == ReplacementPolicy::kLru) fifo_.record_load(ready->second);
            return {PageGuard(*this, ready->second), std::nullopt};
        }
        log_event("Buffer MISS", key);
        const auto empty = std::find_if(frames_.begin(), frames_.end(),
                                       [](const Frame& frame) { return frame.state == FrameState::kFree; });
        if (empty != frames_.end()) {
            if (!prefetch) ++stats_.free_frame_miss_count;
            const FrameId frame_id = static_cast<FrameId>(empty - frames_.begin());
            auto completion = std::make_shared<LoadCompletion>();
            completion->origin = origin;
            // Allocate the node before changing the Frame: an allocation failure leaves it FREE.
            decltype(page_table_) staging;
            staging.emplace(key, frame_id);
            auto node = staging.extract(key);
            if (frames_[frame_id].ticket == std::numeric_limits<std::uint64_t>::max())
                return {std::nullopt, invalid("BufferPool Frame ticket exhausted")};
            completion->ticket = ++frames_[frame_id].ticket;
            Frame& reserved = frames_[frame_id];
            reserved.key = key;
            reserved.dirty = false;
            reserved.pin_count = 1; // Internal LOADING reservation, never a PageGuard.
            reserved.state = FrameState::kLoading;
            reserved.completion = completion;
            const auto inserted = page_table_.insert(std::move(node));
            if (!inserted.inserted) std::terminate();

            if (prefetch) ++stats_.prefetch_started;
            lock.unlock();
            std::optional<BufferPoolError> load_error;
            std::optional<RawPage> loaded_page;
            bool read_attempted = false;
            auto lease = files_.acquire_file(key.table_id);
            if (!lease.value || !(**lease.value).is_open()) {
                load_error = invalid("table PageFile is not open");
            } else {
                read_attempted = true;
                PageFileResult<RawPage> loaded;
                try { loaded = read_ ? read_(key) : (**lease.value).read_page(key.page_id); }
                catch (...) { loaded.error = PageFileError{PageFileErrorKind::kIo, "read adapter threw"}; }
                if (loaded.error) load_error = map_error(*loaded.error);
                else if (!loaded.value) load_error = invalid("read adapter returned neither page nor error");
                else loaded_page = std::move(*loaded.value);
            }

            lock.lock();
            if (prefetch && read_attempted) {
                if (loaded_page) ++stats_.prefetch_read_success;
                else ++stats_.prefetch_read_failure;
            }
            const bool reservation_matches = reserved.state == FrameState::kLoading &&
                reserved.key && *reserved.key == key && reserved.ticket == completion->ticket &&
                reserved.completion == completion;
            if (!reservation_matches) {
                const auto stale = invalid("BufferPool load reservation is stale");
                completion->error = stale;
                completion->done = true;
                lock.unlock();
                completion->cv.notify_all();
                return {std::nullopt, stale};
            }
            if (load_error) {
                page_table_.erase(key);
                reset_to_free(reserved);
                completion->error = *load_error;
                completion->done = true;
                lock.unlock();
                completion->cv.notify_all();
                return {std::nullopt, *load_error};
            }
            static_assert(std::is_nothrow_copy_assignable_v<RawPage>);
            reserved.page = *loaded_page;
            reserved.state = FrameState::kReady;
            reserved.dirty = false;
            reserved.completion.reset();
            reserved.pin_count = prefetch ? 0 : 1;
            reserved.prefetched_ready = prefetch;
            reserved.prefetch_had_foreground = completion->foreground_arrived;
            if (prefetch) ++stats_.prefetch_ready;
            fifo_.record_load(frame_id);
            completion->done = true;
            lock.unlock();
            completion->cv.notify_all();
            if (prefetch) return {};
            return {PageGuard(*this, frame_id), std::nullopt};
        }

        // A full pool keeps the established synchronous victim transaction intact.
        std::optional<FrameId> candidate;
        candidate = fifo_.choose_victim([this, prefetch](FrameId id) {
            return frames_[id].state == FrameState::kReady && frames_[id].key &&
                frames_[id].pin_count == 0 && !frames_[id].replacement_reserved &&
                (!prefetch || !frames_[id].dirty);
        });
        if (!candidate)
            return {std::nullopt, BufferPoolError{BufferPoolErrorKind::kNoVictim,
                                                "all resident Frames are pinned"}};
        Frame& target = frames_[*candidate];
        const auto old_key = target.key;
        if (target.ticket == std::numeric_limits<std::uint64_t>::max())
            return {std::nullopt, invalid("BufferPool Frame ticket exhausted")};
        if (old_key && stats_.eviction_count == std::numeric_limits<std::uint64_t>::max())
            return {std::nullopt, invalid("BufferPool eviction counter exhausted")};
        if (old_key && !target.dirty) {
            ++stats_.clean_victim_selection_count;
            if (target.replacement_ticket == std::numeric_limits<std::uint64_t>::max())
                return {std::nullopt, invalid("BufferPool replacement ticket exhausted")};
            const auto completion = std::make_shared<LoadCompletion>();
            completion->origin = origin;
            const auto next_ticket = target.replacement_ticket + 1;
            auto reservation = std::make_shared<LoadReservation>(LoadReservation{
                *candidate, *old_key, next_ticket, target.access_generation, completion});
            const auto [entry, inserted] = loading_table_.emplace(key, reservation);
            if (!inserted) std::terminate();
            target.replacement_reserved = true;
            target.replacement_ticket = next_ticket;

            if (prefetch) ++stats_.prefetch_started;
            lock.unlock();
            std::optional<BufferPoolError> load_error;
            std::optional<RawPage> loaded_page;
            bool read_attempted = false;
            {
                auto lease = files_.acquire_file(key.table_id);
                if (!lease.value || !(**lease.value).is_open()) {
                    load_error = invalid("table PageFile is not open");
                } else {
                    read_attempted = true;
                    PageFileResult<RawPage> loaded;
                    try { loaded = read_ ? read_(key) : (**lease.value).read_page(key.page_id); }
                    catch (...) { loaded.error = PageFileError{PageFileErrorKind::kIo, "read adapter threw"}; }
                    if (loaded.error) load_error = map_error(*loaded.error);
                    else if (!loaded.value) load_error = invalid("read adapter returned neither page nor error");
                    else loaded_page = std::move(*loaded.value);
                }
            }

            lock.lock();
            if (prefetch && read_attempted) {
                if (loaded_page) ++stats_.prefetch_read_success;
                else ++stats_.prefetch_read_failure;
            }
            const auto published = loading_table_.find(key);
            const bool reservation_published = published != loading_table_.end() && published->second == reservation;
            const bool victim_owned = target.replacement_reserved &&
                target.replacement_ticket == reservation->replacement_ticket;
            const auto complete_failure = [&](BufferPoolError error) {
                if (reservation_published) loading_table_.erase(key);
                if (victim_owned) target.replacement_reserved = false;
                if (error.kind == BufferPoolErrorKind::kNoVictim)
                    ++stats_.clean_replacement_cancel_count;
                completion->error = std::move(error);
                completion->done = true;
                const auto result = *completion->error;
                lock.unlock();
                completion->cv.notify_all();
                return BufferPoolResult<PageGuard>{std::nullopt, result};
            };
            if (!reservation_published)
                return complete_failure(invalid("BufferPool victim load reservation is stale"));
            if (load_error) return complete_failure(*load_error);

            const auto old_mapping = page_table_.find(*old_key);
            const bool can_commit = is_open_locked() && target.state == FrameState::kReady &&
                target.key && *target.key == *old_key && old_mapping != page_table_.end() &&
                old_mapping->second == *candidate && !target.dirty && target.pin_count == 0 &&
                victim_owned && target.access_generation == reservation->access_generation;
            if (!can_commit) {
                return complete_failure(BufferPoolError{BufferPoolErrorKind::kNoVictim,
                    "clean victim changed during speculative read"});
            }

            // Build the new mapping before changing the old committed identity.
            decltype(page_table_) staging;
            staging.emplace(key, *candidate);
            auto node = staging.extract(key);
            const auto inserted_page = page_table_.insert(std::move(node));
            if (!inserted_page.inserted) std::terminate();
            page_table_.erase(*old_key);
            static_assert(std::is_nothrow_copy_assignable_v<RawPage>);
            static_assert(std::is_nothrow_assignable_v<std::optional<PageKey>&, PageKey>);
            if (target.prefetched_ready && !target.prefetch_had_foreground) ++stats_.unused_prefetch_evicted;
            target.prefetched_ready = prefetch;
            target.prefetch_had_foreground = completion->foreground_arrived;
            completion->ticket = ++target.ticket;
            if (prefetch) ++stats_.prefetch_ready;
            target.page = *loaded_page;
            target.key = key;
            target.dirty = false;
            target.pin_count = prefetch ? 0 : 1;
            target.state = FrameState::kReady;
            target.replacement_reserved = false;
            target.completion.reset();
            loading_table_.erase(key);
            fifo_.record_load(*candidate);
            ++stats_.eviction_count;
            ++stats_.clean_replacement_commit_count;
            log_event("Evict", *old_key, *candidate);
            completion->done = true;
            lock.unlock();
            completion->cv.notify_all();
            if (prefetch) return {};
            return {PageGuard(*this, *candidate), std::nullopt};
        }
        if (old_key && target.dirty) {
            ++stats_.dirty_victim_selection_count;
            if (target.replacement_ticket == std::numeric_limits<std::uint64_t>::max())
                return {std::nullopt, invalid("BufferPool replacement ticket exhausted")};
            const auto completion = std::make_shared<LoadCompletion>();
            completion->origin = origin;
            const auto next_ticket = target.replacement_ticket + 1;
            auto reservation = std::make_shared<LoadReservation>(LoadReservation{
                *candidate, *old_key, next_ticket, target.access_generation, completion});
            const auto [entry, inserted] = loading_table_.emplace(key, reservation);
            if (!inserted) std::terminate();
            target.replacement_reserved = true;
            target.replacement_ticket = next_ticket;
            ++stats_.dirty_requested_read_count;

            if (prefetch) ++stats_.prefetch_started;
            lock.unlock();
            std::optional<BufferPoolError> load_error;
            std::optional<RawPage> loaded_page;
            bool read_attempted = false;
            {
                auto lease = files_.acquire_file(key.table_id);
                if (!lease.value || !(**lease.value).is_open()) {
                    load_error = invalid("table PageFile is not open");
                } else {
                    read_attempted = true;
                    PageFileResult<RawPage> loaded;
                    try { loaded = read_ ? read_(key) : (**lease.value).read_page(key.page_id); }
                    catch (...) { loaded.error = PageFileError{PageFileErrorKind::kIo, "read adapter threw"}; }
                    if (loaded.error) load_error = map_error(*loaded.error);
                    else if (!loaded.value) load_error = invalid("read adapter returned neither page nor error");
                    else loaded_page = std::move(*loaded.value);
                }
            }

            lock.lock();
            if (prefetch && read_attempted) {
                if (loaded_page) ++stats_.prefetch_read_success;
                else ++stats_.prefetch_read_failure;
            }
            const auto complete_failure = [&](BufferPoolError error) {
                const auto published = loading_table_.find(key);
                if (published != loading_table_.end() && published->second == reservation)
                    loading_table_.erase(published);
                if (target.replacement_reserved &&
                    target.replacement_ticket == reservation->replacement_ticket)
                    target.replacement_reserved = false;
                if (error.kind == BufferPoolErrorKind::kNoVictim)
                    ++stats_.dirty_replacement_cancel_count;
                completion->error = std::move(error);
                completion->done = true;
                const auto result = *completion->error;
                lock.unlock();
                completion->cv.notify_all();
                return BufferPoolResult<PageGuard>{std::nullopt, result};
            };
            const auto reservation_published = [&] {
                const auto published = loading_table_.find(key);
                return published != loading_table_.end() && published->second == reservation;
            };
            const auto victim_owned = [&] {
                return target.replacement_reserved &&
                    target.replacement_ticket == reservation->replacement_ticket;
            };
            if (!reservation_published())
                return complete_failure(invalid("BufferPool dirty victim load reservation is stale"));
            if (load_error) return complete_failure(*load_error);

            const auto old_mapping = page_table_.find(*old_key);
            const bool can_snapshot = is_open_locked() && target.state == FrameState::kReady &&
                target.key && *target.key == *old_key && old_mapping != page_table_.end() &&
                old_mapping->second == *candidate && target.dirty && target.pin_count == 0 &&
                victim_owned() && target.access_generation == reservation->access_generation;
            if (!can_snapshot) {
                return complete_failure(BufferPoolError{BufferPoolErrorKind::kNoVictim,
                    "dirty victim changed during speculative read"});
            }
            if (stats_.dirty_flush_count == std::numeric_limits<std::uint64_t>::max())
                return complete_failure(invalid("BufferPool dirty flush counter exhausted"));

            static_assert(std::is_nothrow_copy_constructible_v<RawPage>);
            const RawPage dirty_snapshot = target.page;
            const auto write_generation = target.access_generation;

            lock.unlock();
            std::optional<BufferPoolError> write_error;
            bool write_attempted = false;
            bool write_succeeded = false;
            {
                auto lease = files_.acquire_file(old_key->table_id);
                if (!lease.value || !(**lease.value).is_open()) {
                    write_error = invalid("table PageFile is not open");
                    log_event("Flush dirty", *old_key, {}, "failure");
                } else {
                    write_attempted = true;
                    const auto error = write_ ? write_(*old_key, dirty_snapshot)
                                              : (**lease.value).write_page(old_key->page_id, dirty_snapshot);
                    if (error) {
                        write_error = map_error(*error);
                        log_event("Flush dirty", *old_key, {}, "failure");
                    } else {
                        write_succeeded = true;
                        log_event("Flush dirty", *old_key, {}, "success");
                    }
                }
            }

            lock.lock();
            if (write_attempted) ++stats_.dirty_write_attempt_count;
            if (write_succeeded) ++stats_.dirty_write_success_count;
            if (write_error) return complete_failure(*write_error);
            const auto final_mapping = page_table_.find(*old_key);
            const bool can_commit = is_open_locked() && reservation_published() &&
                target.state == FrameState::kReady && target.key && *target.key == *old_key &&
                final_mapping != page_table_.end() && final_mapping->second == *candidate &&
                target.dirty && target.pin_count == 0 && victim_owned() &&
                target.access_generation == write_generation &&
                stats_.dirty_flush_count != std::numeric_limits<std::uint64_t>::max();
            if (!can_commit) {
                return complete_failure(BufferPoolError{BufferPoolErrorKind::kNoVictim,
                    "dirty victim changed during speculative writeback"});
            }

            // Build the new mapping before changing the old committed identity.
            decltype(page_table_) staging;
            staging.emplace(key, *candidate);
            auto node = staging.extract(key);
            const auto inserted_page = page_table_.insert(std::move(node));
            if (!inserted_page.inserted) std::terminate();
            page_table_.erase(*old_key);
            static_assert(std::is_nothrow_copy_assignable_v<RawPage>);
            static_assert(std::is_nothrow_assignable_v<std::optional<PageKey>&, PageKey>);
            if (target.prefetched_ready && !target.prefetch_had_foreground) ++stats_.unused_prefetch_evicted;
            target.prefetched_ready = prefetch;
            target.prefetch_had_foreground = completion->foreground_arrived;
            completion->ticket = ++target.ticket;
            if (prefetch) ++stats_.prefetch_ready;
            target.page = *loaded_page;
            target.key = key;
            target.dirty = false;
            target.pin_count = prefetch ? 0 : 1;
            target.state = FrameState::kReady;
            target.replacement_reserved = false;
            target.completion.reset();
            loading_table_.erase(key);
            fifo_.record_load(*candidate);
            ++stats_.dirty_flush_count;
            ++stats_.eviction_count;
            ++stats_.dirty_replacement_commit_count;
            log_event("Evict", *old_key, *candidate);
            completion->done = true;
            lock.unlock();
            completion->cv.notify_all();
            if (prefetch) return {};
            return {PageGuard(*this, *candidate), std::nullopt};
        }
        auto lease = files_.acquire_file(key.table_id);
        if (!lease.value || !(**lease.value).is_open())
            return {std::nullopt, invalid("table PageFile is not open")};
        auto loaded = read_ ? read_(key) : (**lease.value).read_page(key.page_id);
        if (loaded.error) return {std::nullopt, map_error(*loaded.error)};
        if (!loaded.value) return {std::nullopt, invalid("read adapter returned neither page nor error")};

        // Stage a map node outside PageTable before writeback: allocation failure cannot
        // alter resident state, and I/O callbacks see only the old committed mappings.
        decltype(page_table_) staging;
        staging.emplace(key, *candidate);
        auto node = staging.extract(key);
        if (old_key) {
            if (auto error = flush_frame_locked(target)) return {std::nullopt, std::move(error)};
        }
        // All I/O succeeded. Same allocator + reserved buckets + noexcept hash/equality
        // mean node insertion does not allocate or rehash during commit.
        const auto inserted = page_table_.insert(std::move(node));
        if (!inserted.inserted) std::terminate(); // Non-reentrant single-thread invariant.
        if (old_key) page_table_.erase(*old_key);
        static_assert(std::is_nothrow_copy_assignable_v<RawPage>);
        static_assert(std::is_nothrow_assignable_v<std::optional<PageKey>&, PageKey>);
        target.page = *loaded.value;
        target.key = key;
        target.dirty = false;
        target.pin_count = 1;
        target.state = FrameState::kReady;
        target.completion.reset();
        fifo_.record_load(*candidate);
        if (old_key) {
            ++stats_.eviction_count;
            log_event("Evict", *old_key, *candidate);
        }
        return {PageGuard(*this, *candidate), std::nullopt};
    } // demand retry loop
}
std::optional<BufferPoolError> BufferPool::close() {
    std::lock_guard control(worker_control_mutex_);
    stop_prefetch_worker();
    bool retry = false;
    const auto restart = [this, &retry](BufferPool*) { if (retry) start_prefetch_worker(); };
    std::unique_ptr<BufferPool, decltype(restart)> resume(this, restart);
    std::lock_guard lock(metadata_mutex_);
    if (lifecycle_ == LifecycleState::kClosed) return std::nullopt;
    if (lifecycle_ == LifecycleState::kClosing) return invalid("BufferPool is closing");
    lifecycle_ = LifecycleState::kClosing;
    if (!loading_table_.empty() || std::any_of(frames_.begin(), frames_.end(), [](const Frame& f) {
        return f.pin_count != 0 || f.replacement_reserved;
    }))
    {
        lifecycle_ = LifecycleState::kOpen;
        retry = true;
        return invalid("cannot close BufferPool with active PageGuards");
    }
    if (auto error = flush_all_locked()) {
        lifecycle_ = LifecycleState::kOpen;
        retry = true;
        return error;
    }
    page_table_.clear();
    fifo_.clear();
    for (auto& frame : frames_) reset_to_free(frame);
    lifecycle_ = LifecycleState::kClosed;
    return std::nullopt;
}
BufferPoolStats BufferPool::stats() const noexcept {
    std::lock_guard lock(metadata_mutex_);
    auto result = stats_;
    result.prefetch_requested = prefetch_requested_.load(std::memory_order_relaxed);
    result.prefetch_dropped += prefetch_submission_dropped_.load(std::memory_order_relaxed);
    result.prefetch_deduplicated += prefetch_submission_deduplicated_.load(std::memory_order_relaxed);
    return result;
}

} // namespace tinydbms::storage::internal
