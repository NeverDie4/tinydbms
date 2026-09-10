# BufferPool Phase 3B–3E

## 范围

缓存实现位于 `src/storage/buffer_pool.h/.cpp`，3E 在 storage.cpp 的私有 StorageState 接入生命周期；3B–3E 实现 guard/pin、dirty/flush、FIFO/LRU、failure-safe eviction、统计和日志。BufferPool 本身不实现页面分配包装、CRUD 或 table scan；这些上层能力已由 Phase 4 实现。

借鉴 MiniOB `FrameId(buffer_pool_id, page_num)` 的跨文件身份和 Frame pin 思路；tinydbms 使用已有 TableId 文件映射，不引入 MiniOB 的锁、日志、替换策略或恢复系统。

## 身份与状态

- PageKey = TableId + PageId；TableId 0 合法，普通 fetch 拒绝 PageId 0。
- PageKey equality 比较两个字段，hash 对两个 uint32 拼成的 uint64 计算；hash 仅供进程内查找，不持久化。
- FrameId = size_t，是固定数组索引，不是 PageId。
- Frame 保存 RawPage、optional PageKey、dirty（加载初始为 false）、uint32 pin_count。
- BufferPool owns Frames，borrows FileManager；容量创建后不变。
- PageTable 和 occupied Frame.key 一一对应；read 失败不会发布 Frame/mapping。

## API 与生命周期

`BufferPool::create(files, capacity, optional_read_adapter, optional_write_adapter, policy, optional_log_sink)`、`fetch_page(key)`、`flush_page(key)`、`flush_all()`、`release_table(table_id)`、`close()`、`stats()`。为兼容现有调用，policy 默认 FIFO、日志默认关闭，旧 create 调用保持有效。策略只能创建时选择，不支持运行时切换。

`PageGuard::page()` 返回 const RawPage&；`mutable_page()` 返回 RawPage&，但不自动标脏。RecordPage mutation 成功后由调用者显式 `mark_dirty()`。`valid()`、`release()` 和只读 `pin_count()` 可检查 guard/pin 状态。不可复制，可 noexcept 移动；move assignment 先释放目标已有 pin。release 幂等，析构只 unpin、不清 dirty、不执行 I/O。失效 guard 的 page/mutable_page/mark_dirty/pin_count 操作抛 logic_error，不能继续使用旧引用。

关闭池时先检查全部 pin；仍有 guard 则返回 private invalid argument，且不开始 flush。无 pin 后调用 flush_all，失败保留 open 和全部 Frame/PageTable；全部成功后才清空缓存并标 closed。重复关闭成功，累计 stats 保留。closed 池的 fetch/flush_page/flush_all/release_table 返回 invalid argument。

所有 guard 必须先于 BufferPool 销毁，FileManager 必须比池活得久。违反 guard 生命周期或内部 unpin 不变量会 terminate，而非静默产生悬空引用。析构不执行 I/O，不能替代显式成功的 close；销毁尚未成功写回的池会丢失其内存修改，失败后应保留池和文件处理错误。

池存续期间不得绕过它改写、free、关闭再重开已缓存页面/文件。3C 可先成功 release_table，再由调用者关闭对应文件；release_table 本身不关闭文件。不存在长期 PageFile*；fetch/write 仅临时 find_table_file。该检查不是跨 close/reopen identity generation 防护。

## HIT/MISS 与提交

基础检查（池打开、PageId 非零、文件已打开、计数未耗尽）后进入 PageTable lookup。

- HIT：计 hit，检查 pin 溢出，增加 pin，返回 guard，不调用 read。
- MISS：计 miss，优先寻找 empty Frame；没有 empty 才按所选策略选择最老的 unpinned Frame，全部 pinned 则 kNoVictim 且不执行 I/O。
- 有 empty：PageFile read 到临时 RawPage，成功后提交映射、Frame 与 FIFO，pin=1，不计 eviction。
- 单线程且禁止适配器重入，中间提交状态不可被外部观察。标准内存分配异常可传播，但不留下半提交 Frame。

## 错误与统计

private errors：kInvalidArgument、kNoVictim、kIo、kCorrupt。PageFile 三类错误按含义直接映射，不新增公共 StorageErrorKind。invalid/free/out-of-range page 不缓存。

fetch_count = hit_count + miss_count；基础检查失败不计数，read failure/no empty 仍计 miss。hit_rate 在零 fetch 时为 0，否则使用浮点除法。pin/count 都在递增前检查溢出，unpin 在递减前检查零值。

## 测试与 I/O 适配口

`tests/buffer_pool_test.cpp` 使用真实临时表文件测试正常路径和非法页；optional read(PageKey) 回调用于统计 read 与注入失败。3C 增加 optional write(PageKey, const RawPage&) 用于精确写计数与第 N 次写失败；均不保存 PageFile*、不缓存数据，不得重入池或修改其状态。

验证身份隔离、多 PageId、HIT 不读盘、多个 guard、移动与释放、pin overflow/underflow、全部 pinned 满池拒绝、失败不改变旧缓存、不占空槽、统计口径和基础 close。3D 将旧阶段的“未 pin 满池仍拒绝”断言更新为 FIFO 替换；不引入旧策略开关。所有 Phase 0–2 测试继续回归。

## Phase 3C flush / release 契约

