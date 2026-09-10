#include "page_file.h"
#include "tinydbms/storage.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using tinydbms::ColumnMeta;
using tinydbms::Type;
using tinydbms::storage::StorageError;
using tinydbms::storage::StorageErrorKind;
using tinydbms::storage::internal::PageFile;

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::string label) {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("tinydbms-storage-page-file-" + std::move(label) + "-" +
                 std::to_string(suffix));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    ~TemporaryDirectory() {
        (void)tinydbms::storage::close_storage({});
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path table_file(std::uint32_t table_id) const {
        return path_ / "tables" / ("table_" + std::to_string(table_id) + ".dat");
    }

private:
    std::filesystem::path path_;
};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "expectation failed: " << message << '\n';
    }
    return condition;
}

bool has_error(const std::optional<StorageError>& error, StorageErrorKind kind) {
    return error.has_value() && error->kind == kind;
}

std::vector<ColumnMeta> columns() {
    return {{"id", Type::kInt}};
}

bool test_create_close_and_reopen() {
    TemporaryDirectory directory{"lifecycle"};
    bool passed = expect(!tinydbms::storage::open_storage({directory.path().string()}).error,
                         "storage must open");
    passed = expect(!tinydbms::storage::create_table({0, "zero", columns()}).error &&
                        !tinydbms::storage::create_table({1, "one", columns()}).error,
                    "TableId 0 and 1 must create table files") && passed;
    passed = expect(std::filesystem::exists(directory.path() / "storage.meta") &&
                        std::filesystem::exists(directory.table_file(0)) &&
                        std::filesystem::exists(directory.table_file(1)),
                    "successful create must commit metadata and both table files") && passed;
    std::error_code filesystem_error;
    const auto table0_size = std::filesystem::file_size(directory.table_file(0), filesystem_error);
    const bool table0_valid = !filesystem_error && table0_size == 4096;
    filesystem_error.clear();
    const auto table1_size = std::filesystem::file_size(directory.table_file(1), filesystem_error);
    passed = expect(table0_valid && !filesystem_error && table1_size == 4096,
                    "new table files must contain a valid header page") && passed;
    passed = expect(!tinydbms::storage::close_storage({}).error,
                    "close_storage must close all PageFiles") && passed;

    auto page0 = PageFile::open(directory.table_file(0));
    auto page1 = PageFile::open(directory.table_file(1));
    passed = expect(page0.value.has_value() && page1.value.has_value(),
                    "closed table files must independently reopen as valid PageFiles") && passed;
    if (page0.value) {
        (*page0.value)->close();
    }
    if (page1.value) {
        (*page1.value)->close();
    }

    passed = expect(!tinydbms::storage::open_storage({directory.path().string()}).error,
                    "storage must reopen only after validating all table files") && passed;
    const auto tables = tinydbms::storage::list_tables({});
    passed = expect(!tables.error && tables.tables.size() == 2,
                    "reopen must restore both catalog entries") && passed;
    passed = expect(!tinydbms::storage::close_storage({}).error, "final close must succeed") && passed;
    return passed;
}

