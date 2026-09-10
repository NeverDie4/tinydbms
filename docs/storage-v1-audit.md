# Storage Engine V1 Final Audit

审计范围：Phase 0–4 typed heap storage；不是 SQL DBMS 全课程验收。
使用 code-reviewer 的正确性、资源、缓存、错误和测试清单，人工检查 C++ 实现；没有对不支持 C++ 的脚本评分作背书。

## A. 实际架构

```text
public storage.h / storage.cpp
  ├─ INSERT / DELETE ──────────────────── HeapTable
  └─ open_table / scan_next ─ CursorRegistry ─┘
HeapTable
  ├─ BufferPool ── FileManager lookup ── PageFile ── Disk
  ├─ RecordPage ── SlottedPage（只处理 RawPage 字节）
  │             ├─ RecordCodec（schema / Value）
  │             └─ RecordIdCodec
  └─ FileManager / PageFile allocation-state + 新页 bootstrap/补偿
```

这不是 `SlottedPage → PageFile` 的源码依赖：RecordPage/SlottedPage 是无 I/O 的内存变换，
由 HeapTable 在 BufferPool guard 上调用。FileManager 独占 TableId→PageFile 映射。
`storage.cpp` 不调用 physical RecordPage CRUD；core/public headers 没有 PageId/SlotId/Frame。
依据：`heap_table.cpp:92/118/52/159`，`record_page.cpp:29/55/67/78`，`buffer_pool.cpp:117/184`。

## B. Identity model

| ID | 类型 / 公开性 | 唯一范围、持久化 | 0 / 复用 / stale 防护 |
|---|---|---|---|
| TableId | uint32，公开 | 一个库的 Catalog，storage.meta/文件名 | 0 合法；V1 无 DROP，不自动回收；重复 create 拒绝 |
| PageId | uint32，私有 | 一个 PageFile，header/RID/offset | 0 是文件 header；raw free-list 可复用；普通 RecordPage 不 free |
| FrameId | size_t，私有 | 一个 BufferPool 的数组索引，不持久化 | 0 合法；eviction 复用；guard pin 防止在用 frame 被替换 |
| SlotId | uint16，私有 | 一页的 slot directory/私有 RID | 0 合法；deleted 可复用，generation 增加；retired 不复用 |
| RecordId | opaque uint64，公开 | TableId scope；页/slot/gen 可恢复，无单独 RID 字段 | 0 不会签发；同物理 slot 复用后 RID 不同；gen65535 retire |
| CursorId | uint64，公开 | 进程生命周期，不持久化 | 0 合法；不回绕/不复用，reopen 不重置；进程重启后旧句柄不可携带 |

来源：`types.h`、`page_types.h`、`buffer_pool.h:17/25`、`slotted_page.h`、`record_page.cpp:41`、`cursor_state.cpp:61`。
公共 ID 没有 magic invalid sentinel；PageFile 内部 free head=0 是冻结的私有链终止表示。
跨表 RID 数值可完全碰撞，不能检测所有错误来源；必须与来源 TableId 一起使用。

## C. 磁盘格式对照

| 层 | 实际格式 | 一致性结论 |
|---|---|---|
| PageFile | 4096 bytes；Page0：TDBPAGE1@0，version uint32@8，free head uint32@12，page_count uint64@16 | LE；逻辑 count 包括 header；允许 aligned tail；与格式一致 |
| Free Page | TFR1@0，next uint32@4 | LIFO，reserved 写零、读取忽略；文档已明确 |
| RecordPage | TSP1/version1，header32；self ID、slot/live、lower/upper；reserved 必须零 | 与代码一致，未改格式 |
| Slot | offset/length/generation/flags 四个 uint16，共8 bytes | LIVE/deleted/retired，未知冲突 flags 报 corrupt |
| Payload | INT32/64 4/8，FLOAT/DOUBLE 4/8 bit_cast，BOOL 1，VARCHAR uint32 LE长度+UTF-8 | 六类型一致；无 type/RID/null/schema tag |

逻辑上限4096不含 VARCHAR prefix；encoded4056包含 prefix；VARCHAR1024 bytes。
Codec 检查 UTF-8、列数/类型、short/trailing/BOOL；Page validation 检查范围、重叠、连续 payload 和计数。
删除按 SlotId 升序向下重建 scratch payload，不改 slot/gen；最后 live 删除 upper=4096，目录不缩减。
PageFile reserved 宽松读取与 RecordPage reserved 严格校验是已冻结的不同层规则，不应混写。
发现的文档不一致：free-list allocate 实际先更新 header 再清零，旧文档写相反；已修文档，不改格式。

## D. 持久化不变量

