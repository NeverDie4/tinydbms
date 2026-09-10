# MiniOB Catalog 与 Metadata

## 1. 名称对应关系

MiniOB 当前没有一个传统意义上“所有表结构都存成系统表、查询系统表恢复 schema”的完整 System Catalog。它把不同层次的信息分散在以下结构中：

| 概念 | 当前代码中的承担者 | 作用 | 是否持久化 |
|---|---|---|---|
| Database | `DefaultHandler::opened_dbs_` + `Db` | 管理一个数据库目录、表、BufferPool、Log、TrxKit | DB 目录和 `.db` 文件；打开对象是运行时 |
| Table | `Db::opened_tables_` + `Table` | 表级 API，持有 `TableMeta` 和 `TableEngine` | 表结构在 `.table`，数据在 `.data` |
| Field | `Field` | 运行时把 `Table *` 与 `FieldMeta *` 组合成可绑定字段 | 间接由 TableMeta 持久化 |
| FieldMeta | `FieldMeta` | 字段名、类型、偏移、长度、可见性、field id | `TableMeta` JSON 中的 `fields` |
| TableMeta | `TableMeta` | table id、表名、字段、索引元数据、主键、存储格式/引擎、record size | `<table>.table` JSON |
| IndexMeta | `IndexMeta` | 索引名与被索引字段名 | `<table>.table` JSON 的 `indexes` |
| System Catalog | 没有负责 schema 的系统表；`src/observer/catalog/Catalog` 仅是表统计 singleton | Cascades/成本模型目前可读取 `TableStats.row_nums` | 当前不持久化，源码明确写了 planned via system table |

## 2. Database 的内存对象

`src/observer/storage/default/default_handler.h` 的 `DefaultHandler` 是 SQL 层看到的存储入口：

```text
DefaultHandler
 ├─ base_dir_ / db_dir_
 └─ map<string, Db *> opened_dbs_
      └─ Db
          ├─ name_ / path_
          ├─ unordered_map<string, Table *> opened_tables_
          ├─ BufferPoolManager
          ├─ LogHandler
          ├─ TrxKit
          └─ ObLsm*
```

`GCTX.handler_` 指向全局的 `DefaultHandler`。Session 不保存数据库名字符串来做查询，而是保存当前 `Db *`；`Session::set_current_db` 通过 `GCTX.handler_->find_db` 找到它。

`Db::find_table` 再从 `opened_tables_` 按名字返回 `Table *`。所以 Resolve 阶段的表解析实际上是内存 map 查找，不会每次 SQL 都重新打开 `.table` 文件。

## 3. TableMeta 里面有什么

`TableMeta` 的关键字段：

```text
table_id_
name_
trx_fields_       MVCC 隐藏字段的 FieldMeta
fields_           用户字段 + sys fields，按物理偏移排列
indexes_
primary_keys_
storage_format_   ROW_FORMAT / PAX_FORMAT
storage_engine_   HEAP / LSM
record_size_
```

`TableMeta::init` 接收 Parser 产生的 `AttrInfoSqlNode`，为每个字段计算连续 `field_offset`，调用 `FieldMeta::init`，并把事务字段放在用户字段之前。`sys_field_num()` 返回隐藏字段数量；`field_num()` 返回全部字段数量；`record_size()` 是整行物理大小。

`FieldMeta` 的持久化属性是：

```text
name, type, offset, len, visible, FIELD_id
```

`visible=false` 的字段不会被 `SELECT *` 展开；MVCC 的 `__trx_xid_begin` 和 `__trx_xid_end` 就属于这种字段。

## 4. CREATE TABLE 后元数据如何保存

真实路径：

```text
CREATE TABLE
 -> yacc_sql.y::create_table_stmt
    -> ParsedSqlNode.create_table
 -> Stmt::create_stmt
    -> CreateTableStmt::create
 -> CommandExecutor::execute
 -> CreateTableExecutor::execute
 -> Session::get_current_db()->Db::create_table
      -> table_meta_file(path, table_name)  // <name>.table
      -> new Table
      -> table_id = next_table_id_++
      -> Table::create
           -> 创建 <name>.table 文件
           -> TableMeta::init
           -> TableMeta::serialize(fstream)
           -> BufferPoolManager::create_file(<name>.data)
           -> 创建 HeapTableEngine/LsmTableEngine 并 open
      -> opened_tables_[table_name] = table
 -> CommandExecutor 发现 stmt_type_ddl
      -> current_db->sync()
```

