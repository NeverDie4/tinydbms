#pragma once

#include "page_types.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace tinydbms::storage::internal {

struct StorageTestAccess;
struct PageFileTestAccess;
class FileManager;

inline constexpr std::array<char, 8> kPageFileMagic{
    'T', 'D', 'B', 'P', 'A', 'G', 'E', '1'};
inline constexpr std::uint32_t kPageFileFormatVersion = 1;

enum class PageFileErrorKind {
    kIo,
    kCorrupt,
    kInvalidArgument,
};

struct PageFileError {
    PageFileErrorKind kind;
    std::string message;
};

template <typename T>
struct PageFileResult {
    std::optional<T> value;
    std::optional<PageFileError> error;
};

enum class PageAllocationState { kAllocated, kFree };

// Physical counters describe successful/attempted C++ stream operations, not
// device persistence. Timings are synchronous stream-path durations; write
// time includes its flush, while flush time is tracked separately.
struct PageFileStats {
    std::uint64_t physical_read_attempts = 0;
    std::uint64_t physical_read_successes = 0;
    std::uint64_t physical_read_failures = 0;
    std::uint64_t physical_write_attempts = 0;
    std::uint64_t physical_write_successes = 0;
    std::uint64_t physical_write_failures = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t bytes_written = 0;
    std::chrono::nanoseconds read_time_total{};
    std::chrono::nanoseconds write_time_total{};
    std::uint64_t flush_count = 0;
    std::chrono::nanoseconds flush_time_total{};
    // Logical locality covers successful read_page/write_page data requests.
    // Header and free-list raw I/O remain in physical counters only.
    std::uint64_t sequential_read_count = 0;
    std::uint64_t sequential_write_count = 0;
    std::uint64_t read_logical_page_distance = 0;
    std::uint64_t write_logical_page_distance = 0;
};

class PageFile {
public:
    static PageFileResult<std::unique_ptr<PageFile>> create(
        const std::filesystem::path& path);
    static PageFileResult<std::unique_ptr<PageFile>> open(
        const std::filesystem::path& path);

    ~PageFile();

    PageFile(const PageFile&) = delete;
    PageFile& operator=(const PageFile&) = delete;
    PageFile(PageFile&&) = delete;
    PageFile& operator=(PageFile&&) = delete;

    PageFileResult<PageId> allocate_page();
    PageFileResult<PageAllocationState> page_allocation_state(PageId page_id);
    std::optional<PageFileError> free_page(PageId page_id);
    PageFileResult<RawPage> read_page(PageId page_id);
    std::optional<PageFileError> write_page(PageId page_id, const RawPage& page);
    std::optional<PageFileError> close();

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::uint64_t page_count() const noexcept;
    [[nodiscard]] PageFileStats stats() const noexcept;

private:
    friend class HeapTable; // Reuse existing volatile poison latch on failed bootstrap compensation.
    friend class FileManager;
    friend struct StorageTestAccess;
    friend struct PageFileTestAccess;
    explicit PageFile(std::filesystem::path path);

    PageFileResult<PageId> allocate_page_locked();
    PageFileResult<PageAllocationState> page_allocation_state_locked(PageId page_id);
    std::optional<PageFileError> free_page_locked(PageId page_id);
    PageFileResult<RawPage> read_page_locked(PageId page_id);
    std::optional<PageFileError> write_page_locked(PageId page_id, const RawPage& page);
    std::optional<PageFileError> close_locked();
    PageFileResult<RawPage> read_raw_page_locked(PageId page_id);
    std::optional<PageFileError> write_raw_page_locked(PageId page_id, const RawPage& page);
    std::optional<PageFileError> write_header_locked();
    PageFileResult<bool> is_free_page_locked(PageId page_id);
    PageFileResult<PageId> read_free_next_locked(PageId page_id);
    std::optional<PageFileError> require_open() const;
    std::optional<PageFileError> require_open_locked() const;
    void record_read_locality(PageId page_id) noexcept;
    void record_write_locality(PageId page_id) noexcept;
    void record_flush(std::chrono::steady_clock::duration elapsed) noexcept;
    static bool take_close_failure_for_testing() noexcept;

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::fstream stream_;
    // Independent test-failure latch; never substitutes for the instance mutex.
    static std::atomic_bool fail_next_close_for_testing_;
    bool open_{false};
    bool poisoned_{false};
    std::uint64_t page_count_{1};
    PageId free_page_head_{0};
    PageFileStats stats_{};
    std::optional<PageId> last_read_page_id_;
    std::optional<PageId> last_write_page_id_;
    // Storage-private deterministic close-exclusion test coordination only.
    std::function<void()> before_raw_read_for_testing_;
};

}  // namespace tinydbms::storage::internal
