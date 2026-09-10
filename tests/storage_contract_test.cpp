#include "tinydbms/storage.h"

#include "page_file.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace {

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::string suffix)
        : path_(std::filesystem::temp_directory_path() / ("tinydbms-storage-contract-" + suffix)) {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
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

bool has_error(const std::optional<tinydbms::StorageError>& error,
               tinydbms::StorageErrorKind kind) {
    return error.has_value() && error->kind == kind;
}

bool same_columns(const std::vector<tinydbms::ColumnMeta>& left,
                  const std::vector<tinydbms::ColumnMeta>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].name != right[index].name || left[index].type != right[index].type) {
            return false;
        }
    }
    return true;
}

std::vector<tinydbms::ColumnMeta> all_supported_columns() {
    return {
        {"int32_value", tinydbms::Type::kInt32},
        {"int64_value", tinydbms::Type::kInt64},
        {"float_value", tinydbms::Type::kFloat},
        {"double_value", tinydbms::Type::kDouble},
        {"bool_value", tinydbms::Type::kBool},
        {"varchar_value", tinydbms::Type::kVarchar},
    };
}

bool test_value_type_contract() {
    using tinydbms::Type;
    using tinydbms::Value;

    const std::vector<std::pair<Type, Value>> values = {
        {Type::kInt32, Value{std::int32_t{1}}},
        {Type::kInt64, Value{std::int64_t{2}}},
        {Type::kFloat, Value{3.0F}},
        {Type::kDouble, Value{4.0}},
        {Type::kBool, Value{true}},
        {Type::kVarchar, Value{std::string{"text"}}},
    };

    bool passed = expect(std::variant_size_v<decltype(Value{}.data)> == values.size(),
                         "Value variant must contain exactly one alternative per Type");
    for (const auto& [type, value] : values) {
        passed = expect(tinydbms::value_matches_type(type, value),
                        "each Type must match its corresponding Value alternative") &&
                 passed;
    }
    passed = expect(!tinydbms::value_matches_type(Type::kInt32, Value{std::int64_t{1}}),
                    "mismatched Value and ColumnMeta type must be detectable") &&
             passed;
    passed = expect(tinydbms::kMaxVarcharBytes == 1024 && tinydbms::kMaxRowLogicalBytes == 4096,
                    "VARCHAR and logical row limits must remain frozen") &&
             passed;
    return passed;
}

bool test_lifecycle_and_metadata_recovery() {
    TemporaryDirectory directory{"lifecycle"};
    bool passed = true;

    passed = expect(has_error(tinydbms::storage::list_tables().error,
                              tinydbms::StorageErrorKind::kInvalidRequest),
                    "list_tables before open must be invalid") &&
             passed;
    passed = expect(has_error(tinydbms::storage::open_table({0}).error,
                              tinydbms::StorageErrorKind::kInvalidRequest),
                    "open_table before open must be invalid") &&
             passed;

    passed = expect(!tinydbms::storage::open_storage({directory.path().string()}).error.has_value(),
                    "open_storage must create an absent data directory") &&
             passed;
    passed = expect(has_error(tinydbms::storage::open_storage({directory.path().string()}).error,
                              tinydbms::StorageErrorKind::kInvalidRequest),
                    "double open must be invalid") &&
             passed;

    const auto columns = all_supported_columns();
    passed = expect(!tinydbms::storage::create_table({0, "types", columns}).error.has_value(),
                    "TableId 0 must be valid") &&
             passed;
    passed = expect(has_error(tinydbms::storage::create_table({0, "other", columns}).error,
                              tinydbms::StorageErrorKind::kInvalidRequest),
                    "duplicate TableId must be invalid") &&
             passed;
    passed = expect(has_error(tinydbms::storage::create_table({1, "types", columns}).error,
                              tinydbms::StorageErrorKind::kInvalidRequest),
                    "duplicate table name must be invalid") &&
             passed;
    passed = expect(has_error(tinydbms::storage::create_table({1, "", columns}).error,
                              tinydbms::StorageErrorKind::kInvalidRequest),
                    "empty table name must be invalid") &&
             passed;
    passed = expect(has_error(tinydbms::storage::create_table({1, "empty_columns", {}}).error,
                              tinydbms::StorageErrorKind::kInvalidRequest),
                    "empty columns must be invalid") &&
             passed;

    const auto tables = tinydbms::storage::list_tables();
    passed = expect(!tables.error.has_value() && tables.tables.size() == 1 &&
                         tables.tables.front().table_id == 0 &&
                         same_columns(tables.tables.front().columns, columns),
                    "list_tables must expose the TableId 0 schema") &&
             passed;

    passed = expect(!tinydbms::storage::close_storage().error.has_value(),
                    "close_storage must persist metadata") &&
             passed;
    passed = expect(!tinydbms::storage::close_storage().error.has_value(),
                    "repeated close must remain successful") &&
             passed;
    passed = expect(has_error(tinydbms::storage::list_tables().error,
                              tinydbms::StorageErrorKind::kInvalidRequest),
                    "list_tables after close must be invalid") &&
             passed;
    passed = expect(!tinydbms::storage::open_storage({directory.path().string()}).error.has_value(),
                    "reopen must recover valid metadata") &&
             passed;

    const auto recovered = tinydbms::storage::list_tables();
    passed = expect(!recovered.error.has_value() && recovered.tables.size() == 1 &&
                         recovered.tables.front().table_id == 0 &&
                         same_columns(recovered.tables.front().columns, columns),
                    "metadata recovery must preserve every supported Type") &&
             passed;
    passed = expect(!tinydbms::storage::close_storage().error.has_value(),
                    "final close must succeed") &&
             passed;
    return passed;
}

