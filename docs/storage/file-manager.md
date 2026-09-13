# tinydbms FileManager 与 Storage 集成（V2 System Catalog）

## 1. Scope

`FileManager` 是 storage-private 的表文件所有者，只负责：

```text
TableId → tables/table_<TableId>.dat → PageFile ownership/lifecycle
```

它不缓存 Page，不保存 `PageId → RawPage` 映射，也不包含 Record、Slotted Page、BufferPool、扫描或数据行读写。

## 2. Directory and naming contract

```text
<data_dir>/
  storage.meta                  # TINYDBMS_STORAGE_BOOTSTRAP_V2，只作 bootstrap
  tables/
    table_0.dat                 # tdb_sys_tables
    table_1.dat                 # tdb_sys_columns
    table_<user-id>.dat         # user-id >= 2
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
→ 读取/验证 storage.meta V2 bootstrap
→ 初始化 FileManager 和 tables/
→ 创建或打开并验证 system PageFile 0/1
→ 扫描 system catalog，重建用户 TableMeta
→ 按已提交的用户 TableMeta 打开并验证 PageFile
→ 拒绝 catalog 未引用的 canonical table_<id>.dat
→ 全部成功后才设置 StorageState.open
```

bootstrap、system catalog、catalog 声明的 PageFile 缺失、header 损坏或格式不合法均映射为 `kCorrupt`。不会创建空 PageFile 掩盖数据丢失，也不会删除或猜测修复不一致目录。若中途失败，临时 FileManager 析构并关闭已经打开的文件，StorageState 恢复为空。

关闭链：

```text
close_storage
→ FileManager.close_all()
→ 清空 StorageState
```

## 5. create_table commit order

```text
校验 schema、重复 TableId 和重复表名
→ create_table_file(TableId)
→ 向 tdb_sys_columns 写入本次所有列记录
→ 向 tdb_sys_tables 写入 table row（catalog commit marker）
→ flush marker 所在页
→ 发布 TableMeta 到内存 catalog
→ 成功返回
```

任何 catalog 重复都在文件创建前失败。已有 `table_<TableId>.dat` 而 catalog 没有对应项时，禁止覆盖并返回 `kCorrupt`。对正常 API 返回失败，Storage 删除本次写入的列记录并删除新用户文件；marker flush 或补偿失败不伪装成功，后续 open 以 fail-closed 方式拒绝不一致状态。

本阶段没有 WAL，因此不承诺进程崩溃发生在列记录、marker 与用户文件更新之间时的自动恢复。

## 6. Metadata and table-file invariant

- 每个 `tdb_sys_tables` commit marker 都必须有完整、连续 ordinal 的 `tdb_sys_columns` rows，并且对应一个合法 `tables/table_<TableId>.dat`。
- `open_storage` 验证 bootstrap → system files → catalog → user files，并拒绝未被 catalog 引用的 canonical user table file。
- 非 canonical 的无关文件不参与该一致性判断；Storage 不会自动删除或覆盖任何 orphan。
- `create_table` 成功返回时，完整 catalog rows、flush 后的 commit marker 和带合法 Page 0 的用户表文件同时存在。

## 7. Legacy decision

不实现 V1 自动迁移。旧 metadata 格式或与 V2 system catalog 不一致的目录均按 `kCorrupt` 处理，不自动改写磁盘。

## 8. MiniOB adaptation

MiniOB 的 `BufferPoolManager`/`DiskBufferPool` 展示了“管理器按文件名拥有已打开文件对象、统一 create/open/close”的责任边界。tinydbms 借用该所有权分层，但当前 FileManager 直接拥有独立 PageFile，并明确省略 frame cache、pin/dirty、替换策略、日志和恢复。
