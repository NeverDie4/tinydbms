# tinydbms Record 与 Slotted Page 格式（Phase 2A/2B 冻结）

## 1. Scope and implementation stages

本文冻结 schema-driven Record binary format、Slotted Page layout、Slot 生命周期和 RecordId 私有编码。Phase 2 已完成；跨页放置、扫描、删除和公共 Storage CRUD 已由 Phase 4 HeapTable 实现。后文 Phase 2 范围描述保留为分层边界，不代表整个 V1 的实现状态。

后续实现严格分为：

```text
Phase 2C  RecordCodec
Phase 2D  SlottedPage
Phase 2E  RecordId codec + in-memory RecordPage
Phase 2F  RecordPage + PageFile persistence integration tests
```

Phase 2C 的 storage-private `RecordCodec` 不访问 PageFile；Phase 2D 的 `SlottedPage` 只操作一个 `RawPage`、private PageId/SlotHandle 和 byte payload；Phase 2E 的 `RecordPage` 组合这两层与 RecordId codec，不执行 I/O。Phase 2F 仅由测试 fixture 显式组合 RecordPage 与 PageFile，不实现跨页选择、FileManager 接线或公共 Storage CRUD。

BufferPool、Frame、pin/unpin、dirty、LRU/FIFO、索引、事务、WAL、MVCC 和 schema evolution 均不属于上述阶段。

## 2. Schema and logical Record contract

Record 是由 `TableMeta.columns` 驱动的变长编码。`Record.values[i]` 必须对应 `TableMeta.columns[i]`。

Record payload 不保存 column count、Type tag、schema version、RecordId、NULL bitmap 或 total-length 字段。Slot Entry 保存 payload 长度；解码所需的列顺序和类型来自不可变的 TableMeta。

因此冻结以下前提：

```text
Table schema is immutable after table creation.
ALTER TABLE / schema evolution is out of scope.
```

NULL 当前不支持。未来加入 NULL 或 schema evolution 必须升级格式版本，不能改变 version 1 的解释。

## 3. Logical and physical size limits

逻辑行大小定义为：

```text
logical_row_size =
    sum(fixed-width value bytes)
  + sum(VARCHAR UTF-8 payload byte count)
```

VARCHAR 的 4-byte length prefix 不计入 logical size，只计入 physical encoded size。

```text
logical_row_size <= kMaxRowLogicalBytes       // 4096
encoded_size     <= kMaxRecordPayloadBytes    // 4056
```

`kMaxRecordPayloadBytes` 来自空 Slotted Page 的精确上限：

```text
4096 - 32-byte Page Header - 8-byte first Slot Entry = 4056
```

logical 或 encoded 上限失败均映射到现有公共 `StorageErrorKind::kValueTooLarge`，不增加公共错误类型。

## 4. Record binary format

所有字段按 `TableMeta.columns` 顺序紧密连接，不做额外 alignment padding。所有多字节值显式使用 little-endian，禁止持久化 C++ 对象内存布局。

| Type | Encoding | Bytes |
|---|---|---:|
| `kInt32` | two's-complement bit pattern, little-endian | 4 |
| `kInt64` | two's-complement bit pattern, little-endian | 8 |
| `kFloat` | IEEE-754 binary32 bit pattern, little-endian | 4 |
| `kDouble` | IEEE-754 binary64 bit pattern, little-endian | 8 |
| `kBool` | `0x00` false, `0x01` true | 1 |
| `kVarchar` | `uint32` byte length + UTF-8 bytes | 4 + N |

编码前必须校验字段数、Value/Type 一一对应、UTF-8、单个 VARCHAR 最多 1024 bytes、logical size 和 encoded size。

解码必须按 schema 恰好消费 Slot Entry 指定的全部 payload。short input、trailing bytes、非法 BOOL、非法 UTF-8、VARCHAR length 越界或不能按 TableMeta 完整解释均为 `kCorrupt`。

FLOAT/DOUBLE 实现应通过 `std::bit_cast` 提取位模式，并以编译期断言验证宽度及 IEC 559 支持。

## 5. Slotted Page overview

PageFile Page 0 仍是 File Header。Slotted Record Page 只存在于 Page 1+，整体仍是一个 4096-byte `RawPage`。

```text
offset 0
+--------------------------------------+
| Page Header (32 bytes)               |
+--------------------------------------+
| Slot 0 (8 bytes)                     |
| Slot 1 (8 bytes)                     | grows upward
| ...                                  |
+---------------- free_lower ----------+
| contiguous free space                |
+---------------- free_upper ----------+
| encoded record payloads              | grows downward
+--------------------------------------+
offset 4096
```

Phase 2 的 table file 保持“一张 Table 对应一个 PageFile，PageFile 包含多张 Record Page”。Record Page 不保存 TableId；RecordId 只在其来源 TableId 内有效。

该约定不把“所有 Page 1+ 永远都是 Record Page”写成永久 PageFile 格式。V1 HeapTable 已遍历候选页：free 跳过，allocated 必须是合法 TSP1，其他内容报 corrupt；不能静默跳过 unknown allocated page。

