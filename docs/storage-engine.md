# MiniOB 存储系统

本文以默认 `HEAP + ROW_FORMAT` 路径为主，再标出 PAX、LSM、索引、事务和恢复的分叉。

## 1. 磁盘文件布局

路径拼接集中在 `src/observer/storage/common/meta_util.*`：

```text
<base_dir>/db/<db_name>/
├─ <db_name>.db                         Db checkpoint LSN 文本
├─ <table>.table                        TableMeta JSON
├─ <table>.data                         表数据的分页文件
├─ <table>-<index>.index                 B+ Tree 索引分页文件
├─ <table>.lob                          大对象文件（若使用）
├─ dblwr.db                              数据库级 DoubleWriteBuffer
├─ clog/                                 DiskLogHandler 日志文件
└─ lsm/                                  Db 初始化时打开的 ObLsm 目录
```

一个 DB 目录由 `DefaultHandler::create_db/open_db` 管理。`Db` 当前直接使用 `dbpath` 作为表文件目录，而不是再加一层同名目录。

## 2. Page、Page ID、页头

### 2.1 Page

`src/observer/storage/buffer/page.h` 定义：

```cpp
struct Page {
  LSN lsn;
  CheckSum check_sum;
  char data[BP_PAGE_DATA_SIZE];
};
```

`BP_PAGE_SIZE = 1 << 13`，即 8192 字节；`BP_PAGE_DATA_SIZE` 是减去 `LSN` 和 `CheckSum` 后的空间。磁盘文件、内存 Frame、读写都以完整 Page 为单位。

`PageNum` 是 `int32_t`，page 0 是 `BP_HEADER_PAGE`，通常只保存该分页文件的 Buffer Pool 元数据。数据文件的记录页从 page 1 开始。

### 2.2 Buffer Pool 文件头

`disk_buffer_pool.h::BPFileHeader` 位于 page 0 的 `Page::data` 开头：

```text
int32 buffer_pool_id
int32 page_count
int32 allocated_pages
char  bitmap[0]
```

bitmap 的第 0 位永远表示头页已分配。`BufferPoolIterator::init(bp, 1)` 用这个 bitmap 遍历已分配的数据页，所以 Table 不需要在 TableMeta 中保存“所有数据页列表”；数据页集合由 `.data` 文件的 BPFileHeader 反映。

### 2.3 Frame 和 Page ID

`FrameId` 是 `(buffer_pool_id, page_num)`。`Frame` 持有：

- `Page page_`：页数据；
- `FrameId frame_id_`；
- `dirty_`：是否修改；
- `pin_count_`：是否仍被使用；
- `acc_time_`：LRU 访问时间；
- 读写 latch 和调试锁。

因此 PageNum 只在一个分页文件内有意义；跨表/索引定位必须带 `buffer_pool_id`，而 `RID` 只记录数据表内的 `(page_num, slot_num)`。

## 3. Page 从磁盘进入内存

以读取已存在页为例：

```text
RecordPageHandler::init
  -> DiskBufferPool::get_this_page(page_num, &frame)
  -> BPFrameManager::get(id, page_num)
     命中：pin + access，直接返回
  -> 未命中：DiskBufferPool::allocate_frame
     -> BPFrameManager::alloc
     -> 若无可用 Frame，purge_frames(1, purger)
  -> DiskBufferPool::load_page
     -> DiskDoubleWriteBuffer::read_page 尝试读二次缓冲
     -> 未命中则 lseek(page_num * BP_PAGE_SIZE)
     -> readn(file_desc, &page, BP_PAGE_SIZE)
```

`BPFrameManager::get_internal` 命中时会 `frame->pin()`；新 Frame 由 `alloc` 先 pin。调用者在结束使用页时必须通过 `DiskBufferPool::unpin_page` 释放 pin，并在 `RecordPageHandler::cleanup` 中释放 latch。

## 4. Buffer Pool 管理、淘汰、dirty 和 flush

### 4.1 BufferPoolManager 和 DiskBufferPool

`Db` 为每个数据库创建一个 `BufferPoolManager`。它维护：

