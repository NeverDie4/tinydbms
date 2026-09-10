#include "storage_fake.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace tinydbms::testing::fake_storage {
namespace {

State fake_state;

void rebuild_records_view(State& fake) {
    fake.records.clear();
    for (const auto& entry : fake.records_by_table) {
        fake.records.insert(fake.records.end(), entry.second.begin(), entry.second.end());
    }
}

}  // namespace

State& state() {
    return fake_state;
}

void reset() {
    fake_state = State{};
}

void set_tables(std::vector<TableMeta> tables) {
    fake_state.tables = std::move(tables);
    fake_state.records.clear();
    fake_state.records_by_table.clear();
    for (const TableMeta& table : fake_state.tables) {
        fake_state.records_by_table.emplace(table.table_id, std::vector<storage::Record>{});
    }
}

void set_open_error(storage::StorageError error) {
    fake_state.open_error = std::move(error);
}

void set_close_error(storage::StorageError error) {
    fake_state.close_error = std::move(error);
}

void set_list_tables_error(storage::StorageError error) {
    fake_state.list_tables_error = std::move(error);
}

void set_create_table_error(storage::StorageError error) {
    fake_state.create_table_error = std::move(error);
}

void set_open_table_error(storage::StorageError error) {
    fake_state.open_table_error = std::move(error);
}

void set_close_cursor_error(storage::StorageError error) {
    fake_state.close_cursor_error = std::move(error);
}

void set_insert_error(storage::StorageError error) {
    fake_state.insert_error = std::move(error);
}

void set_delete_error(storage::StorageError error) {
    fake_state.delete_error = std::move(error);
}

void set_records(std::vector<storage::Record> records) {
    if (fake_state.tables.size() == 1) {
        set_records_for_table(fake_state.tables.front().table_id, std::move(records));
        return;
    }

    // Do not guess a table for a multi-table fixture. Callers must use the
    // table-specific helper in that case.
    fake_state.records_by_table.clear();
    fake_state.records = std::move(records);
}

void set_records_for_table(TableId table_id, std::vector<storage::Record> records) {
    fake_state.records_by_table[table_id] = std::move(records);
    rebuild_records_view(fake_state);
}

void set_open_table_result(storage::OpenTableResult result) {
    fake_state.open_table_result = std::move(result);
}

void set_scan_results(std::deque<storage::ScanNextResult> results) {
    fake_state.scan_results = std::move(results);
}

void set_close_cursor_result(storage::CloseCursorResult result) {
    fake_state.close_cursor_result = std::move(result);
}

void set_insert_result(storage::InsertResult result) {
    fake_state.insert_result = std::move(result);
}

void set_delete_result(storage::DeleteResult result) {
    fake_state.delete_result = std::move(result);
}

void set_throw_on_open(bool enabled) {
    fake_state.throw_on_open = enabled;
}

void set_throw_after_open(bool enabled) {
    fake_state.throw_after_open = enabled;
}

void set_throw_before_close(bool enabled) {
    fake_state.throw_before_close = enabled;
}

void set_throw_on_close(bool enabled) {
    fake_state.throw_on_close = enabled;
}

void set_throw_on_list_tables(bool enabled) {
    fake_state.throw_on_list_tables = enabled;
}

void set_throw_on_create_table(bool enabled) {
    fake_state.throw_on_create_table = enabled;
}

void set_throw_after_create_table(bool enabled) {
    fake_state.throw_after_create_table = enabled;
}

void set_throw_on_open_table(bool enabled) {
    fake_state.throw_on_open_table = enabled;
}

void set_throw_after_open_table(bool enabled) {
    fake_state.throw_after_open_table = enabled;
}

void set_throw_on_scan_next(bool enabled) {
    fake_state.throw_on_scan_next = enabled;
}

void set_throw_on_close_cursor(bool enabled) {
    fake_state.throw_on_close_cursor = enabled;
}

void set_throw_on_insert(bool enabled) {
    fake_state.throw_on_insert = enabled;
}

void set_throw_after_insert(bool enabled) {
    fake_state.throw_after_insert = enabled;
}

void set_throw_on_delete(bool enabled) {
    fake_state.throw_on_delete = enabled;
}

void set_throw_after_delete(bool enabled) {
    fake_state.throw_after_delete = enabled;
}

void clear_close_error() {
    fake_state.close_error.reset();
}

void clear_create_table_error() {
    fake_state.create_table_error.reset();
}

}  // namespace tinydbms::testing::fake_storage

