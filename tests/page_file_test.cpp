#include "page_file.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace {

using tinydbms::storage::internal::PageFile;
using tinydbms::storage::internal::PageFileErrorKind;
using tinydbms::storage::internal::PageId;
using tinydbms::storage::internal::RawPage;
using tinydbms::storage::internal::kPageFileFormatVersion;
using tinydbms::storage::internal::kPageFileMagic;
using tinydbms::storage::internal::kPageSize;

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::string label) {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("tinydbms-page-file-" + std::move(label) + "-" + std::to_string(suffix));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "expectation failed: " << message << '\n';
    }
    return condition;
}

template <typename T>
bool has_error(const tinydbms::storage::internal::PageFileResult<T>& result,
               PageFileErrorKind kind) {
    return result.error.has_value() && result.error->kind == kind && !result.value.has_value();
}

bool has_error(const std::optional<tinydbms::storage::internal::PageFileError>& error,
               PageFileErrorKind kind) {
    return error.has_value() && error->kind == kind;
}

void write_u32_le(std::fstream& stream, std::uint64_t offset, std::uint32_t value) {
    std::array<char, 4> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>((value >> (index * 8U)) & 0xffU);
    }
    stream.seekp(static_cast<std::streamoff>(offset));
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
}

void write_u64_le(std::fstream& stream, std::uint64_t offset, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>((value >> (index * 8U)) & 0xffU);
    }
    stream.seekp(static_cast<std::streamoff>(offset));
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
}

bool page_is_zero(const RawPage& page) {
    return std::all_of(page.bytes.begin(), page.bytes.end(),
                       [](std::byte value) { return value == std::byte{0}; });
}

RawPage patterned_page(std::uint8_t seed) {
    RawPage page;
    for (std::size_t index = 0; index < page.bytes.size(); ++index) {
        page.bytes[index] = std::byte{static_cast<std::uint8_t>(seed + index)};
    }
    return page;
}

bool test_create_and_header_layout() {
    TemporaryDirectory directory{"create"};
    const auto path = directory.path() / "table_0.dat";
    auto created = PageFile::create(path);
    bool passed = expect(created.value.has_value() && !created.error.has_value(),
                         "PageFile::create must succeed for a new path");
    if (!created.value.has_value()) {
        return false;
    }
    passed = expect((*created.value)->page_count() == 1,
                    "a new PageFile must contain only header Page 0") &&
             passed;
    passed = expect(std::filesystem::file_size(path) == kPageSize,
                    "a new PageFile must be exactly 4096 bytes") &&
             passed;
    passed = expect(!(*created.value)->close().has_value(), "new PageFile close must succeed") && passed;

    std::array<char, kPageSize> header{};
    std::ifstream input(path, std::ios::binary);
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    passed = expect(input.gcount() == static_cast<std::streamsize>(header.size()),
                    "header read must return exactly one page") &&
             passed;
    passed = expect(std::equal(kPageFileMagic.begin(), kPageFileMagic.end(), header.begin()),
                    "header magic must match the frozen format") &&
             passed;
    const auto version = static_cast<std::uint8_t>(header[8]) |
                         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[9])) << 8U) |
                         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[10])) << 16U) |
                         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[11])) << 24U);
    passed = expect(version == kPageFileFormatVersion, "header version must match") && passed;
    return passed;
}

bool test_allocate_write_read_and_reopen() {
    TemporaryDirectory directory{"roundtrip"};
    const auto path = directory.path() / "table_1.dat";
    auto created = PageFile::create(path);
    if (!expect(created.value.has_value(), "roundtrip PageFile creation must succeed")) {
        return false;
    }
    auto& file = **created.value;
    const auto first = file.allocate_page();
    const auto second = file.allocate_page();
    bool passed = expect(first.value == std::optional<PageId>{1} &&
                             second.value == std::optional<PageId>{2},
                         "consecutive allocation must return PageId 1 then 2");
    const auto zero_page = file.read_page(1);
    passed = expect(zero_page.value.has_value() && page_is_zero(*zero_page.value),
                    "newly allocated page must be zero-filled") &&
             passed;

    const RawPage expected = patterned_page(17);
    passed = expect(!file.write_page(2, expected).has_value(), "4096-byte write must succeed") && passed;
    const auto actual = file.read_page(2);
    passed = expect(actual.value.has_value() && actual.value->bytes == expected.bytes,
                    "4096-byte read/write must round-trip") &&
             passed;
    passed = expect(!file.close().has_value(), "roundtrip close must succeed") && passed;

    auto reopened = PageFile::open(path);
    passed = expect(reopened.value.has_value(), "valid PageFile must reopen") && passed;
    if (reopened.value.has_value()) {
        const auto persisted = (*reopened.value)->read_page(2);
        passed = expect((*reopened.value)->page_count() == 3 && persisted.value.has_value() &&
                            persisted.value->bytes == expected.bytes,
                        "page_count and page contents must persist across reopen") &&
                 passed;
        passed = expect(!(*reopened.value)->close().has_value(), "reopened close must succeed") && passed;
    }
    return passed;
}