## 6. Page Header: 32 bytes

| Offset | Size | Field | Encoding / meaning |
|---:|---:|---|---|
| 0 | 4 | `magic` | ASCII `TSP1` |
| 4 | 2 | `format_version` | little-endian `uint16`, value 1 |
| 6 | 2 | `header_size` | little-endian `uint16`, value 32 |
| 8 | 4 | `page_id` | little-endian storage-private PageId |
| 12 | 2 | `slot_count` | allocated Slot Entry count |
| 14 | 2 | `live_count` | occupied Slot Entry count |
| 16 | 2 | `free_lower` | first byte after Slot Directory |
| 18 | 2 | `free_upper` | first byte of packed payload area |
| 20 | 12 | reserved | writer emits zero; version 1 reader requires zero |

空页初始化为：

```text
slot_count = 0
live_count = 0
free_lower = 32
free_upper = 4096
```

当 `live_count == 0` 时必须有 `free_upper == 4096`，但 `slot_count` 不清零。

## 7. Slot Entry: 8 bytes

storage-private `SlotId` 为 `uint16_t`，Slot `i` 位于 `32 + i * 8`。

| Entry offset | Size | Field |
|---:|---:|---|
| 0 | 2 | `record_offset` |
| 2 | 2 | `record_length` |
| 4 | 2 | `generation` |
| 6 | 2 | `flags` |

```text
kSlotOccupied = 0x0001
kSlotRetired  = 0x0002
kKnownSlotFlags = kSlotOccupied | kSlotRetired
```

Version 1 只允许以下状态：

| State | OCCUPIED | RETIRED | offset / length |
|---|---:|---:|---|
| live | 1 | 0 | both greater than 0 |
| reusable deleted | 0 | 0 | both 0 |
| retired | 0 | 1 | both 0 |

以下情况均为 `kCorrupt`：未知 flag bit、同时 OCCUPIED/RETIRED、非 live slot 带 offset/length、live slot generation 为 0，或 live payload 越界/重叠。

## 8. Growth and free-space calculation

Slot Directory 向高地址增长，payload 向低地址增长：

```text
free_lower = 32 + slot_count * 8
contiguous_free = free_upper - free_lower
```

复用 reusable deleted slot：

```text
required = encoded_size
```

没有 reusable slot、必须创建新 Slot Entry：

```text
required = encoded_size + 8
```

插入仅在 `contiguous_free >= required` 时成功。Phase 2 采用 eager compaction，因此成功操作后的可回收 payload 空间全部并入 contiguous free space，不维护 fragmented-byte 计数。

## 9. Generation state machine

generation 0 永不签发。严格状态机为：

```text
first creation
  generation = 1
  flags = OCCUPIED

delete live slot with generation < 65535
  flags = 0
  offset = 0
  length = 0
  generation unchanged

reuse reusable deleted slot
  generation = generation + 1
  flags = OCCUPIED

delete live slot with generation == 65535
  flags = RETIRED
  generation = 65535
  offset = 0
  length = 0

RETIRED
  never reused
```

generation 只在下一次复用时增加，不在删除时增加。

## 10. RecordId encoding and scope

公共 `RecordId { uint64_t value; }` 保持 opaque。只有 storage 私有实现允许编码或解析：

```text
bits 63..32  PageId      uint32
bits 31..16  generation  uint16
bits 15..0   SlotId      uint16
```

```text
value = (uint64(PageId) << 32)
      | (uint64(generation) << 16)
      | uint64(SlotId)
```

RecordId 是 table-scoped handle，不是跨表全局 ID。`DeleteRequest.table_id` 先选择独立 PageFile，再在该文件内解释 RecordId。校验至少包括 PageId 非 0、generation 非 0、SlotId 小于 slot_count、slot 为 live，且 generation 完全匹配。

## 11. Delete and eager compaction

删除流程：

```text
decode and validate RecordId
→ update slot using generation state machine
→ live_count--
→ rebuild all live payloads in a scratch RawPage
→ update every live slot record_offset
→ set free_upper to packed payload start
→ validate final page
```

推荐按 SlotId 升序重建 payload。整个页只有 4096 bytes，使用一个 scratch RawPage 比原地移动更简单且可避免覆盖错误。compaction 不改变任何 live SlotId 或 generation。

删除最后一条记录后：

```text
slot_count > 0
live_count = 0
free_lower = 32 + slot_count * 8
free_upper = 4096
```

Slot Directory 不回收。高频 churn 可能造成页内元数据膨胀；这是 Phase 2 为保证 RID 安全和实现简单性接受的空间退化，不属于本阶段优化范围。

## 12. Empty Record Page and PageFile free list

普通 Record Page 即使 `live_count == 0`，Phase 2 也不调用 `PageFile::free_page`，而是保留并优先用于后续插入。否则 PageId 被 PageFile 复用并重新初始化 generation 后，历史 RecordId 可能再次命中新记录。

PageFile free list 格式保持冻结，但 Phase 2 的 heap Record Page 不参与该 free list。未来若要回收空 Record Page，必须先设计 page incarnation 或等价的 stale-RID 防护。

