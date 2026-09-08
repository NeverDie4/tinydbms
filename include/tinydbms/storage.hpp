#ifndef TINYDBMS_STORAGE_HPP
#define TINYDBMS_STORAGE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "tinydbms/common.hpp"

namespace tinydbms::storage {

struct RecordId {
    std::uint64_t value;  // 内部结构归 storage，core 只保存并原样归还
};

using CursorId = std::uint64_t;  // 由 storage 分配

enum class StorageErrorKind {
    kTableNotFound,
    kCursorInvalid,
    kValueTooLarge,
    kIoError,
    kCorrupt,
    kInvalidRequest
};

struct StorageError {
    StorageErrorKind kind;
    std::string message;
};

struct OpenStorageRequest {
    std::string data_dir;  // 数据库目录，UTF-8 编码；来自入口 --data-dir 或缺省 ./tinydbms-data
};

struct OpenStorageResult {
    std::optional<StorageError> error;
};

struct CloseStorageRequest {};

struct CloseStorageResult {
    std::optional<StorageError> error;
};

struct ListTablesRequest {};

struct ListTablesResult {
    std::vector<TableMeta> tables;
    std::optional<StorageError> error;
};

struct CreateTableRequest {
    TableId table_id;  // core 分配
    std::string table_name;
    std::vector<ColumnMeta> columns;
};

struct CreateTableResult {
    std::optional<StorageError> error;
};

struct OpenTableRequest {
    TableId table_id;
};

struct OpenTableResult {
    std::optional<CursorId> cursor;  // 成功时必有值；失败时为空
    std::optional<StorageError> error;
};

struct ScanNextRequest {
    CursorId cursor;
};

struct Record {
    RecordId rid;                // storage 分配，core 不解释
    std::vector<Value> values;   // 按建表列序，返回全行
};

struct ScanNextResult {
    std::optional<Record> record;  // 空 + error 空 = 扫描结束
    std::optional<StorageError> error;
};

struct CloseCursorRequest {
    CursorId cursor;
};

struct CloseCursorResult {
    std::optional<StorageError> error;
};

struct InsertRequest {
    TableId table_id;
    std::vector<std::vector<Value>> rows;  // 一次多行，按顺序执行；每行都是全行、按建表列序
};

struct InsertResult {
    std::vector<RecordId> rids;  // 与已成功插入的行一一对应
    std::optional<StorageError> error;
};

struct DeleteRequest {
    TableId table_id;
    std::vector<RecordId> rids;  // 一次一串，按顺序执行
};

struct DeleteResult {
    std::uint64_t deleted_count;  // 已成功删除的数量
    std::optional<StorageError> error;
};

OpenStorageResult open_storage(const OpenStorageRequest& request);
CloseStorageResult close_storage(const CloseStorageRequest& request);
ListTablesResult list_tables(const ListTablesRequest& request);
CreateTableResult create_table(const CreateTableRequest& request);
OpenTableResult open_table(const OpenTableRequest& request);
ScanNextResult scan_next(const ScanNextRequest& request);
CloseCursorResult close_cursor(const CloseCursorRequest& request);
InsertResult insert(const InsertRequest& request);
DeleteResult delete_records(const DeleteRequest& request);  // 避开 C++ delete 关键字

}  // namespace tinydbms::storage

#endif  // TINYDBMS_STORAGE_HPP