bool test_free_list_and_invalid_operations() {
    TemporaryDirectory directory{"free-list"};
    const auto path = directory.path() / "table_2.dat";
    auto created = PageFile::create(path);
    if (!expect(created.value.has_value(), "free-list PageFile creation must succeed")) {
        return false;
    }
    auto& file = **created.value;
    const auto first = file.allocate_page();
    const auto second = file.allocate_page();
    bool passed = expect(first.value.has_value() && second.value.has_value(),
                         "free-list setup allocation must succeed");
    passed = expect(!file.free_page(*first.value).has_value(), "free page must succeed") && passed;
    passed = expect(has_error(file.free_page(*first.value), PageFileErrorKind::kInvalidArgument),
                    "double free must be rejected") &&
             passed;
    passed = expect(has_error(file.free_page(0), PageFileErrorKind::kInvalidArgument),
                    "Page 0 must not be freed") &&
             passed;
    passed = expect(has_error(file.read_page(*first.value), PageFileErrorKind::kInvalidArgument),
                    "free page must not be readable") &&
             passed;
    passed = expect(has_error(file.write_page(*first.value, patterned_page(3)),
                              PageFileErrorKind::kInvalidArgument),
                    "free page must not be writable") &&
             passed;
    passed = expect(has_error(file.read_page(99), PageFileErrorKind::kInvalidArgument),
                    "out-of-range page must not be readable") &&
             passed;
    passed = expect(has_error(file.write_page(0, patterned_page(4)),
                              PageFileErrorKind::kInvalidArgument),
                    "generic write must not modify Page 0") &&
             passed;
    passed = expect(!file.close().has_value(), "free-list close must succeed") && passed;

    auto reopened = PageFile::open(path);
    passed = expect(reopened.value.has_value(), "free list must survive reopen") && passed;
    if (reopened.value.has_value()) {
        const auto reused = (*reopened.value)->allocate_page();
        passed = expect(reused.value == first.value,
                        "allocation after reopen must reuse the free-list head") &&
                 passed;
        if (reused.value.has_value()) {
            const auto cleared = (*reopened.value)->read_page(*reused.value);
            passed = expect(cleared.value.has_value() && page_is_zero(*cleared.value),
                            "reused page must be zero-filled") &&
                     passed;
        }
        passed = expect(!(*reopened.value)->close().has_value(), "free-list final close must succeed") &&
                 passed;
    }
    return passed;
}

