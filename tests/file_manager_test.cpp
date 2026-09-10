#include "file_manager.h"

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

using tinydbms::TableId;
using tinydbms::storage::internal::FileManager;
using tinydbms::storage::internal::PageFileErrorKind;
using tinydbms::storage::internal::PageId;
using tinydbms::storage::internal::RawPage;

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::string label) {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("tinydbms-file-manager-" + std::move(label) + "-" +
                 std::to_string(suffix));
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
    return !result.value.has_value() && result.error.has_value() && result.error->kind == kind;
}

RawPage filled_page(std::uint8_t value) {
    RawPage page;
    page.bytes.fill(std::byte{value});
    return page;
}

bool test_create_open_find_and_close() {
    TemporaryDirectory directory{"lifecycle"};
    auto opened = FileManager::open(directory.path());
    if (!expect(opened.value.has_value(), "FileManager must initialize tables directory")) {
        return false;
    }
    auto& manager = **opened.value;
    bool passed = expect(std::filesystem::is_directory(directory.path() / "tables"),
                         "FileManager must own tables directory creation");

    const auto table0 = manager.create_table_file(TableId{0});
    const auto table1 = manager.create_table_file(TableId{1});
    passed = expect(table0.value.has_value() && table1.value.has_value(),
                    "TableId 0 and 1 files must be creatable") && passed;
    passed = expect(std::filesystem::exists(directory.path() / "tables" / "table_0.dat") &&
                        std::filesystem::exists(directory.path() / "tables" / "table_1.dat"),
                    "table files must use stable TableId names") && passed;
    passed = expect(manager.find_table_file(0) == table0.value.value_or(nullptr) &&
                        manager.find_table_file(1) == table1.value.value_or(nullptr),
                    "find_table_file must return the owned opened PageFile") && passed;
    passed = expect(has_error(manager.create_table_file(0), PageFileErrorKind::kInvalidArgument),
                    "duplicate create must be rejected") && passed;

    passed = expect(!manager.close_table_file(0).has_value() &&
                        manager.find_table_file(0) == nullptr,
                    "close_table_file must close and release one PageFile") && passed;
    passed = expect(!manager.close_table_file(0).has_value(),
                    "repeated close_table_file must be successful") && passed;
    const auto reopened = manager.open_table_file(0);
    passed = expect(reopened.value.has_value() && manager.find_table_file(0) == *reopened.value,
                    "open_table_file must reopen an existing table file") && passed;
    passed = expect(has_error(manager.open_table_file(99), PageFileErrorKind::kInvalidArgument),
                    "opening a missing table file must be rejected") && passed;

    passed = expect(!manager.close_all().has_value() && manager.find_table_file(0) == nullptr &&
                        manager.find_table_file(1) == nullptr,
                    "close_all must close every owned PageFile") && passed;
    passed = expect(!manager.close_all().has_value(), "repeated close_all must succeed") && passed;
    return passed;
}

bool test_table_files_are_isolated() {
    TemporaryDirectory directory{"isolation"};
    auto opened = FileManager::open(directory.path());
    if (!expect(opened.value.has_value(), "isolation FileManager must open")) {
        return false;
    }
    auto& manager = **opened.value;
    const auto table0 = manager.create_table_file(0);
    const auto table1 = manager.create_table_file(1);
    if (!expect(table0.value.has_value() && table1.value.has_value(),
                "isolation table files must be created")) {
        return false;
    }

    const auto page0 = (*table0.value)->allocate_page();
    const auto page1 = (*table1.value)->allocate_page();
    bool passed = expect(page0.value == std::optional<PageId>{1} &&
                             page1.value == std::optional<PageId>{1},
                         "each table file must have an independent PageId space");
    const RawPage zeros = filled_page(0);
    const RawPage ones = filled_page(1);
    passed = expect(!(*table0.value)->write_page(1, zeros).has_value() &&
                        !(*table1.value)->write_page(1, ones).has_value(),
                    "writes to independent table files must succeed") && passed;
    const auto read0 = (*table0.value)->read_page(1);
    const auto read1 = (*table1.value)->read_page(1);
    passed = expect(read0.value.has_value() && read1.value.has_value() &&
                        read0.value->bytes == zeros.bytes && read1.value->bytes == ones.bytes,
                    "table page contents must remain isolated") && passed;
    passed = expect(!(*table0.value)->free_page(1).has_value(),
                    "freeing a page in table 0 must succeed") && passed;
    const auto still_readable = (*table1.value)->read_page(1);
    passed = expect(still_readable.value.has_value() && still_readable.value->bytes == ones.bytes,
                    "freeing table 0 page must not affect table 1") && passed;
    passed = expect(!manager.close_all().has_value(), "isolation close_all must succeed") && passed;
    return passed;
}

bool test_filesystem_errors() {
    TemporaryDirectory directory{"filesystem"};
    const auto file_path = directory.path() / "not-a-directory";
    {
        std::ofstream output(file_path);
        output << "file";
    }
    return expect(has_error(FileManager::open(file_path), PageFileErrorKind::kIo),
                  "a non-directory data path must return kIo");
}

}  // namespace

int main() {
    const bool passed = test_create_open_find_and_close() && test_table_files_are_isolated() &&
                        test_filesystem_errors();
    return passed ? 0 : 1;
}
