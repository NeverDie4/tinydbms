#pragma once

#include "file_manager.h"

#include <functional>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

struct Frame {
    RawPage page;
    std::optional<PageKey> key;
    bool dirty = false;
    std::uint32_t pin_count = 0;
    bool try_pin() noexcept;
    bool try_unpin() noexcept;
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
struct BufferPoolStats {
    std::uint64_t fetch_count = 0;
    std::uint64_t hit_count = 0;
    std::uint64_t miss_count = 0;
    std::uint64_t dirty_flush_count = 0;
    std::uint64_t eviction_count = 0;
    double hit_rate() const noexcept;
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
        ReplacementPolicy policy = ReplacementPolicy::kFifo, LogSink log = {});
    // All guards must be released first. Destruction with outstanding pins is a
    // programmer lifetime violation (terminate); close() instead reports an error.
    // Destructor does not perform I/O: explicit successful close is required to persist.
    ~BufferPool() noexcept;
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    BufferPool(BufferPool&&) = delete;
    BufferPool& operator=(BufferPool&&) = delete;

    BufferPoolResult<PageGuard> fetch_page(PageKey key);
    std::optional<BufferPoolError> flush_page(PageKey key);
    std::optional<BufferPoolError> flush_all();
    std::optional<BufferPoolError> release_table(TableId table_id);
    std::optional<BufferPoolError> close();
    BufferPoolStats stats() const noexcept;

private:
    friend class PageGuard;
    friend struct BufferPoolTestAccess; // Read-only invariant inspection in tests.
    BufferPool(FileManager& files, std::size_t capacity, ReadPage read, WritePage write,
               ReplacementPolicy policy, LogSink log);
    void log_event(std::string_view event, PageKey key, std::optional<FrameId> frame = {},
                   std::string_view result = {}) const noexcept;
    std::optional<BufferPoolError> flush_frame(Frame& frame);
    void unpin(FrameId frame) noexcept;
    FileManager& files_; // Must outlive pool. Files cannot close/reopen while cached.
    std::vector<Frame> frames_; // Sized once; never grows.
    std::unordered_map<PageKey, FrameId, PageKeyHash> page_table_;
    FifoReplacer fifo_;
    ReplacementPolicy policy_;
    LogSink log_;
    ReadPage read_;
    WritePage write_;
    BufferPoolStats stats_;
    bool open_ = true;
};

} // namespace tinydbms::storage::internal
