# Phase 4D：HeapTable RID DELETE

## 范围与 API

`HeapTable::delete_record(RecordId)` 返回 optional HeapTableError；
`delete_batch(vector<RecordId>)` 返回 `HeapDeleteResult{uint64_t deleted_count, error}`。
两个接口均 storage-private；不改 storage.h 或公共 Storage CRUD 实现。
不增加 CursorRegistry 依赖、WHERE、FSM、Index、WAL 或 Transaction。

## 执行链

```text
RecordIdCodec::decode
→ check_file（包括 PageFile poison）
→ PageFile::page_allocation_state（含 logical range）
→ BufferPool.fetch_page(TableId, PageId)
→ RecordPage::get_record（目标 payload 校验）
→ RecordPage::erase_record
→ mark_dirty
```

decode 拒绝 PageId 0 / generation 0；allocation-state 拒绝越界，free 明确映射 invalid RID。
allocated 非 TSP1、损坏页/slot/目标 payload 返回 corrupt。slot live、generation、deleted、
retired 校验全部委托既有 RecordPage/SlottedPage，HeapTable 不重复解析。

原有 erase_record 无 schema 参数，仅检查页结构和 slot。为避免静默删除损坏 payload，
在同一个 guard 内通过既有 get_record 解码验证目标记录；不重新设计底层接口或磁盘布局。
其他记录 payload 不在本次删除中逐条解码；页结构仍接受完整 SlottedPage validation。

## Table-scoped RID

RID 必须与其来源 TableId 一起使用。Storage 不保证检测跨表 PageId/SlotId/generation
数值完全碰撞情况下的来源错误。RID 不编码 TableId，也没有来源注册表。
目标表中越界、free、slot 不存在、generation 不匹配属于可检测 invalid；相同 RID.value
可在两个表内各自合法，操作仅作用于当前 HeapTable 的 table scope。

## Mutation、页面和 guard

mutable_page 本身不 dirty；只有 erase 成功才 mark_dirty。失败不改变页字节、不新增 dirty，
也不清除原 dirty。单次删除仅持有一个局部 PageGuard，成功/失败均 RAII 释放。
不存在直接 PageFile.read/write/free 普通 RecordPage，也不通过 release_table 清缓存。

最后一条记录删除后，页仍 allocated/TSP1，slot_count 与 generation 历史保留，
不缩减目录、不 free。后续 First-Fit 自然复用空间；generation 在复用时增加，
65535 删除后 retired 且永不复用。旧 RID 不得删除新记录。
成功删除先在 BufferPool 中生效，持久化依赖成功 flush/eviction/close；不承诺断电恢复。

## Batch 与错误

逐个 RID 执行、non-transactional、non-atomic、stop-on-first-error；返回成功前缀计数，
不回滚。重复 RID 第二次失败，后续 RID 不执行。表文件存在且健康时空 batch 为 0/成功。
单行/批量继续检查现有 PageFile poison，不绕过 Phase 4B 的失败生命周期。

invalid/stale/deleted/retired、Page 0/out-of-range/free → private kInvalidArgument；
BufferPool no victim → kNoVictim；I/O → kIo；malformed allocated page/slot/payload → kCorrupt。
公共 StorageErrorKind 不变，invalid/no-victim→kInvalidRequest、I/O→kIoError。

同表未 close cursor 阻止非空公共 INSERT/DELETE 的规则，已在 Phase 4E Storage orchestration 实现；空批次允许。
HeapTable 是纯物理表操作，不知道 active cursor。

## MiniOB 对照

MiniOB `record_manager.cpp::RecordFileHandler::delete_record` 通过页 handler 初始化、
删除并 cleanup，再维护 free_pages 集合。tinydbms 借鉴“定位页→页内删除→释放资源”的边界；
使用 PageGuard RAII、不加锁/日志/free-space registry、不归还普通 RecordPage。

## 测试

`heap_delete_test.cpp` 覆盖：单条/中间/最后记录、邻居保留、不可读旧 RID、重复/stale、
generation 复用和 retired、页 0/越界/free/slot 越界、非 TSP1、损坏 slot/payload、
可检测 wrong-table 与完全相同 RID 碰撞、成功 dirty、失败 clean/pre-dirty 保留、
no-victim、窄 read I/O 注入、成功/失败无 pin 泄漏、批量全成功/前缀/重复 RID/空、
delete 和 reuse 后 reopen、两表隔离、capacity=1 多页删除以及空页 allocation 保留。

公共 delete_records/insert/open_table/scan_next/close_cursor 已在 Phase 4E 接线，详见 storage-crud.md。
