#include "buffer_pool.h"
#include "file_manager.h"
#include "heap_table.h"
#include "page_file.h"
#include "storage_test_access.h"

#include "tinydbms/storage.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using tinydbms::Type;
using tinydbms::TableMeta;
using tinydbms::Value;
using tinydbms::storage::StorageErrorKind;
using namespace tinydbms::storage;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " #condition \
                      << '\n';                                                            \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
            ("tinydbms-system-catalog-failure-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(path_)) {
            throw std::runtime_error{"cannot create temporary directory"};
        }
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

bool test_reopen_rejects_orphan_user_table_file() {
    TemporaryDirectory directory;
    CHECK(!open_storage({directory.path().string()}).error.has_value());
    CHECK(!create_table({2, "student", {{"id", Type::kInt}}}).error.has_value());
    CHECK(!close_storage({}).error.has_value());

    const auto orphan_path = directory.path() / "tables" / "table_7.dat";
    auto orphan = tinydbms::storage::internal::PageFile::create(orphan_path);
    CHECK(orphan.value.has_value());
    CHECK(!(*orphan.value)->close().has_value());

    const auto reopened = open_storage({directory.path().string()});
    CHECK(reopened.error.has_value());
    CHECK(reopened.error->kind == StorageErrorKind::kCorrupt);
    return true;
}

bool initialize_empty(const std::filesystem::path& path) {
    return !open_storage({path.string()}).error.has_value() &&
        !close_storage({}).error.has_value();
}

bool initialize_student(const std::filesystem::path& path) {
    if (open_storage({path.string()}).error.has_value()) {
        return false;
    }
    CreateTableRequest request;
    request.table_id = 2;
    request.table_name = "student";
    request.columns = {{"id", Type::kInt}, {"name", Type::kVarchar}};
    return !create_table(request).error.has_value() && !close_storage({}).error.has_value();
}

bool reopen_rejected(const std::filesystem::path& path) {
    const auto reopened = open_storage({path.string()});
    if (!reopened.error.has_value()) {
        (void)close_storage({});
        return false;
    }
    return reopened.error->kind == StorageErrorKind::kCorrupt ||
        reopened.error->kind == StorageErrorKind::kIoError;
}

bool overwrite_byte(const std::filesystem::path& path, std::streamoff offset, char value) {
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream) {
        return false;
    }
    stream.seekp(offset);
    stream.put(value);
    stream.flush();
    return static_cast<bool>(stream);
}

