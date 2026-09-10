# tinydbms Storage Contract（Phase 0 冻结）

## 1. Scope / Non-goals

本文冻结 core 与 storage 的公共边界、逻辑类型、ID、错误、生命周期和 metadata 持久化承诺。Phase 2A/2B 后，Record binary format 与 Slotted Page 私有格式由 [`record-page-format.md`](record-page-format.md) 单独冻结；BufferPool、替换策略、索引、事务、WAL、MVCC 或恢复仍不在本文范围内。

## 2. Core ↔ Storage boundary

core 负责 Logical Catalog、SQL/AST/Expr/WHERE、Filter、Project 和执行协调。storage 只接受类型化请求，不认识 SQL、AST、Expr、WHERE、LogicalPlan、Filter 或 Project。

允许的业务入口固定为：`create_table`、`insert`、`open_table`、`scan_next`、`close_cursor`、`delete_records`。core 只通过 `storage.h` 的自由函数调用 storage；本阶段不改为 `Storage` 类。

## 3. Shared types

`Type` 与 `Value` 的 variant alternative 一一对应：`kInt32/std::int32_t`、`kInt64/std::int64_t`、`kFloat/float`、`kDouble/double`、`kBool/bool`、`kVarchar/std::string`。Core SQL 初版支持 INT32、VARCHAR；其余是可持久化的 Extended 类型。

`kMaxVarcharBytes` 为 1024，`kMaxRowLogicalBytes` 为 4096。logical row size 是 fixed-width value bytes 与 VARCHAR UTF-8 payload bytes 之和，不包含 VARCHAR 的 4-byte 物理长度前缀。空 Slotted Page 的最大 encoded payload 为 4056 bytes；两个上限失败均映射为 `kValueTooLarge`。Storage 必须独立校验行列数、每列类型、UTF-8 和这些上限。NULL、CHAR、DECIMAL、DATE/TIME、BLOB 不属于本阶段。

## 4. ID semantics

`TableId` 的完整 `uint32_t` 值域有效，0 合法，空库第一张表使用 0。公共 ID 不使用 magic invalid sentinel；不存在或不可用的值由 `optional`、Result 和 `StorageError` 表达。

`RecordId { uint64_t value; }` 是 table-scoped opaque handle：仅 storage 生成，core 只能原样保存并与来源 TableId 一起传回 `delete_records`，不得解析其物理含义。私有编码冻结为 `PageId:32 | generation:16 | SlotId:16`，其中 generation 0 永不签发。`PageId`、`SlotId`、`FrameId` 只存在于 storage 私有实现，不进入公共头文件或 core。

## 5. Record / metadata semantics

`Record.record_id` 由 storage 分配。`Record.values` 的顺序严格对应 `TableMeta.columns`，且跨 API 边界按值拥有，不能借用未来 Page/Buffer 内存。

`TableMeta` 保存逻辑 `table_id`、表名和按逻辑列序排列的 `ColumnMeta`；它不是 Page 或文件布局。metadata 必须能完整恢复所有已支持 Type。
旧 `TINYDBMS_STORAGE_V1` metadata 中的 `INT` 标签在读取时兼容映射为 `INT32`；新写入统一使用 `INT32`。

Record encoding 由 TableMeta schema 驱动，不在每条记录中重复保存 column count、Type 或 schema version。因此表创建后 schema 不可变；ALTER TABLE 和 schema evolution 不在当前范围内。

## 6. Lifecycle

私有状态为 `Closed → Open → Closing → Closed`，不进入公共头文件。Open 才允许正常操作；double open 或 Closing 时 open 返回 `kInvalidRequest`。关闭先 BufferPool.close，再 FileManager.close_all，再 metadata 收尾。池关闭失败且文件完整时恢复 Open；池成功关闭后的文件/metadata 失败保持 Closing，拒绝正常操作，允许再次 close 重试收尾。Closed 时 close 成功 no-op。

StorageState owns FileManager、BufferPool、CursorRegistry；创建按此顺序，销毁相反。默认 64 Frames、FIFO，LRU 为私有创建配置；不扩公共 API。已有记录页禁止绕过 BufferPool write/free；HeapTable 新页按 allocate → scratch initialize → bootstrap write → fetch。通过 guard 修改后显式 mark_dirty，正常 close 成功保证写回，不承诺 WAL/断电原子性。

目录不存在时创建；空目录是新库；非空但缺少合法 metadata 为 `kCorrupt`；文件系统失败为 `kIoError`。公共 API 不得泄漏 `std::filesystem_error`。

## 7. Public API overview

- `open_storage(OpenStorageRequest)` / `close_storage()`：打开和关闭 storage。
- `list_tables()`：返回持久化表元数据的值拷贝。
- `create_table(CreateTableRequest)`：创建 metadata；core 负责分配 TableId。storage 校验表名/列名为规范化的 `[a-z][a-z0-9_]*`、长度 1..64、列清单非空、列名不重复、Type 合法，以及 TableId/表名不重复。
- `insert(InsertRequest)`：按顺序处理完整行，非事务、可部分成功。
- `open_table(OpenTableRequest)` / `scan_next(CursorId)` / `close_cursor(CursorId)`：提供逐行扫描。
- `delete_records(DeleteRequest)`：按 opaque RecordId 删除，非事务、可部分成功。

