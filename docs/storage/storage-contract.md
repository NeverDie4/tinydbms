# tinydbms Storage Contract

本文件说明现行 Storage 公共边界；唯一正式声明为 `include/tinydbms/storage.hpp`。

## 边界

Core 负责 catalog snapshot、Plan 调度、表达式、WHERE 与 projection。Storage 只接受 typed request，负责 metadata、表文件、记录、页面和 cursor；它不认识 SQL、Expr、Filter 或 Project。Core 仅调用 `.hpp` 中的 `storage` namespace API，不访问 Storage 私有对象。

## 类型、ID 与记录

支持 `Type::kInt` 与 `Type::kVarchar`。INT 是 signed int32 的四字节 little-endian；VARCHAR 是 uint32 little-endian 长度与 UTF-8 内容。逻辑行最多 4096 字节，VARCHAR 最多 1024 UTF-8 字节，单记录不能跨页。

`TableId` 是 uint32 值。TableId 0 (`tdb_sys_tables`) 与 1 (`tdb_sys_columns`) 为 Storage 保留的 system table；普通用户 `create_table` 只能使用不小于 2 的 ID，也不能使用这两个保留名称。`RecordId` 是 table-scoped opaque value，Core 不解释。`Record` 是 `{rid, values}`，values 永远按 `TableMeta.columns` 顺序并按值返回。

`storage.meta` 是 V2 bootstrap，不携带用户 schema。用户表 schema 由 `tdb_sys_tables` 与 `tdb_sys_columns` 中的 `INT32`/`VARCHAR` catalog records 持久化；未知 bootstrap、损坏的 system PageFile、逻辑 catalog 或 catalog/file 不一致均 fail-closed 为 `kCorrupt`。历史 V1 metadata 不迁移且返回明确错误。

## API 与结果

`open_storage`、`close_storage`、`list_tables`、`create_table`、`open_table`、`scan_next`、`close_cursor`、`insert`、`delete_records` 均采用 `.hpp` 中的 request/result 对象。

`OpenTableResult.cursor` 是成功 cursor。`ScanNextResult` 的 `record` 空且 `error` 空表示 EOF。`InsertResult.rids` 和 `DeleteResult.deleted_count` 分别报告已成功的前缀；它们不提供事务回滚。

`list_tables` 返回 system table 与用户表，使 Core 的 CatalogView 能用常规 SELECT 读取 system catalog。普通 public `insert` 和 `delete_records` 对 TableId 0/1 返回 `kInvalidRequest`；内部 catalog helper 不属于 public API。

## 生命周期与 cursor

Storage 是 singleton：正常数据操作仅在 open 状态可用。BufferPool 关闭失败会恢复 Open 并保留 cursor；文件或 metadata 收尾失败会保持 Closing。两类失败都必须由当前 owner 重试 `close_storage`，完整成功后才释放状态并允许 reopen。已 Closed 的重复 close 是成功 no-op。

正常 close 后 metadata 和数据能在 reopen 读取；不承诺 WAL、掉电恢复或每次操作 fsync。

同表未关闭 cursor 阻止非空 INSERT/DELETE。Core 必须在 EOF 和所有失败路径关闭 cursor。DELETE 的正确调用顺序是扫描、收集 RID、关闭 cursor、批量删除。

## 错误与非目标

公共错误为 `kTableNotFound`、`kCursorInvalid`、`kValueTooLarge`、`kIoError`、`kCorrupt`、`kInvalidRequest`。Storage 不暴露 Page 或 Buffer 专用错误。

FSM、Index、WAL、Transaction、MVCC、schema evolution、V1 自动迁移、DROP 和多进程文件锁不在当前范围。普通 CREATE 仅具有正常 API 失败下的最小补偿；不承诺 crash-atomic DDL。
