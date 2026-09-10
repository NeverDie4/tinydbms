#include "tinydbms/storage.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
using namespace tinydbms;
using namespace tinydbms::storage;

bool expect(bool value, const char* message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}

struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() / "tinydbms-canonical-storage-contract";
    TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

bool test_canonical_lifecycle_and_subset_reopen() {
    TemporaryDirectory directory;
    bool ok = expect(!open_storage({directory.path.string()}).error, "open storage");
    const std::vector<ColumnMeta> columns{{"id", Type::kInt}, {"name", Type::kVarchar}};
    ok = expect(!create_table({0, "people", columns}).error, "create canonical table") && ok;
    const auto inserted = insert({0, {{Value{std::int32_t{7}}, Value{std::string{"Ada"}}}}});
    ok = expect(!inserted.error && inserted.rids.size() == 1, "insert canonical row") && ok;
    ok = expect(!close_storage({}).error, "close storage") && ok;
    ok = expect(!open_storage({directory.path.string()}).error, "reopen INT32/VARCHAR V1 metadata") && ok;
    const auto opened = open_table({0});
    ok = expect(opened.cursor.has_value() && !opened.error, "open cursor") && ok;
    const auto next = scan_next({*opened.cursor});
    ok = expect(next.record.has_value() && next.record->rid.value == inserted.rids[0].value &&
                    next.record->values[0].data == Value{std::int32_t{7}}.data,
                "read original INT32 encoded row") && ok;
    ok = expect(!close_cursor({*opened.cursor}).error, "close cursor") && ok;
    return expect(!close_storage({}).error, "final close") && ok;
}

bool test_extended_legacy_schema_is_rejected() {
    TemporaryDirectory directory;
    std::filesystem::create_directories(directory.path);
    std::ofstream metadata(directory.path / "storage.meta");
    metadata << "TINYDBMS_STORAGE_V1\nTABLE 0 legacy\nCOLUMN INT64 id\nENDTABLE\nEND\n";
    metadata.close();
    const auto opened = open_storage({directory.path.string()});
    return expect(opened.error.has_value() && opened.error->kind == StorageErrorKind::kInvalidRequest,
                  "reject legacy extended schema without corruption aliasing");
}
}

int main() {
    return test_canonical_lifecycle_and_subset_reopen() && test_extended_legacy_schema_is_rejected() ? 0 : 1;
}
