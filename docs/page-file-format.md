# tinydbms PageFile Disk Format（Phase 1A 冻结）

## 1. Scope

本文冻结独立 `PageFile` 的 4096-byte raw page 磁盘格式与正常关闭后的持久化语义。实现位于 storage 私有目录，不属于 `tinydbms/storage.h` 公共 API。

本文只定义 PageFile 层。FileManager、Record/Slotted Page、BufferPool 与公共 CRUD 已在后续阶段实现；索引、事务、WAL 或崩溃恢复仍不属于 V1。

## 2. Basic types and byte order

- `PageId`：storage-private `std::uint32_t`。
- `RawPage`：恰好 4096 bytes。
- 所有多字节整数采用显式 little-endian 编码；不得将 C++ struct 的对象内存直接写盘。
- `PageId 0` 固定为 file header，第一张可分配 raw page 为 `PageId 1`。
- header 内 `free_page_head == 0` 表示 free list 为空；该私有约定不影响公共 ID 契约。

页偏移固定为：

```text
offset(PageId) = uint64(PageId) * 4096
```

实现必须在转换为 stream offset 前检查可表示性。

## 3. File layout

```text
Page 0              File Header
Page 1..page_count-1 Raw allocated pages or Free Pages
page_count..EOF      optional page-aligned preallocated tail
```

文件长度必须至少为 4096 bytes 且为 4096 的整数倍。`page_count` 是包含 Page 0 的逻辑页数。允许物理文件具有 page-aligned tail，但 `page_count * 4096` 不得超过物理文件长度。

## 4. File Header (Page 0)

| Offset | Size | Field | Encoding / meaning |
|---:|---:|---|---|
| 0 | 8 | `magic` | ASCII `TDBPAGE1` |
| 8 | 4 | `format_version` | little-endian `uint32`, current value `1` |
| 12 | 4 | `free_page_head` | little-endian `PageId`; `0` means none |
| 16 | 8 | `page_count` | little-endian `uint64`, includes Page 0 |
| 24 | 4072 | reserved | writer emits zero; reader currently ignores |

打开已有文件时必须验证 magic、version、文件对齐、`page_count` 范围、逻辑长度，以及完整 free list。格式错误返回内部 `kCorrupt`。

Page 0 只能通过 PageFile 的专用 header 路径更新；普通 `read_page`、`write_page` 和 `free_page` 均拒绝 Page 0。

## 5. Allocated raw pages

成功分配的新页或复用页均返回全零的 4096-byte `RawPage`。普通读写必须恰好传输一整页。short read 是格式截断，归类为内部 `kCorrupt`；底层 seek/read/write/flush 失败归类为内部 `kIo`。

`write_page` 成功表示整页已写入并完成 C++ stream flush。`close` 成功后，正常 reopen 必须读取到数据；本阶段不承诺 fsync、断电一致性或 crash recovery。

## 6. Persistent free list

free list 是单向 LIFO 链。`FileHeader.free_page_head` 指向首个 Free Page。

Free Page 布局：

| Offset | Size | Field | Encoding / meaning |
|---:|---:|---|---|
| 0 | 4 | `free_magic` | ASCII `TFR1` |
| 4 | 4 | `next_free_page` | little-endian `PageId`; `0` means end |
| 8 | 4088 | reserved | writer emits zero; reader currently ignores |

释放中间页不截断文件。`free_page` 将该页写成 Free Page 并挂到链首；重开后链仍有效。double free、释放 Page 0、越界 PageId，以及普通读写 free page，均返回内部 `kInvalidArgument`。

`allocate_page` 先验证完整 free list，再取 head：先更新 header 的链首，再将分配页清零；两步失败会阻止继续使用不确定状态（poison）。free list 为空时在逻辑末尾写零页，再更新 `page_count`。这不是 WAL/断电原子协议。当 `PageId` 值域耗尽时拒绝继续分配。

## 7. Lifecycle and errors

`PageFile::create(path)` 只创建不存在的文件；不会创建父目录。`PageFile::open(path)` 只打开已有文件。`close()` 幂等；关闭后其他操作无效。

内部错误固定为：

- `kIo`：filesystem、open、seek、read/write、flush/close 等环境错误；
- `kCorrupt`：磁盘格式、长度、header 或 free list 损坏；
- `kInvalidArgument`：调用顺序或 PageId 不合法、double free、目标已存在等。

这些是 storage-private 错误，集成层按含义映射为现有公共 `StorageErrorKind::kIoError`、`kCorrupt`、`kInvalidRequest`，不扩展公共 Page 专用错误。

## 8. Ownership boundary

一个 `PageFile` 独占一个打开的文件 stream，禁止复制和移动；析构时执行 best-effort close。它不缓存 page，也不管理多个 table 文件。FileManager 拥有多个 PageFile；BufferPool 借用 FileManager 且只在调用中临时查找文件。见 [`file-manager.md`](file-manager.md)。