bool test_schema_validation_contract() {
    TemporaryDirectory directory{"schema-validation"};
    const auto columns = all_supported_columns();
    bool passed = expect(!tinydbms::storage::open_storage({directory.path().string()}).error.has_value(),
                          "schema validation test open must succeed");
    passed = expect(has_error(tinydbms::storage::create_table({0, "Uppercase", columns}).error,
                              tinydbms::StorageErrorKind::kInvalidRequest),
                    "storage must reject non-normalized table names") &&
             passed;
    passed = expect(has_error(tinydbms::storage::create_table(
                                  {0, "duplicate_columns", {{"id", tinydbms::Type::kInt32},
                                                           {"id", tinydbms::Type::kVarchar}}})
                                  .error,
                              tinydbms::StorageErrorKind::kInvalidRequest),
                    "storage must reject duplicate column names") &&
             passed;
    passed = expect(!tinydbms::storage::close_storage().error.has_value(),
                    "schema validation test close must succeed") &&
             passed;
    return passed;
}

bool test_corruption_and_io_errors() {
    TemporaryDirectory corrupt{"corrupt"};
    std::filesystem::create_directories(corrupt.path());
    {
        std::ofstream output(corrupt.path() / "storage.meta");
        output << "not tinydbms metadata\n";
    }

    bool passed = expect(has_error(tinydbms::storage::open_storage({corrupt.path().string()}).error,
                                    tinydbms::StorageErrorKind::kCorrupt),
                          "invalid metadata must return kCorrupt");

    TemporaryDirectory file_path{"file"};
    {
        std::ofstream output(file_path.path());
        output << "not a directory";
    }
    passed = expect(has_error(tinydbms::storage::open_storage({file_path.path().string()}).error,
                              tinydbms::StorageErrorKind::kIoError),
                    "a data path that is a file must return kIoError") &&
             passed;

    TemporaryDirectory legacy{"legacy"};
    std::filesystem::create_directories(legacy.path());
    std::filesystem::create_directories(legacy.path() / "tables");
    {
        std::ofstream output(legacy.path() / "storage.meta");
        output << "TINYDBMS_STORAGE_V1\n"
               << "TABLE 0 legacy\n"
               << "COLUMN INT id\n"
               << "ENDTABLE\n"
               << "END\n";
    }
    auto legacy_page_file = tinydbms::storage::internal::PageFile::create(
        legacy.path() / "tables" / "table_0.dat");
    passed = expect(legacy_page_file.value.has_value(),
                    "legacy metadata fixture must include a valid table PageFile") &&
             passed;
    if (legacy_page_file.value.has_value()) {
        (*legacy_page_file.value)->close();
    }
    const auto legacy_opened = tinydbms::storage::open_storage({legacy.path().string()});
    passed = expect(!legacy_opened.error.has_value(),
                    "legacy INT metadata must recover as INT32") &&
             passed;
    const auto legacy_tables = tinydbms::storage::list_tables();
    passed = expect(!legacy_tables.error.has_value() && legacy_tables.tables.size() == 1 &&
                         legacy_tables.tables.front().columns.front().type == tinydbms::Type::kInt32,
                    "legacy INT metadata must deserialize to Type::kInt32") &&
             passed;
    passed = expect(!tinydbms::storage::close_storage().error.has_value(),
                    "legacy metadata close must rewrite a valid current metadata file") &&
             passed;
    return passed;
}

