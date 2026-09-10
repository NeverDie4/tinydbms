# HeapTable — Phase 4B 冻结实现

本文描述 Phase 4B 的 INSERT 边界；后续 Phase 4C 已新增扫描能力，见 [heap-scan.md](heap-scan.md)。
Phase 4D 已新增私有 RID DELETE，见 [heap-delete.md](heap-delete.md)；Phase 4E 已接公共 CRUD，见 [storage-crud.md](storage-crud.md)。

## 1. 范围与所有权

仅实现 storage-private 的多页 INSERT 放置。`HeapTable` 保存已解析、不可变
`TableMeta` 副本，借用 `FileManager&` 与 `BufferPool&`；不拥有文件，不缓存
`PageFile*`，不长期持有 `PageGuard`，不维护 registry/FSM。
FileManager、BufferPool 必须覆盖 HeapTable 的使用生命周期。
调用方应保证 schema 与 Catalog 一致；HeapTable 不另建 Catalog，也不解析 SQL。

每张表独立 PageFile；Page 0 是文件 Header。其余页 free 时跳过，allocated
时必须是合法 TSP1 RecordPage，未知/零填充 allocated 页一律 corrupt，不静默初始化。
普通 RecordPage 永不 free；free hole 只来自底层既有 free-list 或未发布新页的补偿。
本阶段单线程，禁止在缓存仍驻留时绕过 BufferPool free/reopen 文件。

## 2. 私有 API

```cpp
PageFileResult<PageAllocationState> PageFile::page_allocation_state(PageId);
// PageAllocationState::{kAllocated,kFree}; Page 0/越界 -> kInvalidArgument

HeapTable(TableMeta, FileManager&, BufferPool&, NewRecordPageIo = {});
HeapTableResult<RecordId> insert_record(const std::vector<Value>&);
HeapTableBatchResult insert_batch(const std::vector<std::vector<Value>>&);
```

allocation-state 复用 persistent free-list，验证完整链（包括命中节点及后续节点），
不解析错误字符串，不改任何磁盘字节布局。不维护额外 allocation bitmap。
查询开销为 free-list 长度；First-Fit 的线性搜索成本属于当前明确接受的限制。

`NewRecordPageIo` 仅包含 allocate/bootstrap_write/compensate_free 的同步窄适配器；
默认调用真实 PageFile。适配器遵守 PageFile result 语义，不重入、不改既有页，
不伪造 allocation identity，不实现缓存。用于精确注入失败，不提供第二条记录写入管线。

## 3. 单行与 First-Fit

每行首先 `RecordCodec::encoded_size(meta, values)`，在任何 fetch/allocate/write
之前检查字段数、类型、UTF-8、VARCHAR 1024 bytes、logical 4096 bytes、encoded
4056 bytes；logical 不含 VARCHAR 长度前缀，encoded 包含。不接受零字节记录。
不提前验证整个 batch；RecordPage 内部继续执行原有 encode，不扩展 encoded-payload API。

按 `PageId=1..page_count-1` 升序：查询 allocation state；free 跳过；allocated
经 BufferPool fetch，调用 RecordPage::insert_record。成功才 mark_dirty 和返回 RID；
NoSpace 只表示继续 First-Fit；其他错误立即停止。RAII 在成功、失败、continue 都释放
guard，一次最多持有一个。不清缓存，不调用 release_table 作为 INSERT 的收尾。

空页也走真实插入判断，不以 live_count==0 推断容量。slot directory 不回收，
空页可能因历史 slot 数量无法容纳大记录。失败的 RecordPage 原子内存修改不新增 dirty，
已有 dirty 状态也不会被清除。

## 4. 新页 C+B 协调协议

```text
PageFile.allocate_page
→ scratch RawPage + RecordPage.initialize
→ PageFile.write_page（唯一直接写例外：fresh、nonresident、未签发 RID）
→ BufferPool.fetch_page
→ RecordPage.insert_record
→ mark_dirty
→ RID
```

