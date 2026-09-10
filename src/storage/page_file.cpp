#include "page_file.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace tinydbms::storage::internal {
namespace {

constexpr std::size_t kVersionOffset = 8;
constexpr std::size_t kFreePageHeadOffset = 12;
constexpr std::size_t kPageCountOffset = 16;
constexpr std::array<char, 4> kFreePageMagic{'T', 'F', 'R', '1'};

PageFileError make_error(PageFileErrorKind kind, std::string message) {
    return PageFileError{kind, std::move(message)};
}

template <typename T>
PageFileResult<T> failure(PageFileErrorKind kind, std::string message) {
    return PageFileResult<T>{std::nullopt, make_error(kind, std::move(message))};
}

template <typename T>
PageFileResult<T> success(T value) {
    return PageFileResult<T>{std::move(value), std::nullopt};
}

void put_u32(RawPage& page, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        page.bytes[offset + index] =
            std::byte{static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU)};
    }
}

void put_u64(RawPage& page, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        page.bytes[offset + index] =
            std::byte{static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU)};
    }
}

std::uint32_t get_u32(const RawPage& page, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= std::uint32_t{std::to_integer<std::uint8_t>(page.bytes[offset + index])}
                 << (index * 8U);
    }
    return value;
}

std::uint64_t get_u64(const RawPage& page, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= std::uint64_t{std::to_integer<std::uint8_t>(page.bytes[offset + index])}
                 << (index * 8U);
    }
    return value;
}

RawPage make_header(std::uint64_t page_count, PageId free_page_head) {
    RawPage page;
    std::transform(kPageFileMagic.begin(), kPageFileMagic.end(), page.bytes.begin(),
                   [](char value) { return std::byte{static_cast<unsigned char>(value)}; });
    put_u32(page, kVersionOffset, kPageFileFormatVersion);
    put_u32(page, kFreePageHeadOffset, free_page_head);
    put_u64(page, kPageCountOffset, page_count);
    return page;
}

bool has_magic(const RawPage& page, const auto& magic) {
    return std::equal(magic.begin(), magic.end(), page.bytes.begin(),
                      [](char expected, std::byte actual) {
                          return static_cast<unsigned char>(expected) ==
                                 std::to_integer<unsigned char>(actual);
                      });
}

std::optional<std::streamoff> page_offset(PageId page_id) {
    const auto offset = std::uint64_t{page_id} * std::uint64_t{kPageSize};
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return std::nullopt;
    }
    return static_cast<std::streamoff>(offset);
}

}  // namespace

PageFile::PageFile(std::filesystem::path path) : path_(std::move(path)) {}

bool PageFile::fail_next_close_for_testing_ = false;

bool PageFile::take_close_failure_for_testing() noexcept {
    return std::exchange(fail_next_close_for_testing_, false);
}

PageFile::~PageFile() {
    (void)close();
}

PageFileResult<std::unique_ptr<PageFile>> PageFile::create(
    const std::filesystem::path& path) {
    std::error_code filesystem_error;
    const bool exists = std::filesystem::exists(path, filesystem_error);
    if (filesystem_error) {
        return failure<std::unique_ptr<PageFile>>(PageFileErrorKind::kIo,
                                                  "cannot inspect PageFile path");
    }
    if (exists) {
        return failure<std::unique_ptr<PageFile>>(PageFileErrorKind::kInvalidArgument,
                                                  "PageFile already exists");
    }

    std::ofstream output(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        return failure<std::unique_ptr<PageFile>>(PageFileErrorKind::kIo,
                                                  "cannot create PageFile");
    }
    const RawPage header = make_header(1, 0);
    output.write(reinterpret_cast<const char*>(header.bytes.data()),
                 static_cast<std::streamsize>(header.bytes.size()));
    output.flush();
    const bool write_succeeded = output.good();
    output.close();
    const bool close_succeeded = !output.fail();
    if (!write_succeeded || !close_succeeded) {
        std::filesystem::remove(path, filesystem_error);
        return failure<std::unique_ptr<PageFile>>(PageFileErrorKind::kIo,
                                                  "cannot initialize PageFile header");
    }
    return open(path);
}