- Catalog 每个表必须有 `tables/table_<id>.dat`；create 先文件、再 metadata，提交失败补偿新文件。
- orphan 不覆盖；metadata 声明的缺文件/坏文件 header 在 open 报错。不会全面清理 orphan。
- open 验证 PageFile header/free list，不 eager 解码所有 heap 页；allocated 非 TSP1 在访问时 corrupt。
- INSERT/DELETE 在缓存生效；成功 eviction/flush/close 才写回。析构不替代显式成功关闭。
- close：BufferPool→FileManager→metadata→清 CursorRegistry/state；文件关闭失败后可重试收尾。
- reopen 保留值、slot generation、deleted/retired 和 free holes。slot reuse 不复活旧 RID。
- Cursor state、frame/pin/dirty/缓存统计不持久化；新 pool 统计从零起，CursorId 分配器在同进程不重置。

## E. 失败语义

| 条件 | 语义 |
|---|---|
| invalid request / Value-too-large | kInvalidRequest / kValueTooLarge |
| missing table / cursor | kTableNotFound / kCursorInvalid |
| stale/deleted/retired RID、page0/out-of-range/free | invalid RID，不是 corrupt |
| allocated malformed page/slot/目标 payload | kCorrupt；不可静默修复/跳过 |
| NoSpace | 仅 existing-page First-Fit 继续；新空页预验证后 NoSpace 为不变量异常 |
| NoVictim | private kNoVictim→public kInvalidRequest，停止，不继续 allocate |
| bootstrap write 失败 | nonresident 已知新页尝试 free；成功返回原错 |
| bootstrap + compensation 失败 | kIoError，PageFile 内存 poison 覆盖当前打开周期 |
| bootstrap 后 fetch/insert 失败 | 保留合法页，无普通 RecordPage free |
| eviction target read 失败 | 不写 victim，不提交映射/eviction |
| dirty victim write 失败 | victim/dirty/mapping 保留；失败写盘不保证物理原子性 |
| pool close 失败 | 恢复 Open，保留 cursors；file/metadata 收尾失败保持 Closing |
| batch 部分失败 | 停在首错，成功 RID/count 前缀保留，不回滚 |

错误按 enum 映射，无生产错误字符串解析。Closed 的 scan/close_cursor 为 kCursorInvalid，
其他数据入口为 kInvalidRequest；Closing 五个入口均拒绝。EOF/Failed cursor 仍阻止同表非空写入。

## F. Ownership / lifetime

StorageState owns FileManager、BufferPool、CursorRegistry；后两者借用前层资源。
FileManager owns 不可复制/移动的 PageFile，BufferPool owns 固定 Frames 和 PageTable。
HeapTable owns schema 副本，仅借用 manager/pool，无长期 PageFile*。
Cursor 条目拥有 schema/state，不保存 HeapTable/PageFile/Frame/RawPage 指针或 guard。
PageGuard move-only，析构 unpin；错误/EOF/continue 返回均作用域释放。
错误使用内部 guard 生命周期会 terminate，属于编程错误，不是可恢复公共输入错误。

## G. 生产直接 PageFile I/O 清单

1. `buffer_pool.cpp:219` read_page：cache miss 读 scratch，合法。
2. `buffer_pool.cpp:127` write_page：dirty flush/eviction，合法。
3. `heap_table.cpp:136` write_page：刚分配、从未驻留、未签发 RID 的空页 bootstrap，唯一 HeapTable 直写例外。
4. `heap_table.cpp:123` free_page：bootstrap 失败后的明确 nonresident 新页补偿，不释放普通记录页。

其余 read_raw/write_raw/write_header 属于 PageFile 自身文件格式实现；测试 fixture 不计生产旁路。
FileManager 的 create/open/close/remove 用于文件生命周期，不做 page cache。

## H. CRUD 调用链

- INSERT：`storage::insert → HeapTable::insert_batch → encoded_size → allocation-state/First-Fit → pool fetch → RecordPage::insert_record → mark_dirty`。
- SCAN：`open_table → CursorRegistry::create → begin_scan`；`scan_next → registry → HeapTable::next_record → pool fetch → RecordPage::next_record → SlottedPage enum/get + decode`。
- DELETE：`delete_records → HeapTable::delete_batch/delete_record → RID decode/allocation-state → pool fetch → RecordPage get/erase → mark_dirty`。

public 层只做 lifecycle、表查找、cursor restriction、协调和错误映射，未复制物理 CRUD。

## I. 压力测试与 metrics

新增 `storage_v1_stress_test`：6组（FIFO/LRU × capacity1/2/3），每组两表。
最大组每表5000行/66个数据页，共10000行；删除50%后重插5000行，全部复用原 slot、generation+1、旧 RID 均拒绝。
其他5组每表600行/8页；每组3轮 reopen 后 CRUD，再次 reopen 全量核对六类型值/RID。
所有组验证无 pin 泄漏、eviction/dirty flush>0、`fetch=hit+miss`，总计初始16000行、复用8000行。

