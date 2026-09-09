#include "storage_fake.hpp"

#include <stdexcept>
#include <utility>

namespace tinydbms::testing::fake_storage {
namespace {

State fake_state;

}  // namespace

State& state() {
    return fake_state;
}

void reset() {
    fake_state = State{};
}

void set_tables(std::vector<TableMeta> tables) {
    fake_state.tables = std::move(tables);
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

void set_throw_on_open(bool enabled) {
    fake_state.throw_on_open = enabled;
}

void set_throw_after_open(bool enabled) {
    fake_state.throw_after_open = enabled;
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

}  // namespace

OpenStorageResult open_storage(const OpenStorageRequest&) {
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
    ++testing::fake_storage::state().close_calls;
    testing::fake_storage::State& fake = testing::fake_storage::state();

    if (!fake.opened) {
        return CloseStorageResult{invalid_request("storage is not open")};
    }

    fake.opened = false;
    if (fake.throw_on_close) {
        throw std::runtime_error("injected close exception");
    }
    if (fake.close_error.has_value()) {
        return CloseStorageResult{fake.close_error};
    }
    return CloseStorageResult{std::nullopt};
}

ListTablesResult list_tables(const ListTablesRequest&) {
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
    return CreateTableResult{std::nullopt};
}

OpenTableResult open_table(const OpenTableRequest&) {
    return OpenTableResult{std::nullopt, invalid_request("fake open_table is not configured")};
}

ScanNextResult scan_next(const ScanNextRequest&) {
    return ScanNextResult{std::nullopt, invalid_request("fake scan_next is not configured")};
}

CloseCursorResult close_cursor(const CloseCursorRequest&) {
    return CloseCursorResult{invalid_request("fake close_cursor is not configured")};
}

InsertResult insert(const InsertRequest&) {
    return InsertResult{{}, invalid_request("fake insert is not configured")};
}

DeleteResult delete_records(const DeleteRequest&) {
    return DeleteResult{0, invalid_request("fake delete_records is not configured")};
}

}  // namespace tinydbms::storage