`TableMeta::serialize` 使用 JsonCpp，把以下顶层字段写入 `.table`：

```text
table_id
table_name
storage_format
storage_engine
fields[]
indexes[]
primary_keys[]
```

当前 `CREATE TABLE` 的 storage engine 来自启动参数的 `Db::get_storage_engine()`；SQL 中的 `storage_format` 由 `CreateTableStmt::get_storage_format` 解析为空/ROW/PAX。

注意 SQL 层的 `CreateTableExecutor` 只负责把已经解析好的属性传给 Db；文件命名、TableMeta 序列化、data 文件创建是 `Db`/`Table`/`BufferPoolManager` 的职责。

## 5. 程序重启后如何恢复

```text
main
 -> init
 -> DefaultHandler::init
 -> open_db("sys")
 -> Db::init
      -> init_meta                       // 读取 <db>.db 的 checkpoint LSN
      -> open_all_tables
           -> list_file(path, ".*\\.table$")
           -> for each <table>.table:
                new Table
                Table::open(db, filename, path)
                  -> TableMeta::deserialize(fstream)
                  -> 根据 storage_engine 创建对应 TableEngine
                  -> engine->open()
                     -> HeapTableEngine::init
                          -> BufferPoolManager::open_file(<table>.data)
                          -> RecordFileHandler::init
                     -> 读取 TableMeta.indexes，逐个 BplusTreeIndex::open
                -> Db::opened_tables_[table->name()] = table
      -> init_dblwr_buffer / recover
```

`TableMeta::deserialize` 读 JSON，重建 `FieldMeta`、按 offset 排序字段、重建隐藏事务字段、重建 `IndexMeta` 和主键列表，并重新计算 `record_size_`。`Db::open_all_tables` 还会根据持久化的 `table_id` 更新 `next_table_id_`，避免重启后生成重复 table id。

因此恢复表结构不依赖 `Catalog` singleton，也不依赖当前 SQL；只依赖磁盘中的 `.table` 文件。

## 6. IndexMeta 与实际 B+ Tree 文件

`IndexMeta` 只记：

```text
name_   // 逻辑索引名
field_  // 字段名
```

在 `HeapTableEngine::create_index` 中，先建立 `IndexMeta`，通过 `table_index_file(db_path, table, index)` 得到 `<table>-<index>.index`，创建 `BplusTreeIndex`，扫描全表填充索引，再把新的 `IndexMeta` 加进一个复制的 `TableMeta`，写临时 `.table.tmp`，rename 覆盖正式 `.table`，最后 `table_meta_->swap(new_table_meta)`。

重启时 `HeapTableEngine::open` 遍历 `table_meta_->index_num()`，根据每个 `IndexMeta` 找字段、打开对应索引分页文件并放入 `indexes_`。所以 `.table` 是索引“目录”，`.index` 才是索引页数据。

## 7. Catalog 类到底做了什么

`src/observer/catalog/catalog.h` 的 `Catalog` 是一个 singleton：

```cpp
unordered_map<int, TableStats> table_stats_;
```

`get_table_stats(table_id)` 和 `update_table_stats(table_id, TableStats)` 只读写这张内存 map。`TableGetLogicalOperator::find_log_prop` 用它读取 `row_nums` 估算逻辑基数；`AnalyzeTableExecutor` 是应继续阅读的统计更新入口。

源码注释明确说明这张统计 map 当前“不持久化”，未来才计划通过 system table 持久化。因此本版本的 System Catalog 结论是：

> 有一个名字叫 `Catalog` 的统计组件，但没有由它承担数据库表结构恢复的系统目录；表结构由 `.table` 文件和 `Db::open_all_tables` 直接恢复。

## 8. 学习时容易混淆的三种“元数据”

1. **Schema metadata**：`TableMeta`、`FieldMeta`、`IndexMeta`，持久化在表 `.table` JSON。
2. **File/page metadata**：`BPFileHeader`、bitmap、page_count，持久化在各分页文件 page 0。
3. **Optimizer statistics**：`Catalog::table_stats_`，当前只在内存中用于逻辑基数/成本估算。

它们互相有关但不是同一个 Catalog：TableMeta 告诉执行器“字段如何解释”；BPFileHeader 告诉 Buffer Pool“哪些页存在”；Catalog statistics 告诉优化器“估计有多少行”。