bool test_missing_and_corrupt_table_files() {
    bool passed = true;
    TemporaryDirectory missing{"missing"};
    passed = expect(!tinydbms::storage::open_storage({missing.path().string()}).error &&
                        !tinydbms::storage::create_table({0, "missing", columns()}).error &&
                        !tinydbms::storage::close_storage({}).error,
                    "missing-file setup must succeed") && passed;
    std::error_code filesystem_error;
    std::filesystem::remove(missing.table_file(0), filesystem_error);
    passed = expect(!filesystem_error, "missing-file setup removal must succeed") && passed;
    passed = expect(has_error(tinydbms::storage::open_storage({missing.path().string()}).error,
                              StorageErrorKind::kCorrupt),
                    "metadata-declared missing table file must be corrupt") && passed;

    TemporaryDirectory corrupt{"corrupt"};
    passed = expect(!tinydbms::storage::open_storage({corrupt.path().string()}).error &&
                        !tinydbms::storage::create_table({0, "corrupt", columns()}).error &&
                        !tinydbms::storage::close_storage({}).error,
                    "corrupt-file setup must succeed") && passed;
    {
        std::fstream stream(corrupt.table_file(0),
                            std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(0);
        stream.put('X');
    }
    passed = expect(has_error(tinydbms::storage::open_storage({corrupt.path().string()}).error,
                              StorageErrorKind::kCorrupt),
                    "invalid PageFile header must make storage corrupt") && passed;
    return passed;
}

bool test_orphan_and_table_file_creation_failure() {
    TemporaryDirectory orphan{"orphan"};
    bool passed = expect(!tinydbms::storage::open_storage({orphan.path().string()}).error,
                         "orphan storage must open");
    auto orphan_file = PageFile::create(orphan.table_file(7));
    passed = expect(orphan_file.value.has_value(), "orphan setup must create a valid PageFile") && passed;
    if (orphan_file.value) {
        (*orphan_file.value)->close();
    }
    passed = expect(has_error(tinydbms::storage::create_table({7, "orphan", columns()}).error,
                              StorageErrorKind::kCorrupt),
                    "create_table must not overwrite an orphan table file") && passed;
    passed = expect(tinydbms::storage::list_tables({}).tables.empty(),
                    "orphan rejection must not change metadata") && passed;
    passed = expect(!tinydbms::storage::close_storage({}).error, "orphan storage must close") && passed;

    TemporaryDirectory blocked{"blocked"};
    passed = expect(!tinydbms::storage::open_storage({blocked.path().string()}).error,
                    "blocked storage must open") && passed;
    std::error_code filesystem_error;
    std::filesystem::remove(blocked.path() / "tables", filesystem_error);
    {
        std::ofstream blocker(blocked.path() / "tables");
        blocker << "not a directory";
    }
    passed = expect(has_error(tinydbms::storage::create_table({0, "blocked", columns()}).error,
                              StorageErrorKind::kIoError),
                    "table-file creation failure must return kIoError") && passed;
    passed = expect(tinydbms::storage::list_tables({}).tables.empty() &&
                        !std::filesystem::exists(blocked.path() / "storage.meta"),
                    "table-file creation failure must not commit metadata") && passed;
    passed = expect(!tinydbms::storage::close_storage({}).error, "blocked storage must close") && passed;
    return passed;
}

bool test_metadata_failure_rolls_back_table_file() {
    TemporaryDirectory directory{"metadata-rollback"};
    bool passed = expect(!tinydbms::storage::open_storage({directory.path().string()}).error,
                         "rollback storage must open");
    const auto temporary_metadata = directory.path() / "storage.meta.tmp";
    std::filesystem::create_directories(temporary_metadata);
    {
        std::ofstream lock(temporary_metadata / "lock");
        lock << "block metadata write";
    }
    passed = expect(has_error(tinydbms::storage::create_table({0, "rollback", columns()}).error,
                              StorageErrorKind::kIoError),
                    "metadata commit failure must return kIoError") && passed;
    passed = expect(tinydbms::storage::list_tables({}).tables.empty() &&
                        !std::filesystem::exists(directory.table_file(0)),
                    "metadata failure must roll back catalog and new table file") && passed;

    std::error_code filesystem_error;
    std::filesystem::remove_all(temporary_metadata, filesystem_error);
    passed = expect(!filesystem_error && !tinydbms::storage::close_storage({}).error,
                    "storage must remain closeable after successful rollback") && passed;
    passed = expect(!tinydbms::storage::open_storage({directory.path().string()}).error,
                    "old metadata state must remain reopenable") && passed;
    passed = expect(tinydbms::storage::list_tables({}).tables.empty(),
                    "failed table must not appear after reopen") && passed;
    passed = expect(!tinydbms::storage::close_storage({}).error, "rollback final close must succeed") && passed;
    return passed;
}

}  // namespace

int main() {
    const bool passed = test_create_close_and_reopen() && test_missing_and_corrupt_table_files() &&
                        test_orphan_and_table_file_creation_failure() &&
                        test_metadata_failure_rolls_back_table_file();
    return passed ? 0 : 1;
}