- 文件名到 `DiskBufferPool *` 的映射；
- buffer pool id 到 `DiskBufferPool *` 的映射；
- 一个共享 `BPFrameManager`；
- 一个 `DoubleWriteBuffer`。

每个表数据文件、索引文件分别由一个 `DiskBufferPool` 打开；同一个 Frame Manager 管理它们在内存中的 Frame。

### 4.2 脏页

所有修改 Page 内容的操作都应调用 `Frame::mark_dirty()`。当前代码中的典型位置：

- `RowRecordPageHandler::insert_record`：复制记录并标脏；
- `RowRecordPageHandler::delete_record`：清 bitmap 并标脏；
- `RecordPageHandler::init_empty_page`：初始化页头/bitmap 并写日志；
- B+ Tree 节点修改后；
- BPFileHeader 分配位图改变后。

`Frame::clear_dirty()` 只在 `DiskBufferPool::flush_page_internal` 把页加入 double-write buffer 后调用。

### 4.3 什么时候 flush

有三类时机：

1. `Db::sync`：逐表 `Table::sync`；Heap 引擎同步索引并调用 `data_buffer_pool_->flush_all_pages()`，然后刷新 double-write buffer，等待日志 LSN，再写 DB checkpoint。
2. Buffer Pool 淘汰页：`DiskBufferPool::allocate_frame` 找不到 Frame 时调用 `BPFrameManager::purge_frames`；purger 先 flush dirty frame，再释放 Frame。
3. 关闭表/数据库：`DiskBufferPool::close_file` 调 `purge_all_pages`；double-write buffer 的析构和 `Db::sync` 会把二次缓冲中的页写到真实文件。

`DiskBufferPool::flush_page_internal` 的当前代码顺序是：先让 `BufferPoolLogHandler::flush_page` 处理日志，再计算 CRC，调用 `DoubleWriteBuffer::add_page`，最后清 dirty。`DiskDoubleWriteBuffer::add_page` 先把完整页写到 `dblwr.db`；缓存达到 max pages 时 `flush_page` 再通过 `DiskBufferPool::write_page` 写真实文件，并把二次缓冲条目标为 invalid。

### 4.4 淘汰和 pin/unpin

`Frame` 具有 pin count；只有 `pin_count == 0` 的页可淘汰。`BPFrameManager::purge_frames` 从 `LruCache::foreach_reverse` 中挑选可淘汰页，临时增加 pin 防止并发释放，flush 后 `free_internal` 从缓存和内存池移除。

所以 MiniOB 不是“访问一次就复制一次页数据”的缓冲池，而是：

```text
get_this_page -> pin + latch -> 直接在 Frame/Page 上操作
cleanup       -> unlatch + unpin
淘汰          -> 若 dirty 则 flush，再 free Frame
```

## 5. Record 如何序列化和放入 Page

### 5.1 Record 的内存形式

`src/observer/storage/record/record.h::Record` 由：

- `RID rid_`；
- `char *data_`；
- `int len_`；
- `bool owner_`；
- LSM 使用的 `key_`；

组成。它没有额外的“记录长度头”，长度来自 `TableMeta::record_size()` 和页头的 `record_real_size`。`Table::make_record` 按 `FieldMeta::offset/len` 把 `Value` 写到一段定长内存，再交给 `Record::set_data_owner`。

默认 ROW 格式没有独立的序列化函数：`RowRecordPageHandler::insert_record` 直接 `memcpy(record_data, data, record_real_size)`。从页读行时，`RowRecordPageHandler::get_record` 让 Record 指向页内地址；`RecordFileHandler::get_record` 如果需要跨越页生命周期，则再 `copy_data`。

### 5.2 行存页面结构

`RecordPageHandler` 的页面布局是：

```text
| PageHeader | record allocation bitmap | aligned record 0 | ... | record N |
```

`PageHeader`：

```text
record_num
column_num
record_real_size
record_size              // 对齐后的槽大小
record_capacity
col_idx_offset
data_offset
```

