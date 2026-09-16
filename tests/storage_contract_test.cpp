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
    ok = expect(!create_table({2, "people", columns}).error, "create canonical table") && ok;
    const auto inserted = insert({2, {{Value{std::int32_t{7}}, Value{std::string{"Ada"}}}}});
    ok = expect(!inserted.error && inserted.rids.size() == 1, "insert canonical row") && ok;
    ok = expect(!close_storage({}).error, "close storage") && ok;
    ok = expect(!open_storage({directory.path.string()}).error, "reopen V2 system catalog") && ok;
    const auto opened = open_table({2});
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

bool test_stats_are_scoped_to_one_open_lifecycle() {
    TemporaryDirectory directory;

    // 未打开：必须报错，而不是给出一个空快照。
    const auto closed = storage_stats({});
    bool ok = expect(!closed.stats.has_value() &&
                        closed.error.has_value() &&
                        closed.error->kind == StorageErrorKind::kInvalidRequest,
                    "stats require an open storage");

    ok = expect(!open_storage({directory.path.string()}).error, "open storage for stats") && ok;

    // 新会话：计数归零，hit_rate() 不除零。
    const auto fresh = storage_stats({});
    ok = expect(fresh.stats.has_value() && !fresh.error.has_value(), "fresh snapshot") && ok;
    ok = expect(fresh.stats->fetch_count == 0 && fresh.stats->hit_rate() == 0.0,
                "fresh counters are zero") && ok;

    const std::vector<ColumnMeta> columns{{"id", Type::kInt}};
    ok = expect(!create_table({2, "stats_probe", columns}).error, "create table for stats") && ok;
    ok = expect(!insert({2, {{Value{std::int32_t{1}}}}}).error, "insert for stats") && ok;

    const auto opened = open_table({2});
    ok = expect(opened.cursor.has_value() && !opened.error, "open cursor for stats") && ok;
    ok = expect(scan_next({*opened.cursor}).record.has_value(), "scan for stats") && ok;
    ok = expect(!close_cursor({*opened.cursor}).error, "close cursor for stats") && ok;

    const auto busy = storage_stats({});
    ok = expect(busy.stats.has_value() && !busy.error.has_value(), "busy snapshot") && ok;
    ok = expect(busy.stats->fetch_count > 0, "buffer pool saw demand traffic") && ok;
    ok = expect(busy.stats->hit_count + busy.stats->miss_count == busy.stats->fetch_count,
                "hit and miss partition fetch") && ok;
    const double expected_rate =
        static_cast<double>(busy.stats->hit_count) / static_cast<double>(busy.stats->fetch_count);
    ok = expect(busy.stats->hit_rate() == expected_rate, "hit_rate matches the counters") && ok;

    // 快照是只读的：重复读取不改变计数。
    const auto repeated = storage_stats({});
    ok = expect(repeated.stats.has_value() &&
                    repeated.stats->fetch_count == busy.stats->fetch_count &&
                    repeated.stats->hit_count == busy.stats->hit_count &&
                    repeated.stats->miss_count == busy.stats->miss_count,
                "stats snapshot is read-only") && ok;

    ok = expect(!close_storage({}).error, "close storage for stats") && ok;
    const auto after_close = storage_stats({});
    ok = expect(!after_close.stats.has_value() &&
                    after_close.error.has_value() &&
                    after_close.error->kind == StorageErrorKind::kInvalidRequest,
                "close ends the stats scope") && ok;
    return ok;
}
}

int main() {
    return test_canonical_lifecycle_and_subset_reopen() &&
            test_extended_legacy_schema_is_rejected() &&
            test_stats_are_scoped_to_one_open_lifecycle()
        ? 0
        : 1;
}