## 13. Page validation invariants

读取或修改前必须验证：

- magic、format version、header size 和 reserved bytes；
- header PageId 等于实际 PageId；
- `live_count <= slot_count`；
- `slot_count <= floor((4096 - 32) / 8)`；
- `free_lower == 32 + slot_count * 8`；
- `32 <= free_lower <= free_upper <= 4096`；
- `live_count == 0` 时 `free_upper == 4096`；
- flags 只包含 known bits，且状态组合合法；
- live slot generation 非 0，offset/length 非 0；
- live payload 完全位于 `[free_upper, 4096)` 且互不重叠；
- occupied slot 数量等于 live_count；
- payload area compact 后无未计入空洞；
- 被读取的 payload 能由当前不可变 TableMeta 精确解码（由 RecordPage 调用 RecordCodec 检查，不是 SlottedPage 的职责）。

磁盘格式不变量失败映射为公共 `kCorrupt`。API 输入字段数/类型错误映射为 `kInvalidRequest`；logical/physical size 超限映射为 `kValueTooLarge`。

## 14. MiniOB adaptation

MiniOB 默认 ROW Record Page 采用 PageHeader、allocation bitmap 和定长对齐记录，RID 为 `(page_num, slot_num)`。tinydbms 借鉴 Page/Slot 定位和页级职责，但因真实 VARCHAR 为变长 UTF-8，改用 offset/length Slot Directory，并增加 generation 解决 slot reuse 的 ABA 风险。

tinydbms 不复制 MiniOB 的定长记录内存布局，也不在 Phase 2A/2B 引入 BufferPool、Frame、日志、事务或索引。

## 15. Required tests for implementation phases

Phase 2C RecordCodec tests：全部 Type round-trip、字段数/类型错误、UTF-8、VARCHAR 1024 边界、logical 4096 边界、encoded 4056 边界、short input、trailing bytes、非法 BOOL、非法 VARCHAR length。

Phase 2D SlottedPage tests：初始化、header/slot 固定布局、双向增长、边界容量、复用 slot 时 generation 增加、删除时 generation 不变、65535 retire、未知/冲突 flags、get stale RID、eager compact、删除最后一条后的 free_upper、slot directory 不回收，以及全部 page corruption invariants。

Phase 2E `record_page_test`：RID 位布局 golden value 与边界、六类型/UTF-8、多记录、错误类型和大小、no-space、失败不修改、wrong-page、deleted/stale/retired handle、损坏 page/payload。

Phase 2F `record_page_file_integration_test`：初始化与六类型落盘恢复、同页多记录、中间删除及 compact、删除后 reopen 再复用、generation 与 RID 跨 reopen 稳定、空页保留 slot_count、两文件隔离、magic/self PageId/flags/generation/BOOL/VARCHAR 损坏、写失败报告、独立 raw page free-list 复用。

## 16. Implemented RecordPage API and persistence boundary

`src/storage/record_page.h` 提供 private `RecordIdCodec::encode/decode` 和 `RecordPage::initialize/insert_record/get_record/erase_record`。
RID packing 只有一处实现；codec 只拒绝 PageId 0 / generation 0，实际 SlotId 范围、live 状态和 generation 匹配由 SlottedPage 校验。
RecordId 不包含 TableId，调用者必须先选择正确表文件；不同表可以存在相同 RID.value。

insert 先 encode，再在 scratch RawPage 插入并生成 RID，全部成功后才发布页修改；失败不返回 RID。get 返回拥有自身 values 的 Record，record_id 原样保留，不修改页。erase 委托 SlottedPage 的失败不修改和 compact/generation 状态机。

private 错误为 invalid argument、value too large、no space、corrupt；不改变公共 StorageErrorKind。合法页中的 stale generation 是 invalid argument，不是 corrupt；页内 generation 0 或非法 slot 状态才是 corrupt。没有 checksum，不能识别所有仍符合格式的不当字节改写。

持久化链由测试 fixture 显式执行：

```text
PageFile read → RecordPage insert/erase → PageFile write
→ close → open → read → RecordPage get → RecordCodec decode
```

RawPage 修改成功不等于写盘成功。调用者必须检查 write_page 返回值；失败后内存可能已改变，磁盘状态遵循 PageFile 当前写语义，不承诺 mid-write atomicity、断电一致性或恢复。
写失败测试通过已关闭 PageFile 的确定性拒绝验证错误报告及未写入状态，不模拟设备半写或断电。
空 RecordPage 保留历史 slots/generation，不调用 free_page；free-list 测试仅操作独立 raw page。

## 17. Phase 2 review gate

2E 完成后先补充 RID golden layout、零值解码、retired handle 与 corrupt mutation 原子性测试，再运行 configure/build/CTest（9/9）及 diff check；通过后才添加 2F。
2F 不新增生产 I/O 包装层，不修改 PageFile、SlottedPage、RecordCodec 或公共 storage.h。依赖只在测试 fixture 汇合，未增加 cache、BufferPool、跨页 placement 或 Storage CRUD。
