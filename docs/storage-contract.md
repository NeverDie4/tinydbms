# tinydbms Storage Contract

本文件说明现行 Storage 公共边界；唯一正式声明为 `include/tinydbms/storage.hpp`。

## 边界

Core 负责 catalog snapshot、Plan 调度、表达式、WHERE 与 projection。Storage 只接受 typed request，负责 metadata、表文件、记录、页面和 cursor；它不认识 SQL、Expr、Filter 或 Project。Core 仅调用 `.hpp` 中的 `storage` namespace API，不访问 Storage 私有对象。

## 类型、ID 与记录

支持 `Type::kInt` 与 `Type::kVarchar`。INT 是 signed int32 的四字节 little-endian；VARCHAR 是 uint32 little-endian 长度与 UTF-8 内容。逻辑行最多 4096 字节，VARCHAR 最多 1024 UTF-8 字节，单记录不能跨页。

`TableId` 完整 uint32 值域有效，0 合法。`RecordId` 是 table-scoped opaque value，Core 不解释。`Record` 是 `{rid, values}`，values 永远按 `TableMeta.columns` 顺序并按值返回。

metadata 写入 `INT32`，读取兼容 `INT`/`INT32`；历史 `INT64/FLOAT/DOUBLE/BOOL` schema 返回明确错误，不再是 Storage 支持类型。

## API 与结果

`open_storage`、`close_storage`、`list_tables`、`create_table`、`open_table`、`scan_next`、`close_cursor`、`insert`、`delete_records` 均采用 `.hpp` 中的 request/result 对象。

`OpenTableResult.cursor` 是成功 cursor。`ScanNextResult` 的 `record` 空且 `error` 空表示 EOF。`InsertResult.rids` 和 `DeleteResult.deleted_count` 分别报告已成功的前缀；它们不提供事务回滚。

## 生命周期与 cursor

Storage 是 singleton：正常数据操作仅在 open 状态可用。正常 close 后 metadata 和数据能在 reopen 读取；不承诺 WAL、掉电恢复或每次操作 fsync。

同表未关闭 cursor 阻止非空 INSERT/DELETE。Core 必须在 EOF 和所有失败路径关闭 cursor。DELETE 的正确调用顺序是扫描、收集 RID、关闭 cursor、批量删除。

## 错误与非目标

公共错误为 `kTableNotFound`、`kCursorInvalid`、`kValueTooLarge`、`kIoError`、`kCorrupt`、`kInvalidRequest`。Storage 不暴露 Page 或 Buffer 专用错误。

System Catalog、FSM、Index、WAL、Transaction、MVCC、schema evolution 和多进程文件锁不在当前范围。