PageFileResult<std::unique_ptr<PageFile>> PageFile::open(
    const std::filesystem::path& path) {
    std::error_code filesystem_error;
    const auto file_size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        return failure<std::unique_ptr<PageFile>>(PageFileErrorKind::kIo,
                                                  "cannot inspect PageFile size");
    }
    if (file_size < kPageSize || file_size % kPageSize != 0) {
        return failure<std::unique_ptr<PageFile>>(PageFileErrorKind::kCorrupt,
                                                  "PageFile length is invalid");
    }

    auto file = std::unique_ptr<PageFile>(new PageFile(path));
    file->stream_.open(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file->stream_.is_open()) {
        return failure<std::unique_ptr<PageFile>>(PageFileErrorKind::kIo,
                                                  "cannot open PageFile");
    }
    file->open_ = true;

    auto header_result = file->read_raw_page(0);
    if (!header_result.value.has_value()) {
        file->close();
        return failure<std::unique_ptr<PageFile>>(
            header_result.error->kind, header_result.error->message);
    }
    const RawPage& header = *header_result.value;
    if (!has_magic(header, kPageFileMagic) ||
        get_u32(header, kVersionOffset) != kPageFileFormatVersion) {
        file->close();
        return failure<std::unique_ptr<PageFile>>(PageFileErrorKind::kCorrupt,
                                                  "PageFile header magic or version is invalid");
    }

    const auto page_count = get_u64(header, kPageCountOffset);
    constexpr auto kMaximumPageCount =
        std::uint64_t{std::numeric_limits<PageId>::max()} + 1U;
    if (page_count == 0 || page_count > kMaximumPageCount ||
        page_count * std::uint64_t{kPageSize} > file_size) {
        file->close();
        return failure<std::unique_ptr<PageFile>>(PageFileErrorKind::kCorrupt,
                                                  "PageFile page_count is invalid");
    }
    file->page_count_ = page_count;
    file->free_page_head_ = get_u32(header, kFreePageHeadOffset);

    std::unordered_set<PageId> visited;
    PageId current = file->free_page_head_;
    while (current != 0) {
        if (std::uint64_t{current} >= file->page_count_ || !visited.insert(current).second) {
            file->close();
            return failure<std::unique_ptr<PageFile>>(PageFileErrorKind::kCorrupt,
                                                      "PageFile free list is invalid");
        }
        auto next = file->read_free_next(current);
        if (!next.value.has_value()) {
            file->close();
            return failure<std::unique_ptr<PageFile>>(next.error->kind, next.error->message);
        }
        current = *next.value;
    }
    return success(std::move(file));
}

std::optional<PageFileError> PageFile::require_open() const {
    if (!open_) {
        return make_error(PageFileErrorKind::kInvalidArgument, "PageFile is closed");
    }
    if (poisoned_) {
        return make_error(PageFileErrorKind::kIo, "PageFile state is unavailable after an I/O failure");
    }
    return std::nullopt;
}

PageFileResult<RawPage> PageFile::read_raw_page(PageId page_id) {
    const auto offset = page_offset(page_id);
    if (!offset.has_value()) {
        return failure<RawPage>(PageFileErrorKind::kInvalidArgument, "Page offset is not representable");
    }
    RawPage page;
    stream_.clear();
    stream_.seekg(*offset);
    if (!stream_) {
        return failure<RawPage>(PageFileErrorKind::kIo, "cannot seek while reading PageFile");
    }
    stream_.read(reinterpret_cast<char*>(page.bytes.data()),
                 static_cast<std::streamsize>(page.bytes.size()));
    if (stream_.gcount() != static_cast<std::streamsize>(page.bytes.size())) {
        const bool io_failure = stream_.bad();
        stream_.clear();
        return failure<RawPage>(io_failure ? PageFileErrorKind::kIo : PageFileErrorKind::kCorrupt,
                                "PageFile page read was short");
    }
    return success(std::move(page));
}