bool test_corrupt_files() {
    bool passed = true;

    TemporaryDirectory magic_directory{"bad-magic"};
    const auto magic_path = magic_directory.path() / "bad.dat";
    auto magic_file = PageFile::create(magic_path);
    if (magic_file.value.has_value()) {
        (*magic_file.value)->close();
    }
    {
        std::fstream stream(magic_path, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(0);
        stream.put('X');
    }
    passed = expect(has_error(PageFile::open(magic_path), PageFileErrorKind::kCorrupt),
                    "corrupt magic must be rejected") &&
             passed;

    TemporaryDirectory version_directory{"bad-version"};
    const auto version_path = version_directory.path() / "bad.dat";
    auto version_file = PageFile::create(version_path);
    if (version_file.value.has_value()) {
        (*version_file.value)->close();
    }
    {
        std::fstream stream(version_path, std::ios::binary | std::ios::in | std::ios::out);
        write_u32_le(stream, 8, kPageFileFormatVersion + 1);
    }
    passed = expect(has_error(PageFile::open(version_path), PageFileErrorKind::kCorrupt),
                    "corrupt format version must be rejected") &&
             passed;

    TemporaryDirectory truncated_directory{"truncated"};
    const auto truncated_path = truncated_directory.path() / "bad.dat";
    {
        std::ofstream stream(truncated_path, std::ios::binary);
        stream.write("short", 5);
    }
    passed = expect(has_error(PageFile::open(truncated_path), PageFileErrorKind::kCorrupt),
                    "truncated header must be rejected") &&
             passed;

    TemporaryDirectory count_directory{"bad-count"};
    const auto count_path = count_directory.path() / "bad.dat";
    auto count_file = PageFile::create(count_path);
    if (count_file.value.has_value()) {
        (*count_file.value)->close();
    }
    {
        std::fstream stream(count_path, std::ios::binary | std::ios::in | std::ios::out);
        write_u64_le(stream, 16, 2);
    }
    passed = expect(has_error(PageFile::open(count_path), PageFileErrorKind::kCorrupt),
                    "page_count larger than the file must be rejected") &&
             passed;

    TemporaryDirectory unaligned_directory{"unaligned"};
    const auto unaligned_path = unaligned_directory.path() / "bad.dat";
    auto unaligned_file = PageFile::create(unaligned_path);
    if (unaligned_file.value.has_value()) {
        (*unaligned_file.value)->close();
        std::ofstream stream(unaligned_path, std::ios::binary | std::ios::app);
        stream.put('x');
    }
    passed = expect(has_error(PageFile::open(unaligned_path), PageFileErrorKind::kCorrupt),
                    "non-page-aligned file length must be rejected") &&
             passed;

    TemporaryDirectory free_list_directory{"bad-free-list"};
    const auto free_list_path = free_list_directory.path() / "bad.dat";
    auto free_list_file = PageFile::create(free_list_path);
    if (free_list_file.value.has_value()) {
        const auto page = (*free_list_file.value)->allocate_page();
        if (page.value.has_value()) {
            (*free_list_file.value)->free_page(*page.value);
        }
        (*free_list_file.value)->close();
    }
    {
        std::fstream stream(free_list_path, std::ios::binary | std::ios::in | std::ios::out);
        write_u32_le(stream, kPageSize + 4, 1);
    }
    passed = expect(has_error(PageFile::open(free_list_path), PageFileErrorKind::kCorrupt),
                    "free-list cycle must be rejected") &&
             passed;
    return passed;
}

bool test_aligned_tail_and_independent_files() {
    TemporaryDirectory directory{"independent"};
    const auto first_path = directory.path() / "table_10.dat";
    const auto second_path = directory.path() / "table_11.dat";
    auto first = PageFile::create(first_path);
    auto second = PageFile::create(second_path);
    if (!expect(first.value.has_value() && second.value.has_value(),
                "two independent PageFiles must be creatable")) {
        return false;
    }

    const auto first_id = (*first.value)->allocate_page();
    const auto second_id = (*second.value)->allocate_page();
    const RawPage first_page = patterned_page(31);
    const RawPage second_page = patterned_page(79);
    bool passed = expect(first_id.value == std::optional<PageId>{1} &&
                             second_id.value == std::optional<PageId>{1},
                         "each PageFile must have an independent PageId space");
    passed = expect(!(*first.value)->write_page(1, first_page).has_value() &&
                         !(*second.value)->write_page(1, second_page).has_value(),
                    "independent PageFile writes must succeed") &&
             passed;
    const auto first_read = (*first.value)->read_page(1);
    const auto second_read = (*second.value)->read_page(1);
    passed = expect(first_read.value.has_value() && second_read.value.has_value() &&
                         first_read.value->bytes == first_page.bytes &&
                         second_read.value->bytes == second_page.bytes,
                    "PageFiles must not affect each other's contents") &&
             passed;
    passed = expect(!(*first.value)->close().has_value() && !(*second.value)->close().has_value(),
                    "independent PageFiles must close") &&
             passed;

    std::filesystem::resize_file(first_path, kPageSize * 3);
    auto preallocated = PageFile::open(first_path);
    passed = expect(preallocated.value.has_value() && (*preallocated.value)->page_count() == 2,
                    "aligned physical tail beyond page_count must be allowed") &&
             passed;
    if (preallocated.value.has_value()) {
        passed = expect(!(*preallocated.value)->close().has_value(),
                        "preallocated PageFile close must succeed") &&
                 passed;
    }
    return passed;
}

bool test_filesystem_errors_and_repeated_close() {
    TemporaryDirectory directory{"io"};
    const auto missing = directory.path() / "missing.dat";
    bool passed = expect(has_error(PageFile::open(missing), PageFileErrorKind::kIo),
                         "opening a missing PageFile must return I/O error");

    const auto missing_parent = directory.path() / "missing" / "table.dat";
    passed = expect(has_error(PageFile::create(missing_parent), PageFileErrorKind::kIo),
                    "creating below a missing parent must return I/O error") &&
             passed;

    const auto existing_path = directory.path() / "existing.dat";
    auto existing = PageFile::create(existing_path);
    if (existing.value.has_value()) {
        (*existing.value)->close();
    }
    passed = expect(has_error(PageFile::create(existing_path),
                              PageFileErrorKind::kInvalidArgument),
                    "creating an existing PageFile must be rejected") &&
             passed;

    const auto path = directory.path() / "close.dat";
    auto created = PageFile::create(path);
    passed = expect(created.value.has_value(), "repeated-close PageFile creation must succeed") && passed;
    if (created.value.has_value()) {
        passed = expect(!(*created.value)->close().has_value() &&
                            !(*created.value)->close().has_value(),
                        "PageFile close must be idempotent") &&
                 passed;
    }
    return passed;
}

}  // namespace

int main() {
    const bool passed = test_create_and_header_layout() &&
                        test_allocate_write_read_and_reopen() &&
                        test_free_list_and_invalid_operations() && test_corrupt_files() &&
                        test_aligned_tail_and_independent_files() &&
                        test_filesystem_errors_and_repeated_close();
    return passed ? 0 : 1;
}
