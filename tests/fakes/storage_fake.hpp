#ifndef TINYDBMS_TEST_STORAGE_FAKE_HPP
#define TINYDBMS_TEST_STORAGE_FAKE_HPP

#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "tinydbms/storage.hpp"

namespace tinydbms::testing::fake_storage {

struct State {
    bool opened = false;
    std::size_t open_calls = 0;
    std::size_t close_calls = 0;
    std::size_t list_tables_calls = 0;
    std::size_t create_table_calls = 0;
    std::size_t open_table_calls = 0;
    std::size_t scan_next_calls = 0;
    std::size_t close_cursor_calls = 0;
    std::size_t insert_calls = 0;
    std::size_t delete_calls = 0;
    std::size_t update_calls = 0;
    std::size_t storage_stats_calls = 0;

    std::vector<std::string> call_order;
    std::vector<TableMeta> tables;
    // `records` is a compatibility view used by existing single-table tests.
    // Storage operations use `records_by_table` so scans never cross table boundaries.
    std::vector<storage::Record> records;
    std::map<TableId, std::vector<storage::Record>> records_by_table;
    std::optional<storage::CreateTableRequest> last_create_request;
    std::optional<storage::OpenTableRequest> last_open_table_request;
    std::optional<storage::CloseCursorRequest> last_close_cursor_request;
    std::optional<storage::InsertRequest> last_insert_request;
    std::optional<storage::DeleteRequest> last_delete_request;
    std::optional<storage::UpdateRequest> last_update_request;
    std::optional<storage::StorageError> open_error;
    std::optional<storage::StorageError> close_error;
    std::optional<storage::StorageError> list_tables_error;
    std::optional<storage::StorageError> create_table_error;
    std::optional<storage::StorageError> open_table_error;
    std::optional<storage::StorageError> close_cursor_error;
    std::optional<storage::StorageError> insert_error;
    std::optional<storage::StorageError> delete_error;
    std::optional<storage::OpenTableResult> open_table_result;
    std::deque<storage::ScanNextResult> scan_results;
    std::optional<storage::CloseCursorResult> close_cursor_result;
    std::optional<storage::InsertResult> insert_result;
    std::optional<storage::DeleteResult> delete_result;
    std::optional<storage::UpdateResult> update_result;
    // storage_stats 成功时的固定快照；未显式设置时返回全零且 hit_rate() 为 0。
    storage::StorageStats stats_snapshot{};
    std::optional<storage::StorageError> storage_stats_error;
    storage::CursorId active_cursor = 0;
    std::optional<TableId> active_table_id;
    storage::CursorId next_cursor_id = 1;
    storage::RecordId next_record_id{1};
    std::size_t scan_index = 0;
    bool cursor_opened = false;
    bool scan_exhausted = false;
    bool throw_on_open = false;
    bool throw_after_open = false;
    bool throw_before_close = false;
    bool throw_on_close = false;
    bool throw_on_list_tables = false;
    bool throw_on_create_table = false;
    bool throw_after_create_table = false;
    bool throw_on_open_table = false;
    bool throw_after_open_table = false;
    bool throw_on_scan_next = false;
    bool throw_on_close_cursor = false;
    bool throw_on_insert = false;
    bool throw_after_insert = false;
    bool throw_on_delete = false;
    bool throw_after_delete = false;
    bool throw_on_storage_stats = false;
    // 每次 scan_next 记录完之后调用；用于在扫描中途注入取消请求等外部事件。
    std::function<void()> on_scan_next;
    // 每次 insert 记录完之后调用（INSERT 首版不设取消检查点，用于验证该边界）。
    std::function<void()> on_insert;
    // 每次公共 fake 入口调用；参数是该入口的名字（open_table、scan_next 等）。
    // 用于制造"调用方仍在 storage 内部"的交错窗口。
    std::function<void(const char* call)> on_storage_call;
};

void reset();
State& state();
void set_tables(std::vector<TableMeta> tables);
void set_open_error(storage::StorageError error);
void set_close_error(storage::StorageError error);
void set_list_tables_error(storage::StorageError error);
void set_create_table_error(storage::StorageError error);
void set_open_table_error(storage::StorageError error);
void set_close_cursor_error(storage::StorageError error);
void set_insert_error(storage::StorageError error);
void set_delete_error(storage::StorageError error);
void set_records(std::vector<storage::Record> records);
void set_records_for_table(TableId table_id, std::vector<storage::Record> records);
void set_open_table_result(storage::OpenTableResult result);
void set_scan_results(std::deque<storage::ScanNextResult> results);
void set_close_cursor_result(storage::CloseCursorResult result);
void set_insert_result(storage::InsertResult result);
void set_delete_result(storage::DeleteResult result);
void set_update_result(storage::UpdateResult result);
void set_storage_stats(storage::StorageStats stats);
void set_storage_stats_error(storage::StorageError error);
void set_throw_on_open(bool enabled);
void set_throw_after_open(bool enabled);
void set_throw_before_close(bool enabled);
void set_throw_on_close(bool enabled);
void set_throw_on_list_tables(bool enabled);
void set_throw_on_create_table(bool enabled);
void set_throw_after_create_table(bool enabled);
void set_throw_on_open_table(bool enabled);
void set_throw_after_open_table(bool enabled);
void set_throw_on_scan_next(bool enabled);
void set_throw_on_close_cursor(bool enabled);
void set_throw_on_insert(bool enabled);
void set_throw_after_insert(bool enabled);
void set_throw_on_delete(bool enabled);
void set_throw_after_delete(bool enabled);
void set_throw_on_storage_stats(bool enabled);
void set_on_scan_next(std::function<void()> hook);
void set_on_insert(std::function<void()> hook);
void set_on_storage_call(std::function<void(const char* call)> hook);
// 并发取证：同时处于公共 fake 入口内部的调用方数量峰值。
// core 串行化正确时恒为 1；大于 1 说明有执行流没有被 core 挡住。
std::size_t max_concurrent_calls();
bool overlap_detected();
void clear_close_error();
void clear_create_table_error();

}  // namespace tinydbms::testing::fake_storage

#endif  // TINYDBMS_TEST_STORAGE_FAKE_HPP