- flush_page：open 池的 non-resident key 为 success no-op，不加载；clean Frame 不写。
- dirty Frame 可在 pinned 状态写回，flush 不改变 pin。成功才 dirty=false 且 dirty_flush_count++。
- 失败时 Frame.page/key、pin、PageTable 不变，dirty 保持 true，计数不增加。
- flush_all 按 FrameId 升序处理，遇错停止；成功前缀变 clean，失败与未处理页保持状态，不承诺整体原子性。
- release_table 先检查该表全部 pin；任一非零则无写回、无移除。随后按 FrameId 升序 flush；全部成功后才删除该表全部映射并 reset Frames。中途失败时所有该表页面仍驻留，已成功写回页可以保持 clean。
- release_table 不影响其他表，不关闭 PageFile；成功产生的 empty Frames 可被后续 MISS 使用，这不是 eviction。
- dirty_flush_count 只统计成功的 dirty write，涵盖四个 flush/close 入口；不改变已有 fetch/hit/miss 口径。计数耗尽在写入前返回 invalid argument，避免回绕。
- 物理写入继承 PageFile 的成功/失败语义，不提供 WAL、断电恢复或磁盘半写原子性保证。

`buffer_pool_flush_test.cpp` 使用受控只读 friend 检查 Frame/PageTable 双向一致，不向生产 API 暴露 Frame。覆盖 partial flush/release/close failure、重试、invalid guard、未标脏不写、计数及真实 RecordPage→flush/close→PageFile reopen 恢复。

## Phase 3D FIFO 与 failure-safe eviction

FifoReplacer 仅保存 FrameId 顺序，容量在创建时 reserve。choose_victim 接受由 BufferPool 提供的 eligibility predicate，不保存 pin/dirty/key，不执行 I/O，不移除候选。
成功 load/replace 加入队尾；HIT、pin/unpin、flush 不重排。被 pin 的老页只暂时跳过，解 pin 后仍保留原年龄。

MISS 顺序严格为：empty 优先，否则选 victim → target READ 到 scratch → dirty victim WRITE → COMMIT。
target read 失败不写 victim；dirty write 失败保留原页、dirty、pin、映射和 FIFO，丢弃 scratch。两类失败均不增加 eviction_count。

为避免写回后 map 分配异常造成半提交，在 I/O 提交前使用临时 map 准备 node handle；生产 PageTable 预留 capacity+1 桶容量。write 成功后插入同 allocator 节点、删除旧映射、替换 Frame、更新 FIFO，无需动态分配。FIFO vector 也预留容量。适配器不得重入，因此不能观察中间提交状态。
dirty_flush_count 在成功 dirty WRITE 时增加；eviction_count 仅在 replacement 完整 commit 后增加。失败磁盘写仍不承诺半写原子性。

release_table 只有在全部目标 flush 成功后才移除对应 FIFO 条目；prepare/flush 失败保留 FIFO。close 成功清空 FIFO，失败保留。resident Frame、PageTable 和 FIFO 一一对应；empty Frame 在两者中都没有条目。

`buffer_pool_fifo_test.cpp` 覆盖 FIFO 基本顺序、HIT 不重排、跳过 pinned 后恢复老年龄、no-victim 零 I/O、clean/dirty victim、精确 READ→WRITE 顺序、第 N 次读写失败与重试、成功计数、真实写回、release/close 生命周期和三方不变量。

## Phase 3E LRU、日志与生命周期接入

LRU 复用已有 FrameId 顺序容器和唯一 fetch/eviction 流程：成功 HIT 在 pin 成功后移动到队尾；成功 MISS/replace 在 commit 后进入队尾。FIFO 的 HIT 不重排。unpin、mark_dirty、flush 均不算 access。跳过 pinned Frame 不改变其年龄。实现无需第二套 eviction、虚基类或工厂。

Stats 保持 fetch_count、hit_count、miss_count、eviction_count、dirty_flush_count 和原 hit_rate 口径。
可选 LogSink 接收轻量文本事件 Buffer HIT、Buffer MISS、Evict、Flush dirty，包含 table/page、相关 frame、policy 与写回 result，不含 payload。空 sink 不构建/输出日志；日志异常被隔离，不影响操作结果。sink 不得重入或修改 BufferPool，string_view 只能在回调期间使用。

StorageState 先拥有 FileManager，后拥有 BufferPool；销毁及 reset 显式先销毁池。生产默认 64 Frames、FIFO、日志关闭，不扩公共配置 API。storage_test_access.h 仅供私有测试访问池/文件、配置小容量与注入失败，core 不使用。

open_storage：metadata → FileManager → 验证所有表文件 → BufferPool → Open。池创建失败时关闭已打开文件并重置状态；若本次新建了空 tables/，只尝试非递归删除该空目录，使失败后同路径可以重试，不删除既有目录或文件。

close_storage：Open → Closing → BufferPool.close → FileManager.close_all → metadata → Closed。
池关闭失败（包括活跃 guard 或写失败）恢复 Open，文件保持打开。池成功关闭后，文件关闭/metadata 失败则保持 Closing；普通操作和重新 open 拒绝，下一次 close 重试文件/metadata 收尾，不错误恢复 Open。重复 Closed close 成功。

缓存一致性：已有 RecordPage 禁止绕过 guard 直接 PageFile.write_page/free_page。HeapTable 新页采用 allocate → scratch initialize → bootstrap write → fetch；这是尚未驻留、尚未签发 RID 的唯一写旁路。raw PageFile 测试可 allocate → fetch。关闭或重开某表前，必须先成功 release_table；整体 Storage close 则先关闭整个池。普通 RecordPage 仍不 free。

storage_buffer_pool_test.cpp 验证 pool 创建重试、关闭失败状态及持久化。FIFO/LRU 共用故障测试；公共 CRUD 已通过 storage_crud_test 与 storage_v1_stress_test 验证。StorageState 当前还拥有 CursorRegistry：先销毁 registry，再池，再文件；正常关闭先完成池/文件/metadata，再清 registry，不重置 CursorId。