bool test_bootstrap_corruption_is_not_rewritten() {
    const std::vector<std::string> corrupt_bootstraps{
        "WRONG\nSYS_TABLES 0\nSYS_COLUMNS 1\nEND\n",
        "TINYDBMS_STORAGE_BOOTSTRAP_V3\nSYS_TABLES 0\nSYS_COLUMNS 1\nEND\n",
        "TINYDBMS_STORAGE_BOOTSTRAP_V2\nSYS_TABLES 2\nSYS_COLUMNS 1\nEND\n",
        "TINYDBMS_STORAGE_BOOTSTRAP_V2\nSYS_TABLES 0\nSYS_COLUMNS 2\nEND\n",
        "TINYDBMS_STORAGE_BOOTSTRAP_V2\nSYS_TABLES 0\nSYS_TABLES 1\nEND\n",
        "TINYDBMS_STORAGE_BOOTSTRAP_V2\nSYS_TABLES 0\n",
        "TINYDBMS_STORAGE_BOOTSTRAP_V2\nSYS_TABLES 0 extra\nSYS_COLUMNS 1\nEND\n",
        "TINYDBMS_STORAGE_BOOTSTRAP_V2\nSYS_TABLES 0\nSYS_COLUMNS 1\n"};
    for (const std::string& bootstrap : corrupt_bootstraps) {
        TemporaryDirectory directory;
        const auto meta = directory.path() / "storage.meta";
        { std::ofstream output(meta); output << bootstrap; }
        CHECK(reopen_rejected(directory.path()));
        std::ifstream input(meta);
        const std::string after{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        CHECK(after == bootstrap);
    }
    return true;
}

bool test_system_table_physical_corruption_is_rejected() {
    {
        TemporaryDirectory directory;
        CHECK(initialize_empty(directory.path()));
        std::error_code error;
        CHECK(std::filesystem::remove(directory.path() / "tables" / "table_0.dat", error) && !error);
        CHECK(reopen_rejected(directory.path()));
    }
    {
        TemporaryDirectory directory;
        CHECK(initialize_empty(directory.path()));
        std::error_code error;
        CHECK(std::filesystem::remove(directory.path() / "tables" / "table_1.dat", error) && !error);
        CHECK(reopen_rejected(directory.path()));
    }
    {
        TemporaryDirectory directory;
        CHECK(initialize_empty(directory.path()));
        std::filesystem::resize_file(directory.path() / "tables" / "table_0.dat", 1);
        CHECK(reopen_rejected(directory.path()));
    }
    {
        TemporaryDirectory directory;
        CHECK(initialize_empty(directory.path()));
        CHECK(overwrite_byte(directory.path() / "tables" / "table_1.dat", 0, 'X'));
        CHECK(reopen_rejected(directory.path()));
    }
    {
        TemporaryDirectory directory;
        CHECK(initialize_student(directory.path()));
        // SlottedPage slot_count is at page 1 + byte offset 12; 65535 is invalid for a 4 KiB page.
        CHECK(overwrite_byte(directory.path() / "tables" / "table_0.dat", 4096 + 12, static_cast<char>(0xff)));
        CHECK(overwrite_byte(directory.path() / "tables" / "table_0.dat", 4096 + 13, static_cast<char>(0xff)));
        CHECK(reopen_rejected(directory.path()));
    }
    return true;
}

struct CatalogTableRow { std::string id; std::string name; std::int32_t column_count; };
struct CatalogColumnRow { std::string id; std::int32_t ordinal; std::string name; std::string type; };

bool write_catalog_rows(const std::filesystem::path& path,
                        const std::vector<CatalogTableRow>& table_rows,
                        const std::vector<CatalogColumnRow>& column_rows) {
    using namespace tinydbms::storage::internal;
    auto files = FileManager::open(path);
    if (!files.value.has_value() || !(*files.value)->open_table_file(0).value.has_value() ||
        !(*files.value)->open_table_file(1).value.has_value()) {
        return false;
    }
    auto pool = BufferPool::create(**files.value, 8);
    if (!pool.value.has_value()) {
        return false;
    }
    const TableMeta tables_meta{0, "tdb_sys_tables", {{"table_id", Type::kVarchar},
        {"table_name", Type::kVarchar}, {"column_count", Type::kInt}}};
    const TableMeta columns_meta{1, "tdb_sys_columns", {{"table_id", Type::kVarchar},
        {"column_ordinal", Type::kInt}, {"column_name", Type::kVarchar}, {"column_type", Type::kVarchar}}};
    HeapTable tables(tables_meta, **files.value, **pool.value);
    HeapTable columns(columns_meta, **files.value, **pool.value);
    for (const CatalogTableRow& row : table_rows) {
        if (tables.insert_record({Value{row.id}, Value{row.name}, Value{row.column_count}}).error.has_value()) {
            return false;
        }
    }
    for (const CatalogColumnRow& row : column_rows) {
        if (columns.insert_record({Value{row.id}, Value{row.ordinal}, Value{row.name}, Value{row.type}}).error.has_value()) {
            return false;
        }
    }
    return !(*pool.value)->close().has_value() && !(*files.value)->close_all().has_value();
}

bool logical_catalog_rejected(const std::vector<CatalogTableRow>& tables,
                              const std::vector<CatalogColumnRow>& columns) {
    TemporaryDirectory directory;
    return initialize_empty(directory.path()) && write_catalog_rows(directory.path(), tables, columns) &&
        reopen_rejected(directory.path());
}

bool test_logical_catalog_corruption_is_rejected() {
    CHECK(logical_catalog_rejected({{"2", "student", 1}, {"2", "other", 1}}, {}));
    CHECK(logical_catalog_rejected({{"2", "student", 1}, {"3", "student", 1}}, {}));
    CHECK(logical_catalog_rejected({{"0", "user_zero", 1}}, {}));
    CHECK(logical_catalog_rejected({{"1", "user_one", 1}}, {}));
    CHECK(logical_catalog_rejected({{"2", "tdb_sys_tables", 1}}, {}));
    CHECK(logical_catalog_rejected({{"2", "tdb_sys_columns", 1}}, {}));
    CHECK(logical_catalog_rejected({{"2", "student", 3}}, {{"2", 0, "a", "INT32"}, {"2", 1, "b", "VARCHAR"}}));
    CHECK(logical_catalog_rejected({{"2", "student", 2}}, {{"2", 0, "a", "INT32"}, {"2", 1, "b", "VARCHAR"}, {"2", 2, "c", "INT32"}}));
    CHECK(logical_catalog_rejected({{"2", "student", 2}}, {{"2", 0, "a", "INT32"}, {"2", 2, "b", "VARCHAR"}}));
    CHECK(logical_catalog_rejected({{"2", "student", 3}}, {{"2", 0, "a", "INT32"}, {"2", 1, "b", "VARCHAR"}, {"2", 1, "c", "INT32"}}));
    CHECK(logical_catalog_rejected({{"2", "student", 1}}, {{"2", 0, "a", "INT64"}}));
    CHECK(logical_catalog_rejected({}, {{"99", 0, "a", "INT32"}}));
    return true;
}

bool test_catalog_file_consistency_is_enforced() {
    {
        TemporaryDirectory directory;
        CHECK(initialize_student(directory.path()));
        std::error_code error;
        CHECK(std::filesystem::remove(directory.path() / "tables" / "table_2.dat", error) && !error);
        CHECK(reopen_rejected(directory.path()));
    }
    {
        TemporaryDirectory directory;
        CHECK(initialize_student(directory.path()));
        { std::ofstream note(directory.path() / "tables" / "notes.txt"); note << "not a table file"; }
        CHECK(!open_storage({directory.path().string()}).error.has_value());
        CHECK(!close_storage({}).error.has_value());
    }
    return true;
}

bool test_create_catalog_write_failure_rolls_back() {
    TemporaryDirectory directory;
    bool fail_columns_flush = true;
    CHECK(tinydbms::storage::internal::StorageTestAccess::configure(
        1, tinydbms::storage::internal::ReplacementPolicy::kFifo,
        [&](tinydbms::storage::internal::PageKey key,
            const tinydbms::storage::internal::RawPage& page)
            -> std::optional<tinydbms::storage::internal::PageFileError> {
            if (key.table_id == 1 && fail_columns_flush) {
                fail_columns_flush = false;
                return tinydbms::storage::internal::PageFileError{
                    tinydbms::storage::internal::PageFileErrorKind::kIo,
                    "injected sys_columns write failure"};
            }
            auto* files = tinydbms::storage::internal::StorageTestAccess::file_manager();
            return files->find_table_file(key.table_id)->write_page(key.page_id, page);
        }));
    CHECK(!open_storage({directory.path().string()}).error.has_value());
    const auto failed = create_table({2, "student", {{"id", Type::kInt}, {"name", Type::kVarchar}}});
    CHECK(failed.error.has_value());
    CHECK(list_tables({}).tables.size() == 2);
    CHECK(!std::filesystem::exists(directory.path() / "tables" / "table_2.dat"));
    CHECK(!close_storage({}).error.has_value());
    CHECK(tinydbms::storage::internal::StorageTestAccess::configure());
    CHECK(!open_storage({directory.path().string()}).error.has_value());
    CHECK(list_tables({}).tables.size() == 2);
    CHECK(!close_storage({}).error.has_value());
    return true;
}

bool test_create_catalog_mid_batch_failure_rolls_back() {
    TemporaryDirectory directory;
    bool fail_columns_flush = true;
    CHECK(tinydbms::storage::internal::StorageTestAccess::configure(
        1, tinydbms::storage::internal::ReplacementPolicy::kFifo,
        [&](tinydbms::storage::internal::PageKey key,
            const tinydbms::storage::internal::RawPage& page)
            -> std::optional<tinydbms::storage::internal::PageFileError> {
            if (key.table_id == 1 && fail_columns_flush) {
                fail_columns_flush = false;
                return tinydbms::storage::internal::PageFileError{
                    tinydbms::storage::internal::PageFileErrorKind::kIo,
                    "injected mid-batch sys_columns write failure"};
            }
            auto* files = tinydbms::storage::internal::StorageTestAccess::file_manager();
            return files->find_table_file(key.table_id)->write_page(key.page_id, page);
        }));
    CHECK(!open_storage({directory.path().string()}).error.has_value());
    CreateTableRequest request;
    request.table_id = 2;
    request.table_name = "wide_student";
    for (std::int32_t index = 0; index < 200; ++index) {
        request.columns.push_back({"column" + std::to_string(index), Type::kVarchar});
    }
    const auto failed = create_table(request);
    CHECK(failed.error.has_value());
    CHECK(list_tables({}).tables.size() == 2);
    CHECK(!std::filesystem::exists(directory.path() / "tables" / "table_2.dat"));
    CHECK(!close_storage({}).error.has_value());
    CHECK(tinydbms::storage::internal::StorageTestAccess::configure());
    CHECK(!open_storage({directory.path().string()}).error.has_value());
    CHECK(list_tables({}).tables.size() == 2);
    CHECK(!close_storage({}).error.has_value());
    return true;
}

bool test_create_commit_marker_write_failure_rolls_back() {
    TemporaryDirectory directory;
    bool fail_marker_flush = true;
    CHECK(tinydbms::storage::internal::StorageTestAccess::configure(
        1, tinydbms::storage::internal::ReplacementPolicy::kFifo,
        [&](tinydbms::storage::internal::PageKey key,
            const tinydbms::storage::internal::RawPage& page)
            -> std::optional<tinydbms::storage::internal::PageFileError> {
            if (key.table_id == 0 && fail_marker_flush) {
                fail_marker_flush = false;
                return tinydbms::storage::internal::PageFileError{
                    tinydbms::storage::internal::PageFileErrorKind::kIo,
                    "injected sys_tables commit-marker write failure"};
            }
            auto* files = tinydbms::storage::internal::StorageTestAccess::file_manager();
            return files->find_table_file(key.table_id)->write_page(key.page_id, page);
        }));
    CHECK(!open_storage({directory.path().string()}).error.has_value());
    const auto failed = create_table({2, "student", {{"id", Type::kInt}}});
    CHECK(failed.error.has_value());
    CHECK(list_tables({}).tables.size() == 2);
    CHECK(!std::filesystem::exists(directory.path() / "tables" / "table_2.dat"));
    CHECK(!close_storage({}).error.has_value());
    CHECK(tinydbms::storage::internal::StorageTestAccess::configure());
    CHECK(!open_storage({directory.path().string()}).error.has_value());
    CHECK(list_tables({}).tables.size() == 2);
    CHECK(!close_storage({}).error.has_value());
    return true;
}

bool test_create_compensation_failure_is_fail_closed() {
    TemporaryDirectory directory;
    bool fail_marker_flush = true;
    CHECK(tinydbms::storage::internal::StorageTestAccess::configure(
        1, tinydbms::storage::internal::ReplacementPolicy::kFifo,
        [&](tinydbms::storage::internal::PageKey key,
            const tinydbms::storage::internal::RawPage& page)
            -> std::optional<tinydbms::storage::internal::PageFileError> {
            if (key.table_id == 0 && fail_marker_flush) {
                return tinydbms::storage::internal::PageFileError{
                    tinydbms::storage::internal::PageFileErrorKind::kIo,
                    "injected persistent sys_tables write failure"};
            }
            auto* files = tinydbms::storage::internal::StorageTestAccess::file_manager();
            return files->find_table_file(key.table_id)->write_page(key.page_id, page);
        }));
    CHECK(!open_storage({directory.path().string()}).error.has_value());
    const auto failed = create_table({2, "student", {{"id", Type::kInt}}});
    CHECK(failed.error.has_value());
    CHECK(list_tables({}).tables.size() == 2);
    fail_marker_flush = false;
    CHECK(!close_storage({}).error.has_value());
    CHECK(tinydbms::storage::internal::StorageTestAccess::configure());
    CHECK(reopen_rejected(directory.path()));
    return true;
}

}  // namespace

int main() {
    try {
        return test_reopen_rejects_orphan_user_table_file() &&
                test_bootstrap_corruption_is_not_rewritten() &&
                test_system_table_physical_corruption_is_rejected() &&
                test_logical_catalog_corruption_is_rejected() &&
                test_catalog_file_consistency_is_enforced() &&
                test_create_catalog_write_failure_rolls_back() &&
                test_create_catalog_mid_batch_failure_rolls_back() &&
                test_create_commit_marker_write_failure_rolls_back() &&
                test_create_compensation_failure_is_fail_closed()
            ? 0 : 1;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
