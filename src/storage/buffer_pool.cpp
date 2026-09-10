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
    return pool_->frames_[frame_].pin_count;
}
void PageGuard::release() noexcept {
    if (auto* pool = std::exchange(pool_, nullptr)) pool->unpin(frame_);
}

BufferPool::BufferPool(FileManager& files, std::size_t capacity, ReadPage read, WritePage write,
                       ReplacementPolicy policy, LogSink log)
    : files_(files), frames_(capacity), fifo_(capacity), policy_(policy), log_(std::move(log)),
      read_(std::move(read)), write_(std::move(write)) {
    // One extra entry permits node-handle insertion before removing the old key.
    page_table_.reserve(capacity + 1);
}
BufferPoolResult<std::unique_ptr<BufferPool>> BufferPool::create(
    FileManager& files, std::size_t capacity, ReadPage read, WritePage write,
    ReplacementPolicy policy, LogSink log) {
    if (capacity == 0 || capacity == std::numeric_limits<std::size_t>::max())
        return {std::nullopt, invalid("BufferPool capacity is not representable or is zero")};
    if (policy != ReplacementPolicy::kFifo && policy != ReplacementPolicy::kLru)
        return {std::nullopt, invalid("unknown replacement policy")};
    return {std::unique_ptr<BufferPool>(new BufferPool(files, capacity, std::move(read), std::move(write), policy, std::move(log))), std::nullopt};
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
RawPage& PageGuard::mutable_page() {
    if (!valid()) throw std::logic_error("access to released PageGuard");
    return pool_->frames_[frame_].page;
}
void PageGuard::mark_dirty() {
    if (!valid()) throw std::logic_error("access to released PageGuard");
    pool_->frames_[frame_].dirty = true;
}
std::optional<BufferPoolError> BufferPool::flush_frame(Frame& frame) {
    if (!frame.key || !frame.dirty) return std::nullopt;
    if (stats_.dirty_flush_count == std::numeric_limits<std::uint64_t>::max())
        return invalid("BufferPool dirty flush counter exhausted");
    PageFile* file = files_.find_table_file(frame.key->table_id);
    if (file == nullptr || !file->is_open()) {
        log_event("Flush dirty", *frame.key, {}, "failure");
        return invalid("table PageFile is not open");
    }
    const auto error = write_ ? write_(*frame.key, frame.page)
                              : file->write_page(frame.key->page_id, frame.page);
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
    if (!open_) return invalid("BufferPool is closed");
    const auto found = page_table_.find(key);
    if (found == page_table_.end()) return std::nullopt;
    return flush_frame(frames_[found->second]);
}
std::optional<BufferPoolError> BufferPool::flush_all() {
    if (!open_) return invalid("BufferPool is closed");
    for (auto& frame : frames_) {
        if (auto error = flush_frame(frame)) return error;
    }
    return std::nullopt;
}
std::optional<BufferPoolError> BufferPool::release_table(TableId table_id) {
    if (!open_) return invalid("BufferPool is closed");
    const auto belongs = [table_id](const Frame& frame) {
        return frame.key && frame.key->table_id == table_id;
    };
    // Complete prepare pass before performing any I/O or removal.
    for (const auto& frame : frames_) {
        if (belongs(frame) && frame.pin_count != 0)
            return invalid("cannot release table with active PageGuards");
    }
    for (auto& frame : frames_) {
        if (belongs(frame)) {
            if (auto error = flush_frame(frame)) return error;
        }
    }
    // Commit only after every target write succeeded; unrelated frames stay resident.
    for (auto& frame : frames_) {
        if (belongs(frame)) {
            page_table_.erase(*frame.key);
            fifo_.remove(static_cast<FrameId>(&frame - frames_.data()));
            frame = Frame{};
        }
    }
    return std::nullopt;
}
BufferPool::~BufferPool() noexcept {
    // Violating the owner-before-guard lifetime rule must not become a dangling pointer.
    if (std::any_of(frames_.begin(), frames_.end(), [](const Frame& f) { return f.pin_count != 0; }))
        std::terminate();
}
void BufferPool::unpin(FrameId frame) noexcept {
    if (frame >= frames_.size() || !frames_[frame].key || !frames_[frame].try_unpin())
        std::terminate(); // Internal invariant failure; guard destruction cannot throw.
}
BufferPoolResult<PageGuard> BufferPool::fetch_page(PageKey key) {
    if (!open_ || key.page_id == 0)
        return {std::nullopt, invalid("pool is closed or PageId is zero")};
    // Lookup borrows the PageFile only for this call; never retained in a Frame.
    PageFile* file = files_.find_table_file(key.table_id);
    if (file == nullptr || !file->is_open())
        return {std::nullopt, invalid("table PageFile is not open")};
    if (stats_.fetch_count == std::numeric_limits<std::uint64_t>::max())
        return {std::nullopt, invalid("BufferPool fetch counter exhausted")};
    ++stats_.fetch_count;
    const auto found = page_table_.find(key);
    if (found != page_table_.end()) {
        ++stats_.hit_count;
        if (!frames_[found->second].try_pin())
            return {std::nullopt, invalid("Frame pin count overflow")};
        if (policy_ == ReplacementPolicy::kLru) fifo_.record_load(found->second);
        log_event("Buffer HIT", key, found->second);
        return {PageGuard(*this, found->second), std::nullopt};
    }
    ++stats_.miss_count;
    log_event("Buffer MISS", key);
    const auto empty = std::find_if(frames_.begin(), frames_.end(),
                                  [](const Frame& frame) { return !frame.key; });
    std::optional<FrameId> candidate;
    if (empty != frames_.end()) candidate = static_cast<FrameId>(empty - frames_.begin());
    else candidate = fifo_.choose_victim([this](FrameId id) {
        return frames_[id].key && frames_[id].pin_count == 0;
    });
    if (!candidate)
        return {std::nullopt, BufferPoolError{BufferPoolErrorKind::kNoVictim,
                                            "all resident Frames are pinned"}};
    Frame& target = frames_[*candidate];
    const auto old_key = target.key;
    if (old_key && stats_.eviction_count == std::numeric_limits<std::uint64_t>::max())
        return {std::nullopt, invalid("BufferPool eviction counter exhausted")};
    auto loaded = read_ ? read_(key) : file->read_page(key.page_id);
    if (loaded.error) return {std::nullopt, map_error(*loaded.error)};
    if (!loaded.value) return {std::nullopt, invalid("read adapter returned neither page nor error")};

    // Stage a map node outside PageTable before writeback: allocation failure cannot
    // alter resident state, and I/O callbacks see only the old committed mappings.
    decltype(page_table_) staging;
    staging.emplace(key, *candidate);
    auto node = staging.extract(key);
    if (old_key) {
        if (auto error = flush_frame(target)) return {std::nullopt, std::move(error)};
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
    fifo_.record_load(*candidate);
    if (old_key) {
        ++stats_.eviction_count;
        log_event("Evict", *old_key, *candidate);
    }
    return {PageGuard(*this, *candidate), std::nullopt};
}
std::optional<BufferPoolError> BufferPool::close() {
    if (!open_) return std::nullopt;
    if (std::any_of(frames_.begin(), frames_.end(), [](const Frame& f) { return f.pin_count != 0; }))
        return invalid("cannot close BufferPool with active PageGuards");
    if (auto error = flush_all()) return error;
    page_table_.clear();
    fifo_.clear();
    for (auto& frame : frames_) frame = Frame{};
    open_ = false;
    return std::nullopt;
}
BufferPoolStats BufferPool::stats() const noexcept { return stats_; }

} // namespace tinydbms::storage::internal