namespace tinydbms::storage {
namespace {

StorageError invalid_request(const char* message) {
    return StorageError{StorageErrorKind::kInvalidRequest, message};
}

StorageError table_not_found(const char* message) {
    return StorageError{StorageErrorKind::kTableNotFound, message};
}

void record_call(const char* name) {
    testing::fake_storage::state().call_order.emplace_back(name);
}

void sync_records_view() {
    testing::fake_storage::State& fake = testing::fake_storage::state();
    fake.records.clear();
    for (const auto& entry : fake.records_by_table) {
        fake.records.insert(fake.records.end(), entry.second.begin(), entry.second.end());
    }
}

bool has_table(TableId table_id) {
    const auto& tables = testing::fake_storage::state().tables;
    return std::any_of(
        tables.begin(),
        tables.end(),
        [table_id](const TableMeta& table) { return table.table_id == table_id; });
}

}  // namespace

OpenStorageResult open_storage(const OpenStorageRequest&) {
    record_call("open_storage");
    ++testing::fake_storage::state().open_calls;
    testing::fake_storage::State& fake = testing::fake_storage::state();

    if (fake.throw_on_open) {
        throw std::runtime_error("injected open exception");
    }
    if (fake.opened) {
        return OpenStorageResult{StorageError{StorageErrorKind::kInvalidRequest, "storage is already open"}};
    }
    if (fake.open_error.has_value()) {
        return OpenStorageResult{fake.open_error};
    }

    fake.opened = true;
    if (fake.throw_after_open) {
        throw std::runtime_error("injected post-open exception");
    }
    return OpenStorageResult{std::nullopt};
}

CloseStorageResult close_storage(const CloseStorageRequest&) {
    record_call("close_storage");
    ++testing::fake_storage::state().close_calls;
    testing::fake_storage::State& fake = testing::fake_storage::state();

    if (!fake.opened) {
        return CloseStorageResult{invalid_request("storage is not open")};
    }

    if (fake.throw_before_close) {
        throw std::runtime_error("injected pre-close exception");
    }
    fake.opened = false;
    fake.cursor_opened = false;
    fake.active_cursor = 0;
    fake.active_table_id = std::nullopt;
    if (fake.throw_on_close) {
        throw std::runtime_error("injected close exception");
    }
    if (fake.close_error.has_value()) {
        return CloseStorageResult{fake.close_error};
    }
    return CloseStorageResult{std::nullopt};
}

ListTablesResult list_tables(const ListTablesRequest&) {
    record_call("list_tables");
    ++testing::fake_storage::state().list_tables_calls;
    testing::fake_storage::State& fake = testing::fake_storage::state();

    if (!fake.opened) {
        return ListTablesResult{{}, invalid_request("storage is not open")};
    }
    if (fake.throw_on_list_tables) {
        throw std::runtime_error("injected list_tables exception");
    }
    if (fake.list_tables_error.has_value()) {
        return ListTablesResult{{}, fake.list_tables_error};
    }
    return ListTablesResult{fake.tables, std::nullopt};
}

CreateTableResult create_table(const CreateTableRequest& request) {
    record_call("create_table");
    ++testing::fake_storage::state().create_table_calls;
    testing::fake_storage::State& fake = testing::fake_storage::state();
    fake.last_create_request = request;

    if (!fake.opened) {
        return CreateTableResult{invalid_request("storage is not open")};
    }
    if (fake.throw_on_create_table) {
        throw std::runtime_error("injected create_table exception");
    }
    if (fake.create_table_error.has_value()) {
        return CreateTableResult{fake.create_table_error};
    }

    fake.tables.push_back(TableMeta{request.table_id, request.table_name, request.columns});
    fake.records_by_table.emplace(request.table_id, std::vector<Record>{});
    if (fake.throw_after_create_table) {
        throw std::runtime_error("injected post-create_table exception");
    }
    return CreateTableResult{std::nullopt};
}

OpenTableResult open_table(const OpenTableRequest& request) {
    record_call("open_table");
    ++testing::fake_storage::state().open_table_calls;
    testing::fake_storage::State& fake = testing::fake_storage::state();
    fake.last_open_table_request = request;

    if (!fake.opened) {
        return OpenTableResult{std::nullopt, invalid_request("storage is not open")};
    }
    if (fake.throw_on_open_table) {
        throw std::runtime_error("injected open_table exception");
    }
    if (fake.open_table_result.has_value()) {
        OpenTableResult result = std::move(*fake.open_table_result);
        fake.open_table_result.reset();
        if (result.cursor.has_value()) {
            fake.cursor_opened = true;
            fake.active_cursor = *result.cursor;
            fake.active_table_id = request.table_id;
            fake.scan_index = 0;
            fake.scan_exhausted = false;
        }
        if (fake.throw_after_open_table) {
            throw std::runtime_error("injected post-open_table exception");
        }
        return result;
    }
    if (fake.open_table_error.has_value()) {
        return OpenTableResult{std::nullopt, fake.open_table_error};
    }
    if (!has_table(request.table_id)) {
        return OpenTableResult{std::nullopt, table_not_found("table does not exist")};
    }

    fake.cursor_opened = true;
    fake.active_cursor = fake.next_cursor_id++;
    fake.active_table_id = request.table_id;
    fake.scan_index = 0;
    fake.scan_exhausted = false;
    if (fake.throw_after_open_table) {
        throw std::runtime_error("injected post-open_table exception");
    }
    return OpenTableResult{fake.active_cursor, std::nullopt};
}

ScanNextResult scan_next(const ScanNextRequest& request) {
    record_call("scan_next");
    ++testing::fake_storage::state().scan_next_calls;
    testing::fake_storage::State& fake = testing::fake_storage::state();

    if (!fake.opened || !fake.cursor_opened || request.cursor != fake.active_cursor) {
        return ScanNextResult{std::nullopt, invalid_request("cursor is invalid")};
    }
    if (fake.throw_on_scan_next) {
        throw std::runtime_error("injected scan_next exception");
    }
    if (fake.scan_exhausted) {
        return ScanNextResult{std::nullopt, std::nullopt};
    }
    if (!fake.scan_results.empty()) {
        ScanNextResult result = std::move(fake.scan_results.front());
        fake.scan_results.pop_front();
        if (!result.record.has_value() && !result.error.has_value()) {
            fake.scan_exhausted = true;
        }
        return result;
    }
    if (!fake.active_table_id.has_value()) {
        return ScanNextResult{std::nullopt, invalid_request("cursor has no table")};
    }
    const auto table_records = fake.records_by_table.find(*fake.active_table_id);
    if (table_records == fake.records_by_table.end() ||
        fake.scan_index == table_records->second.size()) {
        fake.scan_exhausted = true;
        return ScanNextResult{std::nullopt, std::nullopt};
    }
    return ScanNextResult{table_records->second[fake.scan_index++], std::nullopt};
}

CloseCursorResult close_cursor(const CloseCursorRequest& request) {
    record_call("close_cursor");
    ++testing::fake_storage::state().close_cursor_calls;
    testing::fake_storage::State& fake = testing::fake_storage::state();
    fake.last_close_cursor_request = request;

    if (!fake.opened || !fake.cursor_opened || request.cursor != fake.active_cursor) {
        return CloseCursorResult{invalid_request("cursor is invalid")};
    }

    fake.cursor_opened = false;
    fake.active_cursor = 0;
    fake.active_table_id = std::nullopt;
    if (fake.throw_on_close_cursor) {
        throw std::runtime_error("injected close_cursor exception");
    }
    if (fake.close_cursor_result.has_value()) {
        CloseCursorResult result = std::move(*fake.close_cursor_result);
        fake.close_cursor_result.reset();
        return result;
    }
    if (fake.close_cursor_error.has_value()) {
        return CloseCursorResult{fake.close_cursor_error};
    }
    return CloseCursorResult{std::nullopt};
}

InsertResult insert(const InsertRequest& request) {
    record_call("insert");
    ++testing::fake_storage::state().insert_calls;
    testing::fake_storage::State& fake = testing::fake_storage::state();
    fake.last_insert_request = request;

    if (!fake.opened) {
        return InsertResult{{}, invalid_request("storage is not open")};
    }
    if (!has_table(request.table_id)) {
        return InsertResult{{}, table_not_found("table does not exist")};
    }
    if (fake.throw_on_insert) {
        throw std::runtime_error("injected insert exception");
    }
    if (fake.insert_result.has_value()) {
        InsertResult result = std::move(*fake.insert_result);
        fake.insert_result.reset();
        return result;
    }
    if (fake.insert_error.has_value()) {
        return InsertResult{{}, fake.insert_error};
    }

    std::vector<RecordId> rids;
    rids.reserve(request.rows.size());
    std::vector<Record>& table_records = fake.records_by_table[request.table_id];
    for (const std::vector<Value>& row : request.rows) {
        const RecordId rid = fake.next_record_id;
        ++fake.next_record_id.value;
        table_records.push_back(Record{rid, row});
        rids.push_back(rid);
    }
    sync_records_view();
    if (fake.throw_after_insert) {
        throw std::runtime_error("injected post-insert exception");
    }
    return InsertResult{std::move(rids), std::nullopt};
}

DeleteResult delete_records(const DeleteRequest& request) {
    record_call("delete_records");
    ++testing::fake_storage::state().delete_calls;
    testing::fake_storage::State& fake = testing::fake_storage::state();
    fake.last_delete_request = request;

    if (!fake.opened) {
        return DeleteResult{0, invalid_request("storage is not open")};
    }
    if (!has_table(request.table_id)) {
        return DeleteResult{0, table_not_found("table does not exist")};
    }
    if (fake.throw_on_delete) {
        throw std::runtime_error("injected delete exception");
    }
    if (fake.delete_result.has_value()) {
        DeleteResult result = *fake.delete_result;
        fake.delete_result.reset();
        return result;
    }
    if (fake.delete_error.has_value()) {
        return DeleteResult{0, fake.delete_error};
    }

    std::uint64_t deleted_count = 0;
    std::vector<Record>& table_records = fake.records_by_table[request.table_id];
    for (const RecordId rid : request.rids) {
        const auto record = std::find_if(
            table_records.begin(),
            table_records.end(),
            [rid](const Record& candidate) { return candidate.rid.value == rid.value; });
        if (record != table_records.end()) {
            table_records.erase(record);
            ++deleted_count;
        }
    }
    sync_records_view();
    if (fake.throw_after_delete) {
        throw std::runtime_error("injected post-delete exception");
    }
    return DeleteResult{deleted_count, std::nullopt};
}

}  // namespace tinydbms::storage