`OpenTableResult` 成功时 `cursor_id` 必有值。`ScanNextResult` 三态为：有 `record`、无 record 且无 error 的 EOF、或有 error 的失败。

## 8. Error model

公共错误固定为 `kTableNotFound`、`kCursorInvalid`、`kValueTooLarge`、`kIoError`、`kCorrupt`、`kInvalidRequest`。不增加 Page/Buffer 专用公共错误。

INSERT/DELETE 不是事务：遇错停止，已经成功的行不回滚；结果只反映失败前成功的 `record_ids` 或 `deleted_count`。SELECT 扫描出错时由 core 丢弃已收集行。

## 9. Cursor ownership and lifetime

Phase 4E 已将 CursorRegistry 纳入 StorageState 并接通公共 CRUD，见 [storage-crud.md](storage-crud.md)。
CursorId 0 合法，进程生命周期单调、不回绕、不因 reopen 重置；EOF/Failed 仍占 registry，
直到 close。同表未 close cursor 阻止非空公共 INSERT/DELETE；空批次在 lifecycle/表存在校验后
成功，其他表 mutation 不受影响。HeapTable 不依赖 registry。

Closed 时 scan_next/close_cursor 保留 kCursorInvalid，insert/open_table/delete_records 返回
kInvalidRequest；Closing 时五个数据 API 均返回 kInvalidRequest。成功 close 清 registry，
BufferPool close 失败恢复 Open 时保留 registry；不重置进程级 CursorId allocator。

storage 拥有 cursor registry 与扫描状态，core 仅持有 `CursorId`。core 必须在正常 EOF 或任何扫描错误后调用 `close_cursor`；close 后以及成功 `close_storage` 后的 CursorId 均无效。cursor 不保存长期 PageGuard 或 Table/File 物理指针。

## 10. Persistence semantics

正常 `close_storage()` 后，metadata 和表数据必须在 reopen 时可见；不承诺每次操作 fsync、崩溃恢复或断电安全。metadata 更新采用同目录临时文件、确认写入/显式关闭成功、再替换正式文件；写新 metadata 失败不得破坏此前可打开的 metadata。读取时拒绝重复 TableId/表名/列名、非法 identifier/type、缺失结束标记和额外 token/尾部数据；真实 open/read 错误归 kIoError，非法内容归 kCorrupt。

Record binary serialization 必须是显式紧密编码：INT32 4 bytes、INT64 8 bytes、FLOAT 4 bytes、DOUBLE 8 bytes、BOOL 1 byte、VARCHAR 为 4-byte little-endian 长度加 UTF-8 bytes；不得写入 C++ 对象内存布局。完整格式见 [`record-page-format.md`](record-page-format.md)。

## 11. Private Page / future Buffer / FileManager boundary

Page Size 冻结为 4096 bytes。Phase 1A 的独立 raw PageFile 格式见
[`page-file-format.md`](page-file-format.md)，Phase 1C/1D 的 FileManager 所有权及
Storage 集成见 [`file-manager.md`](file-manager.md)。它们仍是 storage 私有实现，不改变本公共契约。单条 Record 不跨 Page，encoded payload 最大 4056 bytes，逻辑行最大 4096 bytes。

Physical page layout, buffer frames and file structures are private
storage implementation details and must not leak through the
core/storage public boundary.

StorageState 当前拥有 FileManager 和 BufferPool，FileManager 按 TableId 拥有 PageFile；`create_table` 已负责先创建表文件再提交 metadata。BufferPool 非拥有地使用 FileManager，PageGuard/pin 在 storage 内部管理。core 永不持有 Page 或 Frame。FileManager 不实现 page cache，当前不引入 latch/线程。

## 12. Explicit non-goals

Phase 4C/4D 已实现私有多页 scan/registry 与 RID DELETE，见 [heap-scan.md](heap-scan.md)、
[heap-delete.md](heap-delete.md)；Phase 4E 已完成公共 CRUD、游标限制与 StorageState 接线。

Phase 2 已实现 RecordCodec/SlottedPage/RecordPage/RID 及持久化测试；Phase 3 已实现 BufferPool 与生命周期；Phase 4 已实现 HeapTable 与公共 Storage CRUD。SQL 执行器、B+Tree、事务、WAL、MVCC 和 Index 尚未实现。

RecordPage 的内存修改与 PageFile::write_page 是两个显式步骤，只有写入成功才能报告持久化成功；不承诺失败写入的磁盘原子性。close/reopen 后 RID、slot generation、删除状态和 compact 后记录均通过恢复测试。stale RID 返回 private invalid argument，结构或 payload 损坏返回 corrupt。详见 `record-page-format.md` 第 16 节；公共 Storage API 与错误种类不变。

## 13. Contract tests

测试覆盖 TableId 0、metadata/类型/错误与生命周期基础契约，以及公共 CRUD、cursor 三态、写入限制、批量前缀、close/reopen 值与 RID 持久化；详见 [storage-crud.md](storage-crud.md)。
