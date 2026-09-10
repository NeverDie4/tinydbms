# Phase 4C：私有多页扫描与 Cursor

## 范围和分层

已实现链路：`CursorRegistry → HeapTable → BufferPool.fetch → const RawPage →
RecordPage::next_record → SlottedPage::next_live_slot/get → RecordCodec::decode`。
磁盘布局、RID packing、public storage.h 均不变。RID DELETE 和公共 CRUD 已在 4D/4E 接通。

MiniOB `record_manager.cpp` 的 `RecordPageIterator::next` 先取当前 slot 的记录，再推进
bitmap 中的 live slot；tinydbms 借鉴枚举和读取分离，改用冻结 Slot Directory、generation
和 optional EOF，不复制 bitmap/-1 sentinel，也不引入事务、日志或长期 pin。

## 页内接口

`SlottedPage::next_live_slot(const RawPage&, PageId, size_t start_position)` 返回
`LiveSlotResult{optional<SlotHandle> slot, next_position, error}`。
先完整 validate，再按 SlotId 升序查找；合法 deleted/retired 跳过，任何结构损坏报 corrupt。
命中 slot i 返回 i+1；目录结束为空 slot/无 error，位置等于 slot_count。
start 超过 slot_count 为 invalid argument；即使 start 已到 EOF 也先验证页面。

`RecordPage::next_record(const RawPage&, PageId, const TableMeta&, size_t)` 返回
`NextRecordResult{optional<Record> record, next_position, error}`。复用上述枚举、RID codec
以及原 get_record 路径；不复制 slot parsing。payload 解码错误原样映射；Record 拥有
values 内存，任何结果都不修改 RawPage。错误不提交下一位置。

## HeapTable 扫描位置

`begin_scan()` 返回 `HeapScanPosition{next_page=1,next_slot=0,page_end_exclusive=page_count}`。
`next_record(HeapScanPosition&)` 返回 owned Record / 空 EOF / error。

next_page、page_end_exclusive 均为 uint64_t；next_slot 为 size_t。开始时验证合法范围，
包括 exclusive end 不超过 UINT32_MAX+1、当前文件边界和 slot 最大数量，然后才转换 PageId。
终点固定不随后续 allocation 扩大。位置只在成功取记录或 table EOF 时提交；错误保留原位置。

free hole 经 allocation-state 跳过；allocated 页必须是 TSP1，否则 corrupt，不能当空页跳过。
页内 EOF 转下一页并重置 next_slot；空 allocated RecordPage 自然参与这一流程。
一页一个局部 guard，所有 record/EOF/error 返回和跨页前都释放。只调用 guard.page()，
不调用 mutable_page/mark_dirty，不直接 PageFile.read_page。
扫描可能因 BufferPool 正常 eviction 写回原先已有 dirty 页，但扫描本身不把页标脏。

## CursorRegistry 和所有权

当前是 storage-private Registry，已由 4E 纳入 StorageState 所有权；HeapTable 不知道 registry。
registry 借用 FileManager/BufferPool，条目拥有 TableMeta 副本与 CursorState；不允许复制或移动
registry，以避免把同一 CursorId 复制成多个可独立存活条目。

`create(meta)` 校验已打开健康表文件并冻结 page boundary；`lookup(id)` 返回状态副本；
`next_record(id)` 按状态推进；`close(id)` 删除有效条目；`clear()` 使全部条目失效；
`has_cursor(table_id)` 包含 Active、Eof、Failed，直到显式 close/clear。
CursorState 仅保存 TableId、HeapScanPosition、status、saved_error，不保存物理指针或 guard。

| 状态 | next_record 行为 | 是否仍在 registry |
|---|---|---|
| Active | 创建临时 HeapTable，推进扫描；成功返回 owned Record | 是 |
| Eof | 重复空 Record/无 error，不再访问页 | 是 |
| Failed | 重复同一 saved_error，不推进、不重试 I/O | 是 |

close 第一次成功，第二次返回 kCursorInvalid；EOF 不自动 close。clear 只清条目，不重置 ID。
成功 Storage close 后清理 registry；池关闭失败恢复 Open 时保留，进入 Closing 后禁止使用。正式生命周期已在 4E 接通。

## CursorId 和错误

进程级静态 optional 计数器从 0 开始，0 可合法签发；所有 registry 共享同一单调序列。
最大 uint64 可签发一次，此后 optional 为空表示耗尽，create 返回 private kInvalidArgument。
不 wrap，不因 clear、registry 重建或 storage reopen 重置。当前单线程，不引入并发 ID 分配。

private CursorErrorKind 保留 kCursorInvalid、kInvalidArgument、kValueTooLarge、kNoVictim、
kIo、kCorrupt。首次任何扫描错误进入 Failed。公共映射：invalid cursor→kCursorInvalid，
invalid/no-victim→kInvalidRequest，too-large→kValueTooLarge，I/O→kIoError，corrupt→kCorrupt。
公共 StorageErrorKind 当前未修改。

## Mutation 约束（4E 接线）

同表存在任何未 close cursor（包括 Eof/Failed），非空公共 INSERT/DELETE 拒绝；空批次在 lifecycle/表存在检查后允许。
多个同表读 cursor 允许且独立推进；其他表 mutation 不受影响。
这是公共 Storage 层职责，不能让 HeapTable::insert_record 反查 registry。
4E 已实现公共检查。page_end snapshot 仅是页面边界，不是事务/记录快照；
测试通过内部 INSERT 验证终点不增长，不表示最终公共 API 允许这种同表并行 mutation。

## Phase 4B poison 收尾

poison 保存在 FileManager 拥有的 PageFile::poisoned_，不在临时 HeapTable；
HeapTable::check_file 使用 PageFile::require_open 检查。原实现已满足，不修改。
新增测试证明临时 HeapTable 销毁后新对象和 cursor create 仍报 I/O；关闭重开后，
遗留 allocated 零页由正常 RecordPage validation 报 corrupt。没有新增恢复系统。

## 测试与边界

`scan_page_test`：empty/one/multiple/order、deleted/retired 跳过、未知/冲突 flags、
generation 0、非 live offset/length、retired generation、布局损坏、非法位置、EOF 前验证、
owned Record/RID、非法 BOOL payload、RawPage 不变。

`heap_scan_test`：空表、单页/多页、free hole 和空页跳过、deleted/retired 遗漏排除、
RID/order reopen、固定页面终点、位置越界、allocated 非 TSP1、I/O/no-victim、capacity 1、
每次返回无额外 pin/无 dirty、双 cursor 独立进度、重复 EOF、Failed 错误粘性、close/lookup/clear、
跨 reopen 不复用 ID、ID 0 与最大值耗尽、Phase 4B poison 的打开生命周期。

CursorRegistry→StorageState、HeapTable DELETE、public CRUD 已完成；FSM、WAL、Transaction、Index 不属于 V1。
