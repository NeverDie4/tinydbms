#include "file_manager.h"
#include "page_file.h"
#include "storage_test_access.h"

#include "tinydbms/storage.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using tinydbms::ColumnMeta;
using tinydbms::TableId;
using tinydbms::Type;
using tinydbms::Value;
using tinydbms::storage::Record;
using tinydbms::storage::StorageErrorKind;
using namespace tinydbms::storage;

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " #condition \
                      << '\n';                                                              \
            return false;                                                                    \
        }                                                                                    \
    } while (false)

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("tinydbms-system-catalog-" + std::to_string(stamp));
        CHECK_OR_THROW(std::filesystem::create_directory(path_));
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    static void CHECK_OR_THROW(bool value) {
        if (!value) {
            throw std::runtime_error{"cannot create temporary directory"};
        }
    }

    std::filesystem::path path_;
};

std::vector<Record> scan(TableId table_id) {
    const auto opened = open_table({table_id});
    if (opened.error.has_value() || !opened.cursor.has_value()) {
        throw std::runtime_error{"cannot open catalog table"};
    }
    std::vector<Record> records;
    while (true) {
        const auto next = scan_next({*opened.cursor});
        if (next.error.has_value()) {
            throw std::runtime_error{"cannot scan catalog table"};
        }
        if (!next.record.has_value()) {
            break;
        }
        records.push_back(*next.record);
    }
    if (close_cursor({*opened.cursor}).error.has_value()) {
        throw std::runtime_error{"cannot close catalog cursor"};
    }
    return records;
}

bool test_bootstrap_system_tables_and_user_catalog_records() {
    TemporaryDirectory directory;
    CHECK(!open_storage({directory.path().string()}).error.has_value());

    const auto tables = list_tables({});
    CHECK(!tables.error.has_value());
    CHECK(tables.tables.size() == 2);
    CHECK(tables.tables[0].table_id == 0 && tables.tables[0].table_name == "tdb_sys_tables");
    CHECK(tables.tables[1].table_id == 1 && tables.tables[1].table_name == "tdb_sys_columns");
    CHECK(std::filesystem::exists(directory.path() / "storage.meta"));
    CHECK(std::filesystem::exists(directory.path() / "tables" / "table_0.dat"));
    CHECK(std::filesystem::exists(directory.path() / "tables" / "table_1.dat"));

    auto* files = tinydbms::storage::internal::StorageTestAccess::file_manager();
    CHECK(files != nullptr);
    CHECK(files->find_table_file(0) != nullptr);
    CHECK(files->find_table_file(1) != nullptr);

    CHECK(!create_table({2, "student", {{"id", Type::kInt}, {"name", Type::kVarchar}}}).error.has_value());
    CHECK(!create_table({3, "course", {{"code", Type::kVarchar}}}).error.has_value());
    CHECK(create_table({0, "reserved_id", {{"id", Type::kInt}}}).error.has_value());
    CHECK(create_table({4, "tdb_sys_tables", {{"id", Type::kInt}}}).error.has_value());

    const auto table_rows = scan(0);
    CHECK(table_rows.size() == 2);
    CHECK(table_rows[0].values.size() == 3);
    CHECK(std::get<std::string>(table_rows[0].values[0].data) == "2");
    CHECK(std::get<std::string>(table_rows[0].values[1].data) == "student");
    CHECK(std::get<std::int32_t>(table_rows[0].values[2].data) == 2);
    CHECK(table_rows[1].values.size() == 3);
    CHECK(std::get<std::string>(table_rows[1].values[0].data) == "3");
    CHECK(std::get<std::string>(table_rows[1].values[1].data) == "course");
    CHECK(std::get<std::int32_t>(table_rows[1].values[2].data) == 1);
    CHECK(scan(1).size() == 3);
    CHECK(!close_storage({}).error.has_value());

    CHECK(!open_storage({directory.path().string()}).error.has_value());
    const auto reopened_tables = list_tables({});
    CHECK(!reopened_tables.error.has_value());
    CHECK(reopened_tables.tables.size() == 4);
    CHECK(!insert({2, {{Value{std::int32_t{7}}, Value{std::string{"Ada"}}}}}).error.has_value());
    CHECK(scan(2).size() == 1);
    CHECK(!close_storage({}).error.has_value());

    CHECK(!open_storage({directory.path().string()}).error.has_value());
    const auto second_reopen_rows = scan(2);
    CHECK(second_reopen_rows.size() == 1);
    CHECK(std::get<std::int32_t>(second_reopen_rows[0].values[0].data) == 7);
    CHECK(std::get<std::string>(second_reopen_rows[0].values[1].data) == "Ada");
    CHECK(!close_storage({}).error.has_value());
    return true;
}

bool test_invalid_or_legacy_bootstrap_is_rejected_without_rewrite() {
    TemporaryDirectory malformed;
    {
        std::ofstream output{malformed.path() / "storage.meta"};
        output << "TINYDBMS_STORAGE_BOOTSTRAP_V2\nSYS_TABLES 0\nSYS_TABLES 1\nEND\n";
    }
    const auto malformed_result = open_storage({malformed.path().string()});
    CHECK(malformed_result.error.has_value());
    CHECK(malformed_result.error->kind == StorageErrorKind::kCorrupt);

    TemporaryDirectory legacy;
    const std::string v1 = "TINYDBMS_STORAGE_V1\nTABLE 2 student\nCOLUMN INT32 id\nENDTABLE\nEND\n";
    {
        std::ofstream output{legacy.path() / "storage.meta"};
        output << v1;
    }
    const auto legacy_result = open_storage({legacy.path().string()});
    CHECK(legacy_result.error.has_value());
    CHECK(legacy_result.error->kind == StorageErrorKind::kInvalidRequest);
    std::ifstream input{legacy.path() / "storage.meta"};
    const std::string after{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    CHECK(after == v1);

    TemporaryDirectory orphan;
    std::filesystem::create_directories(orphan.path() / "tables");
    auto page_file = tinydbms::storage::internal::PageFile::create(
        orphan.path() / "tables" / "table_2.dat");
    CHECK(page_file.value.has_value());
    CHECK(!(*page_file.value)->close().has_value());
    const auto orphan_result = open_storage({orphan.path().string()});
    CHECK(orphan_result.error.has_value());
    CHECK(orphan_result.error->kind == StorageErrorKind::kCorrupt);
    return true;
}

}  // namespace

int main() {
    try {
        return test_bootstrap_system_tables_and_user_catalog_records() &&
                test_invalid_or_legacy_bootstrap_is_rejected_without_rewrite()
            ? 0
            : 1;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