`RecordPageHandler::init_empty_page` 计算：8 字节对齐的槽大小、bitmap 大小、容量、数据起点，并把 bitmap 清零。`RowRecordPageHandler::insert_record` 找 bitmap 的第一个 unset bit，把它设为 1，增加 `record_num`，写 Record log，复制数据，返回 `RID(page_num, slot_num)`。

### 5.3 空闲空间

`RecordFileHandler` 在内存中维护 `unordered_set<PageNum> free_pages_`。初始化时 `init_free_pages` 遍历 page 1 以后的所有已分配页，用 `RecordPageHandler::is_full()` 找未满页。

插入时先拿 `free_pages_` 中的页；如果集合为空或候选页实际已满，则 `DiskBufferPool::allocate_page` 扩展文件或复用 bitmap 中的空闲页，随后 `init_empty_page`，再把新页放回 free_pages_。删除成功后把 RID 所在页重新放回 free_pages_。

这个集合是运行时加速结构，不是持久化目录；重启时通过遍历页重建。

## 6. Table、Record、Page、Buffer、File 的调用关系

```text
Table
  -> TableEngine
    -> HeapTableEngine
      -> RecordFileHandler
        -> RecordPageHandler
          -> DiskBufferPool
            -> BPFrameManager / Frame
              -> Page
            -> read/write(file_desc)
```

`Table` 只负责表级 API 和 `TableMeta`；`HeapTableEngine` 把表 API 接到具体数据文件和索引；`RecordFileHandler` 负责跨页组织；`RecordPageHandler` 负责单页布局；`DiskBufferPool` 负责页缓存/磁盘交互。

## 7. 一条 INSERT 到磁盘的完整链

下面是默认单语句 `INSERT INTO student VALUES (...)` 的逻辑顺序：

```text
InsertPhysicalOperator::open(trx)
  -> Table::make_record
     -> TableMeta::record_size
     -> FieldMeta::offset/len
     -> Record::set_data_owner
  -> VacuousTrx::insert_record
     或 MvccTrx::insert_record（先写 begin/end xid）
  -> Table::insert_record
  -> HeapTableEngine::insert_record
  -> RecordFileHandler::insert_record(data, record_size, &rid)
     -> free_pages_ 找一个未满页
     -> RecordPageHandler::init(... READ_WRITE)
        -> DiskBufferPool::get_this_page
        -> FrameManager::get/alloc
        -> load_page（未命中时从 .data 读取）
     -> RowRecordPageHandler::insert_record
        -> bitmap.set_bit(slot)
        -> page_header_->record_num++
        -> RecordLogHandler::insert_record
        -> memcpy 到 Page.data 的槽
        -> Frame::mark_dirty
        -> 返回 RID
  -> HeapTableEngine::insert_entry_of_indexes
     -> BplusTreeIndex::insert_entry（若有索引）
     -> BplusTreeHandler::insert_entry
     -> 节点页修改并标 dirty
  -> SqlResult::close
     -> PhysicalOperator::close
     -> VacuousTrx::commit 或 MvccTrx::commit
  -> Db::sync / 淘汰 / 关闭时
     -> DiskBufferPool::flush_page_internal
     -> DoubleWriteBuffer::add_page
     -> DiskDoubleWriteBuffer::flush_page
     -> DiskBufferPool::write_page
     -> lseek(page_num * sizeof(Page)) + writen
```

“INSERT 成功”不等于“这条记录已经立即写入 `.data`”。成功后通常只在 Frame 中 dirty；真正的物理落盘由 sync、淘汰、关闭或 double-write flush 触发。日志可能比页面更早提交，以满足 WAL 方向。

## 8. 一条 SELECT 从磁盘读取的完整链