最大组初轮记录：fetch536336、hit34964、miss501372、eviction501371、dirty_flush14908。
计数覆盖初始插入/删/复用/扫描，不含后续新 pool 的 reopen 循环；计时仅说明 CI 可承受，不是 benchmark。

统计口径：基础非法参数不计 fetch；通过基础检查后每次 lookup 计 fetch 和 hit/miss；
read/no-victim 失败仍计 miss；eviction 仅完整 commit；dirty_flush 仅成功 write；零 fetch hit_rate=0。
日志是可选私有 LogSink，默认关闭，不是 SQL EXPLAIN/CLI 监控。

答辩序列（capacity2，每次 release）：1,2,1,3,1。
FIFO：M,M,H,M,M，hit1/miss4，20%，eviction2；LRU：M,M,H,M,H，hit2/miss3，40%，eviction1。
新增 policy_demo 断言该精确结果和事件输出，不引入监控系统。

## J. 课程要求映射

| 课程要求 | 实现与测试 | 状态 |
|---|---|---|
| fixed-size Page / allocate/read/write/free | PageFile；page_file/page_allocation_state | 完成 |
| Record serialization / table pages | RecordCodec/SlottedPage/HeapTable；codec/page/heap测试 | 完成 |
| Buffer/cache/FIFO/LRU | BufferPool；buffer_pool/flush/fifo、stress | 完成 |
| 命中统计/替换可观测 | stats/LogSink；fifo/stress policy_demo | 私有层完成；无公开查询/CLI展示 |
| 持久化 CREATE/INSERT/SCAN/DELETE/reopen | storage_crud/storage_v1_stress | Typed API 完成，不是 SQL 完成 |
| Catalog 持久化 | storage.meta；storage_contract/v1_metadata | 已持久化；课程“系统目录作为特殊表”尚未实现 |
| Lexer/Parser/Semantic/Planner/Optimizer/Executor/WHERE/Project | compiler 仅语句切分等骨架，core 调度骨架 | 未完成，非 Storage V1 blocker |

依据仓库 CODEX_CONTEXT.md 第3–19、21、26节。不能用存储测试代替完整 SQL 课程验收。

## K. 本轮修复与文档

1. 确定 bug：read_metadata 接受重复表名/ID、重复列、非法名字及额外 token/尾部数据。
   `storage_v1_metadata_test` 先复现接受损坏目录，再修为完整校验；文本 V1 格式不变。
2. metadata 临时写入补显式 close 成功检查后才 replace；open/read 环境错误与 corrupt 分开。
   close 的设备级失败未单独注入，代码路径审查及既有 metadata failure-atomic 测试覆盖其上下文。
3. PageFile allocate 原先可消费打开后损坏的 free-list 自环；page_allocation_state 测试复现，
   现先复用完整链校验再改 header/清零。
4. 清理 README、storage-contract、record-page-format、buffer-pool、heap-table/scan/delete 的过时阶段描述；
   修正 PageFile allocate 次序、reserved 读取行为，保留 wrong-table 完全碰撞限制。

## L. 已知限制（非本次 blocker）

单线程、同表未 close cursor 阻止非空写；无 WAL/断电原子性；无 checksum，无法检测所有合法形状的位翻转；
不支持 schema evolution/NULL/跨页记录；slot directory 不回收、retired 耗空间；First-Fit 线性搜索、
页内完整验证/compact，stress 小池 miss 高，不承诺吞吐；Catalog 不是 heap 特殊表。
原始 PageFile free/reopen 必须遵守缓存协调前提；没有跨实例文件锁、多进程/外部并发写支持。
内存耗尽等标准分配异常不在所有层归一化为公共 Result，V1 不承诺资源耗尽下完整恢复。
本次只验证当前 Windows/MinGW Debug 环境，不能宣称已通过 Linux/MSVC 实机回归。

## M. 最终验证

当前 Windows/MinGW Debug 环境验证：

- `cmake --preset debug`：通过。
- `cmake --build --preset debug`：通过。
- `ctest --preset debug`：22/22 通过，全量耗时 8.61 秒，新增 stress 测试 6.99 秒。
- `git diff --check`：通过。

耗时为本轮一次执行记录，不代表性能保证；没有声称 Linux/MSVC 已验证。

## N. 冻结结论

已识别 Phase 0–4 确定缺陷均做最小修复，全量回归通过，可冻结为 **Storage Engine V1（Typed Heap Storage）**。
这不表示 SQL DBMS 或全部课程要求已经完成，也不增加事务、索引或 FSM 的承诺。

## O. 下一阶段建议（未实现）

先审计 compiler/core 现有契约，设计从 typed storage 到最小 SQL CREATE/INSERT/SeqScan/DELETE 的执行链，
明确 Catalog 特殊表课程要求的后续安排。确认设计后再实现；本轮未动 SQL/Executor 或任何新存储功能。
