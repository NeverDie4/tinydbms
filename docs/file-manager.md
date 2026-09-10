# tinydbms FileManager 与 Storage 集成（Phase 1C/1D）

## 1. Scope

`FileManager` 是 storage-private 的表文件所有者，只负责：

```text
TableId → tables/table_<TableId>.dat → PageFile ownership/lifecycle
```

它不缓存 Page，不保存 `PageId → RawPage` 映射，也不包含 Record、Slotted Page、BufferPool、扫描或数据行读写。

## 2. Directory and naming contract

```text
<data_dir>/
  storage.meta
  tables/
    table_0.dat
    table_1.dat
```

`FileManager::open(data_dir)` 创建并验证 `tables/`。文件名只使用稳定的 `TableId`，不使用用户表名。单个 FileManager 独占其已打开 PageFile；FileManager 析构时 best-effort `close_all()`。

## 3. Private API

- `FileManager::open(data_dir)`：建立 manager 并创建/验证 `tables/`。
- `create_table_file(TableId)`：创建并持有新的 PageFile，拒绝覆盖已有文件。
- `open_table_file(TableId)`：打开并持有已有 PageFile；同一 TableId 已打开时返回现有对象。
- `find_table_file(TableId)`：非拥有观察指针，只在 manager 持有期间有效。
- `close_table_file(TableId)`：关闭并释放一个 PageFile；重复关闭成功。
- `close_all()`：关闭并释放全部 PageFile；重复调用成功。
- `remove_table_file(TableId)`：仅供本轮 `create_table` 失败回滚使用，先关闭再删除精确文件。

FileManager 复用 `PageFileErrorKind::{kIo,kCorrupt,kInvalidArgument}`。公共边界继续只暴露既有 `StorageErrorKind`。

## 4. Storage lifecycle

打开链：

```text
open_storage
→ 创建/检查 data_dir
→ 读取 storage.meta
→ 初始化 FileManager 和 tables/
→ 按每个 TableMeta.table_id 打开并验证 PageFile
→ 全部成功后才设置 StorageState.open
```

metadata 声明的 PageFile 缺失、header 损坏或格式不合法均映射为 `kCorrupt`。不会创建空 PageFile 掩盖数据丢失。若中途失败，临时 FileManager 析构并关闭已经打开的文件，StorageState 恢复为空。

关闭链：

```text
close_storage
→ FileManager.close_all()
→ 写入 metadata（当前仍采用 failure-atomic replace）
→ 清空 StorageState
```

## 5. create_table commit order

```text
校验 schema、重复 TableId 和重复表名
→ create_table_file(TableId)
→ 将 TableMeta 暂时加入内存 catalog
→ failure-atomic 写入 storage.meta
→ 成功返回
```

任何 catalog 重复都在文件创建前失败。已有 `table_<TableId>.dat` 而 catalog 没有对应项时，禁止覆盖并返回 `kCorrupt`。

metadata commit 失败时：先从内存 catalog 弹出本轮 TableMeta，再关闭并删除本轮新建 PageFile。删除失败返回 `kIoError`，文件保持原状，不尝试覆盖或扩大恢复范围。本阶段没有 WAL，因此不承诺进程崩溃发生在文件创建和 metadata commit 之间时的自动恢复。

## 6. Metadata and table-file invariant

- 每个已提交 `TableMeta.table_id` 必须存在且只能对应一个合法 `tables/table_<TableId>.dat`。
- `open_storage` 验证 `metadata → table file` 方向的完整性。
- 未被 metadata 引用的 orphan 文件不会被自动删除或覆盖；相同 TableId 的后续 `create_table` 会以 `kCorrupt` 拒绝。
- `create_table` 成功返回时，metadata 和带合法 Page 0 的表文件同时存在。

## 7. Legacy decision

不实现 metadata-only 目录迁移。Phase 0 产生的 metadata 若声明表但没有对应 PageFile，从 Phase 1D 起按 `kCorrupt` 处理。

继续兼容 metadata V1 中旧 `INT` 标签到 `INT32` 的类型解析，但测试夹具必须同时创建合法表 PageFile。这不是磁盘文件版本迁移。

## 8. MiniOB adaptation

MiniOB 的 `BufferPoolManager`/`DiskBufferPool` 展示了“管理器按文件名拥有已打开文件对象、统一 create/open/close”的责任边界。tinydbms 借用该所有权分层，但当前 FileManager 直接拥有独立 PageFile，并明确省略 frame cache、pin/dirty、替换策略、日志和恢复。