std::optional<PageFileError> PageFile::write_raw_page(PageId page_id, const RawPage& page) {
    const auto offset = page_offset(page_id);
    if (!offset.has_value()) {
        return make_error(PageFileErrorKind::kInvalidArgument, "Page offset is not representable");
    }
    stream_.clear();
    stream_.seekp(*offset);
    if (!stream_) {
        return make_error(PageFileErrorKind::kIo, "cannot seek while writing PageFile");
    }
    stream_.write(reinterpret_cast<const char*>(page.bytes.data()),
                  static_cast<std::streamsize>(page.bytes.size()));
    stream_.flush();
    if (!stream_) {
        return make_error(PageFileErrorKind::kIo, "cannot write PageFile page");
    }
    return std::nullopt;
}

std::optional<PageFileError> PageFile::write_header() {
    return write_raw_page(0, make_header(page_count_, free_page_head_));
}

PageFileResult<PageId> PageFile::read_free_next(PageId page_id) {
    auto page = read_raw_page(page_id);
    if (!page.value.has_value()) {
        return failure<PageId>(page.error->kind, page.error->message);
    }
    if (!has_magic(*page.value, kFreePageMagic)) {
        return failure<PageId>(PageFileErrorKind::kCorrupt, "free page marker is invalid");
    }
    return success(get_u32(*page.value, 4));
}

PageFileResult<bool> PageFile::is_free_page(PageId page_id) {
    std::unordered_set<PageId> visited;
    bool found = false;
    PageId current = free_page_head_;
    while (current != 0) {
        if (std::uint64_t{current} >= page_count_ || !visited.insert(current).second) {
            return failure<bool>(PageFileErrorKind::kCorrupt, "PageFile free list is invalid");
        }
        if (current == page_id) {
            found = true;
        }
        auto next = read_free_next(current);
        if (!next.value.has_value()) {
            return failure<bool>(next.error->kind, next.error->message);
        }
        current = *next.value;
    }
    return success(found);
}

PageFileResult<PageAllocationState> PageFile::page_allocation_state(PageId page_id) {
    if (auto error = require_open()) return {std::nullopt, std::move(error)};
    if (page_id == 0 || std::uint64_t{page_id} >= page_count_) {
        return failure<PageAllocationState>(PageFileErrorKind::kInvalidArgument,
                                             "PageId is outside the data-page range");
    }
    auto free = is_free_page(page_id);
    if (free.error) return {std::nullopt, std::move(free.error)};
    return success(*free.value ? PageAllocationState::kFree : PageAllocationState::kAllocated);
}

PageFileResult<RawPage> PageFile::read_page(PageId page_id) {
    if (auto error = require_open()) {
        return PageFileResult<RawPage>{std::nullopt, std::move(error)};
    }
    if (page_id == 0 || std::uint64_t{page_id} >= page_count_) {
        return failure<RawPage>(PageFileErrorKind::kInvalidArgument, "PageId is outside the data-page range");
    }
    auto free = is_free_page(page_id);
    if (!free.value.has_value()) {
        return failure<RawPage>(free.error->kind, free.error->message);
    }
    if (*free.value) {
        return failure<RawPage>(PageFileErrorKind::kInvalidArgument, "free page cannot be read");
    }
    return read_raw_page(page_id);
}

std::optional<PageFileError> PageFile::write_page(PageId page_id, const RawPage& page) {
    if (auto error = require_open()) {
        return error;
    }
    if (page_id == 0 || std::uint64_t{page_id} >= page_count_) {
        return make_error(PageFileErrorKind::kInvalidArgument,
                          "PageId is outside the data-page range");
    }
    auto free = is_free_page(page_id);
    if (!free.value.has_value()) {
        return free.error;
    }
    if (*free.value) {
        return make_error(PageFileErrorKind::kInvalidArgument, "free page cannot be written");
    }
    return write_raw_page(page_id, page);
}

