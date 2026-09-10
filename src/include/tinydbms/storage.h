#pragma once

#include "tinydbms/types.h"

#include <optional>
#include <string>
#include <vector>

namespace tinydbms::storage {

struct OpenStorageRequest {
    std::string data_dir;
};

struct OpenStorageResult {
    std::optional<StorageError> error;
};

struct CloseStorageResult {
    std::optional<StorageError> error;
};

struct ListTablesResult {
    std::vector<TableMeta> tables;
    std::optional<StorageError> error;
};

struct CreateTableRequest {
    TableId table_id = 0;
    std::string table_name;
    std::vector<ColumnMeta> columns;
};

struct CreateTableResult {
    std::optional<StorageError> error;
};

struct RecordId {
    std::uint64_t value = 0;
};

struct Record {
    RecordId record_id;
    std::vector<Value> values;
};

struct OpenTableRequest {
    TableId table_id = 0;
};

struct OpenTableResult {
    std::optional<CursorId> cursor_id;
    std::optional<StorageError> error;
};

struct ScanNextResult {
    std::optional<Record> record;
    std::optional<StorageError> error;
};

struct InsertRequest {
    TableId table_id = 0;
    std::vector<std::vector<Value>> rows;
};

struct InsertResult {
    std::vector<RecordId> record_ids;
    std::optional<StorageError> error;
};

struct DeleteRequest {
    TableId table_id = 0;
    std::vector<RecordId> record_ids;
};

struct DeleteResult {
    std::uint64_t deleted_count = 0;
    std::optional<StorageError> error;
};

OpenStorageResult open_storage(const OpenStorageRequest& request);
CloseStorageResult close_storage();
ListTablesResult list_tables();
CreateTableResult create_table(const CreateTableRequest& request);
OpenTableResult open_table(const OpenTableRequest& request);
ScanNextResult scan_next(CursorId cursor_id);
std::optional<StorageError> close_cursor(CursorId cursor_id);
InsertResult insert(const InsertRequest& request);
DeleteResult delete_records(const DeleteRequest& request);

}  // namespace tinydbms::storage
