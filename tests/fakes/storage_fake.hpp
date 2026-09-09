#ifndef TINYDBMS_TEST_STORAGE_FAKE_HPP
#define TINYDBMS_TEST_STORAGE_FAKE_HPP

#include <cstddef>
#include <deque>
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

    std::vector<std::string> call_order;
    std::vector<TableMeta> tables;
    std::vector<storage::Record> records;
    std::optional<storage::CreateTableRequest> last_create_request;
    std::optional<storage::OpenTableRequest> last_open_table_request;
    std::optional<storage::CloseCursorRequest> last_close_cursor_request;
    std::optional<storage::InsertRequest> last_insert_request;
    std::optional<storage::DeleteRequest> last_delete_request;
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
    storage::CursorId active_cursor = 0;
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
void set_open_table_result(storage::OpenTableResult result);
void set_scan_results(std::deque<storage::ScanNextResult> results);
void set_close_cursor_result(storage::CloseCursorResult result);
void set_insert_result(storage::InsertResult result);
void set_delete_result(storage::DeleteResult result);
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
void clear_close_error();
void clear_create_table_error();

}  // namespace tinydbms::testing::fake_storage

#endif  // TINYDBMS_TEST_STORAGE_FAKE_HPP