PageFileResult<PageId> PageFile::allocate_page() {
    if (auto error = require_open()) {
        return PageFileResult<PageId>{std::nullopt, std::move(error)};
    }

    RawPage empty_page;
    if (free_page_head_ != 0) {
        // Reject a damaged link/cycle before publishing a new header or zeroing a node.
        auto validated=is_free_page(free_page_head_);
        if (validated.error) return {std::nullopt,std::move(validated.error)};
        const PageId allocated = free_page_head_;
        auto next = read_free_next(allocated);
        if (!next.value.has_value()) {
            return failure<PageId>(next.error->kind, next.error->message);
        }
        const PageId old_head = free_page_head_;
        free_page_head_ = *next.value;
        if (auto error = write_header()) {
            free_page_head_ = old_head;
            poisoned_ = true;
            return PageFileResult<PageId>{std::nullopt, std::move(error)};
        }
        if (auto error = write_raw_page(allocated, empty_page)) {
            poisoned_ = true;
            return PageFileResult<PageId>{std::nullopt, std::move(error)};
        }
        return success(allocated);
    }

    if (page_count_ > std::numeric_limits<PageId>::max()) {
        return failure<PageId>(PageFileErrorKind::kInvalidArgument, "PageId space is exhausted");
    }
    const auto allocated = static_cast<PageId>(page_count_);
    if (auto error = write_raw_page(allocated, empty_page)) {
        return PageFileResult<PageId>{std::nullopt, std::move(error)};
    }
    ++page_count_;
    if (auto error = write_header()) {
        --page_count_;
        poisoned_ = true;
        return PageFileResult<PageId>{std::nullopt, std::move(error)};
    }
    return success(allocated);
}

std::optional<PageFileError> PageFile::free_page(PageId page_id) {
    if (auto error = require_open()) {
        return error;
    }
    if (page_id == 0 || std::uint64_t{page_id} >= page_count_) {
        return make_error(PageFileErrorKind::kInvalidArgument,
                          "PageId is outside the data-page range");
    }
    auto already_free = is_free_page(page_id);
    if (!already_free.value.has_value()) {
        return already_free.error;
    }
    if (*already_free.value) {
        return make_error(PageFileErrorKind::kInvalidArgument, "page is already free");
    }

    RawPage free_page;
    std::transform(kFreePageMagic.begin(), kFreePageMagic.end(), free_page.bytes.begin(),
                   [](char value) { return std::byte{static_cast<unsigned char>(value)}; });
    put_u32(free_page, 4, free_page_head_);
    if (auto error = write_raw_page(page_id, free_page)) {
        return error;
    }
    const PageId old_head = free_page_head_;
    free_page_head_ = page_id;
    if (auto error = write_header()) {
        free_page_head_ = old_head;
        poisoned_ = true;
        return error;
    }
    return std::nullopt;
}

std::optional<PageFileError> PageFile::close() {
    if (!open_) {
        return std::nullopt;
    }
    if (take_close_failure_for_testing()) {
        return make_error(PageFileErrorKind::kIo, "injected PageFile close failure");
    }
    // A failed stream operation sets failbit. Clear it before a retry so an
    // still-open file can perform the pending flush/close again. If the stream
    // was actually closed despite reporting failure, the next flush fails and
    // the owner remains retained instead of reporting a false successful close.
    stream_.clear();
    stream_.flush();
    const bool flush_succeeded = stream_.good();
    if (!flush_succeeded) {
        stream_.clear();
        return make_error(PageFileErrorKind::kIo, "cannot close PageFile cleanly");
    }
    stream_.close();
    const bool close_succeeded = !stream_.fail() && !stream_.is_open();
    if (!close_succeeded) {
        stream_.clear();
        return make_error(PageFileErrorKind::kIo, "cannot close PageFile cleanly");
    }
    open_ = false;
    return std::nullopt;
}

}  // namespace tinydbms::storage::internal