bool test_metadata_write_failure_preserves_old_metadata() {
    TemporaryDirectory directory{"atomic"};
    const auto columns = all_supported_columns();
    bool passed = expect(!tinydbms::storage::open_storage({directory.path().string()}).error.has_value(),
                          "initial open must succeed");
    passed = expect(!tinydbms::storage::create_table({0, "stable", columns}).error.has_value(),
                    "initial metadata must be written") &&
             passed;
    passed = expect(!tinydbms::storage::close_storage().error.has_value(),
                    "initial close must succeed") &&
             passed;

    passed = expect(!tinydbms::storage::open_storage({directory.path().string()}).error.has_value(),
                    "reopen before induced failure must succeed") &&
             passed;
    const auto temporary_path = directory.path() / "storage.meta.tmp";
    std::filesystem::create_directories(temporary_path);
    {
        std::ofstream lock(temporary_path / "lock");
        lock << "prevent temporary metadata replacement";
    }

    const auto failed = tinydbms::storage::create_table({1, "must_not_persist", columns});
    passed = expect(has_error(failed.error, tinydbms::StorageErrorKind::kIoError),
                    "metadata temporary-file failure must return kIoError") &&
             passed;

    std::error_code error;
    std::filesystem::remove_all(temporary_path, error);
    passed = expect(!error, "test temporary metadata directory must be removable") && passed;
    passed = expect(!tinydbms::storage::close_storage().error.has_value(),
                    "close after failed create must preserve the old in-memory catalog") &&
             passed;
    passed = expect(!tinydbms::storage::open_storage({directory.path().string()}).error.has_value(),
                    "metadata written before failure must remain reopenable") &&
             passed;
    const auto recovered = tinydbms::storage::list_tables();
    passed = expect(!recovered.error.has_value() && recovered.tables.size() == 1 &&
                         recovered.tables.front().table_name == "stable",
                    "failed metadata update must not replace old metadata") &&
             passed;
    passed = expect(!tinydbms::storage::close_storage().error.has_value(),
                    "final close after atomicity test must succeed") &&
             passed;
    return passed;
}

bool test_cursor_contract() {
    TemporaryDirectory directory{"cursor"};
    bool passed = expect(!tinydbms::storage::open_storage({directory.path().string()}).error.has_value(),
                          "cursor test open must succeed");
    const auto opened = tinydbms::storage::open_table({0});
    passed = expect(!opened.cursor_id.has_value() && opened.error.has_value(),
                    "missing table must not return a cursor without an error") &&
             passed;
    const auto next = tinydbms::storage::scan_next(0);
    passed = expect(!next.record.has_value() &&
                         has_error(next.error, tinydbms::StorageErrorKind::kCursorInvalid),
                    "unknown cursor must return error without a Record") &&
             passed;
    passed = expect(!tinydbms::storage::create_table({0,"empty",{{"id",tinydbms::Type::kInt32}}}).error,
                    "empty scan table creation must succeed") && passed;
    const auto cursor = tinydbms::storage::open_table({0});
    passed = expect(cursor.cursor_id.has_value() && !cursor.error,
                    "existing table must return a cursor") && passed;
    if (cursor.cursor_id) {
        const auto eof = tinydbms::storage::scan_next(*cursor.cursor_id);
        passed = expect(!eof.record && !eof.error, "empty table scan must be EOF") && passed;
        passed = expect(!tinydbms::storage::close_cursor(*cursor.cursor_id), "cursor close must succeed") && passed;
    }
    passed = expect(!tinydbms::storage::close_storage().error.has_value(),
                    "cursor test close must succeed") &&
             passed;
    return passed;
}

}  // namespace

int main() {
    const bool passed = test_value_type_contract() && test_lifecycle_and_metadata_recovery() &&
                        test_schema_validation_contract() &&
                        test_corruption_and_io_errors() &&
                        test_metadata_write_failure_preserves_old_metadata() &&
                        test_cursor_contract();
    return passed ? 0 : 1;
}