只允许本次 newly allocated 页 bootstrap 绕过缓存；普通记录页读写始终经 BufferPool。
新页 bootstrap 写入的是合法空 TSP1；记录本身仍只修改缓存。insert 成功不意味着已落盘，
持久化依赖成功 flush/eviction/close。关闭顺序是 BufferPool.close → FileManager.close_all。
不承诺断电一致性、失败写盘原子性或事务回滚。

## 5. 失败补偿

| 失败位置 | 状态与处理 |
|---|---|
| allocate | 返回原错误；无已知 PageId 时绝不 free |
| initialize/bootstrap | 对已知且 nonresident 页尝试 free；成功保留 free hole，返回原错误 |
| bootstrap 与 free 均失败 | 返回 kIo，消息保留两次失败；设置现有 PageFile 内存 poisoned latch，后续 HeapTable（包括重新构造对象）拒绝继续操作该打开文件 |
| bootstrap 成功、fetch 失败 | 保留合法 empty TSP1 allocated 页，不 free；下次 First-Fit 自然复用 |
| fetch 成功、insert 失败 | 保留原有合法页，不新增 dirty，释放 guard |

poison 不持久化，不增加恢复管理器；必须处理错误并关闭资源。重开按实际文件状态验证，
allocated 非 TSP1 在后续 HeapTable INSERT 中报 corrupt，不能假设 close/reopen 自动修复。
正常预验证后，全新空页 NoSpace 不应发生；若发生按内部不变量损坏 kCorrupt 返回，
而非无限分配新页。

## 6. 错误与 batch

private `HeapTableErrorKind`：kInvalidArgument、kValueTooLarge、kNoVictim、kIo、kCorrupt。
Codec invalid/too-large 原义映射；BufferPool no-victim 单独保留；PageFile/BufferPool
I/O 与 corrupt 原义保留；没有已打开表文件为 private invalid argument。
公共层已将 invalid/too-large/IO/corrupt 映射到已有对应错误；no-victim 映射 kInvalidRequest，
附资源不可用消息。没有增加公共 StorageErrorKind。

batch 是逐行 non-transactional、non-atomic、stop-on-first-error；结果同时包含
失败前已成功 RID 前缀和首个错误；不回滚已成功行。表文件已解析、打开且健康时，
空 batch 返回空成功。预留 RID 向量容量在写第一行之前完成。

## 7. 测试与自审

`page_allocation_state_test.cpp`：allocated/free、复用、reopen、Page 0、越界、
closed、损坏 free node（包括查询该节点自身），以及 reopen corruption。

`heap_table_test.cpp`：首次 Page 1/RID、同页/多页、升序 First-Fit、早期删除空间与
空页复用、空页 slot 膨胀、free hole reopen/skip、allocated 非 TSP1、输入错误零分配
与零 fetch、allocation/bootstrap/compensation/read/dirty-victim-write 错误、
双重失败健康状态、fetch 失败空页保留和重试、post-prevalidation 插入失败、批量成功
与前缀、两表隔离、capacity 1、无 pin 泄漏、close/reopen 后 RecordPage 级数据恢复。
post-prevalidation 测试通过 friend 直接调用私有协调器传入错误行，验证通常被预验证
挡住的防御路径；不增加生产 mutation hook。lower-level erase 仅用于测试夹具准备。

MiniOB 对照：`RecordFileHandler::insert_record`
（`src/observer/storage/record/record_manager.cpp`）同样分开“寻找可用页”和“分配初始化新页”，
并及时释放 pin。tinydbms 只借鉴分层职责，使用升序 First-Fit + PageGuard；不移植
MiniOB 的 free_pages 集合、定长 record page、锁、日志或 DiskBufferPool 分配接口。

## 8. 明确未实现

Cursor、SCAN、DELETE 和 public CRUD 已由 4C–4E 实现。V1 未实现
SQL/Executor、跨页单条记录、WAL、Transaction、FSM、Index。
PageFile/RecordCodec/SlottedPage/RecordId/BufferPool 的格式和公共契约不重新设计。
