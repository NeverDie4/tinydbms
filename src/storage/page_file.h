#pragma once

#include "page_types.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

namespace tinydbms::storage::internal {

struct StorageTestAccess;
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

    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] std::uint64_t page_count() const noexcept { return page_count_; }

private:
    friend class HeapTable; // Reuse existing volatile poison latch on failed bootstrap compensation.
    friend class FileManager;
    friend struct StorageTestAccess;
    explicit PageFile(std::filesystem::path path);

    PageFileResult<RawPage> read_raw_page(PageId page_id);
    std::optional<PageFileError> write_raw_page(PageId page_id, const RawPage& page);
    std::optional<PageFileError> write_header();
    PageFileResult<bool> is_free_page(PageId page_id);
    PageFileResult<PageId> read_free_next(PageId page_id);
    std::optional<PageFileError> require_open() const;
    static bool take_close_failure_for_testing() noexcept;

    std::filesystem::path path_;
    std::fstream stream_;
    static bool fail_next_close_for_testing_;
    bool open_{false};
    bool poisoned_{false};
    std::uint64_t page_count_{1};
    PageId free_page_head_{0};
};

}  // namespace tinydbms::storage::internal