```text
TableScanPhysicalOperator::open
  -> Table::get_record_scanner
  -> HeapTableEngine::get_record_scanner
  -> new HeapRecordScanner(...)
  -> HeapRecordScanner::open_scan
     -> BufferPoolIterator::init(data_buffer_pool, start_page=1)

TableScanPhysicalOperator::next
  -> HeapRecordScanner::next
  -> HeapRecordScanner::fetch_next_record
  -> BufferPoolIterator::next
  -> RecordPageHandler::init(page_num)
  -> DiskBufferPool::get_this_page
     -> 命中 Frame：pin/access
     或 -> allocate_frame -> load_page
        -> DoubleWriteBuffer::read_page
        或 -> lseek + readn 读取 .data 中的 8192 字节 Page
  -> RecordPageIterator::init(bitmap)
  -> RecordPageIterator::next
  -> RowRecordPageHandler::get_record
     -> bitmap 检查 slot
     -> Record::set_rid
     -> Record::set_data(页内槽地址)
  -> HeapRecordScanner::fetch_next_record_in_page
     -> trx->visit_record（MVCC 时判断可见性）
  -> TableScanPhysicalOperator::filter
     -> Expression::get_value(RowTuple, Value)
  -> current_tuple
  -> ProjectPhysicalOperator::current_tuple
  -> communicator 写结果
```

扫描一个页面完毕后，`RecordPageHandler::cleanup` 解锁并 unpin；扫描完所有页面后 `HeapRecordScanner::close_scan` 清理 handler。缓存命中时不发生文件读取；淘汰后下次才从文件或 double-write buffer 读取。

## 9. B+ Tree 索引在存储链中的位置

`BplusTreeIndex` 是 `Index` 接口的单字段实现。索引键不是单独的用户值，而是：

```text
user field bytes + RID(page_num, slot_num)
```

这样同一个字段值的多条记录仍然能在 B+ Tree 中保持唯一排序键。`IndexFileHeader` 保存在索引文件头页，记录 root page、内部/叶子最大容量、属性长度和 key 长度。

节点页：

- `IndexNode`：`is_leaf`、`key_num`、`parent`；
- `LeafIndexNode`：额外的 `next_brother`，保存有序 key+RID；
- `InternalIndexNode`：保存 key+child page id；第 0 个 key 被忽略，指针作为第一子树。

查询走 `IndexScanPhysicalOperator`，它创建 `BplusTreeIndexScanner`，再由 `BplusTreeScanner` 从范围边界遍历叶链返回 RID；之后仍需用 Table/Record 层读取真实行。

## 10. 事务、CLog、DoubleWrite 的分工

### 事务

- `TrxKit` 工厂根据启动参数生成 `VacuousTrxKit`、`MvccTrxKit` 或 `LsmMvccTrxKit`。
- `Session::current_trx` 按当前 Db 懒创建事务。
- `SqlResult::close` 在单语句模式提交并销毁事务；多语句模式等待显式 commit/rollback。
- Vacuous 不做可见性和锁逻辑；MVCC 用隐藏字段和 `visit_record` 判断可见性。

### CLog / 恢复

`Db::recover` 创建事务日志回放器，构造 `IntegratedLogReplayer`，调用 `LogHandler::replay`。`IntegratedLogReplayer::replay` 按 `LogModule::Id` 分发给 buffer pool、record manager、B+ Tree、transaction replayer；`on_done` 依次完成各模块收尾。

`DiskLogHandler` 有后台线程把 `LogEntryBuffer` 刷到 `clog` 文件；`Db::sync` 等待当前 LSN 刷盘后再写 `.db` checkpoint。

### DoubleWrite

DoubleWrite 解决“Page 部分写入导致 checksum 不匹配”的问题：脏页先写 `dblwr.db`，再写真实分页文件。启动时 `DiskDoubleWriteBuffer::load_pages` 读仍有效且 checksum 正确的页，`recover()` 调 `flush_page()` 将其写回真实文件。

## 11. 其它存储分支

- PAX：`StorageFormat::PAX_FORMAT` 仍由 Heap 引擎使用，但 `PaxRecordPageHandler` 的插入、读取、chunk 当前源码中包含 `your code here`/`UNIMPLEMENTED`，默认 ROW_FORMAT 才是完整学习路径。
- LSM：`LsmTableEngine`、`LsmRecordScanner`、`src/oblsm` 是另一条存储路径，数据不按默认 RecordPageHandler 的 heap 页面组织；启动 `Db` 时也会打开 `ObLsm`。要学习 B+ Tree/页式数据库，先不要把 LSM 作为主线。
