# MiniOB 最值得阅读的类/文件

下面按阅读优先级给出 29 个入口。每个条目都来自当前仓库的真实文件；P0 读完应该能解释最小 SQL 到磁盘的闭环，P1 补齐替代路径和恢复，P2 用于扩展数据库实现。

## P0：必须理解

### 1. `src/observer/main.cpp` — `main`, `parse_parameter`, `init_server`

看进程参数、初始化、协议选择和 `Server::serve` 入口。读懂后应能回答：MiniOB 进程如何从命令行进入数据库服务？默认协议/事务/存储引擎从哪里来？

### 2. `src/observer/common/init.cpp` — `init`, `init_global_objects`, `cleanup`

看日志、全局 `DefaultHandler`、退出同步。读懂后应能回答：数据库对象是在多线程前还是后创建的？程序退出时为什么会 flush。

### 3. `src/observer/net/sql_task_handler.cpp` — `handle_event`, `handle_sql`

这是当前 SQL 请求的主 stage 串联。读懂后应能回答：一个请求依次经过哪些阶段？哪些错误会阻断后续？为什么 DDL 的 `UNIMPLEMENTED` 可以继续。

### 4. `src/observer/event/sql_event.h` — `SQLStageEvent`

看 SQL、ParsedSqlNode、Stmt、LogicalOperator、PhysicalOperator 的生命周期。读懂后应能画出各层中间对象如何移动。

### 5. `src/observer/sql/parser/yacc_sql.y` + `parse_defs.h`

重点看 `select_stmt`、`insert_stmt`、`delete_stmt`、`create_table_stmt` 和 `ParsedSqlNode`。读懂后应能回答：语法解析输出是 AST、Parse Node 还是绑定后的树？每种 SQL 的原始字段在哪里。

### 6. `src/observer/sql/parser/parse.cpp` + `parse_stage.cpp`

看 `parse -> sql_parse -> ParsedSqlResult` 和错误处理。读懂后应能回答：Parser 的边界在哪里？为什么 Parser 只负责名字而不查 Table。

### 7. `src/observer/sql/parser/resolve_stage.cpp` + `src/observer/sql/stmt/stmt.cpp`

看 `ResolveStage::handle_request` 和 `Stmt::create_stmt` 的分发。读懂后应能回答：ParsedSqlNode 如何变成不同 Statement？语义检查从哪一级开始。

### 8. `src/observer/sql/stmt/select_stmt.cpp` + `filter_stmt.cpp`

看表名查找、字段绑定、where 条件转换、FilterUnit。读懂后应能回答：`SELECT *` 何时展开？`WHERE student.id = 1` 何时变成 Field + Value。

### 9. `src/observer/sql/parser/expression_binder.cpp` + `src/observer/sql/expr/expression.h`

看 `StarExpr`、`UnboundFieldExpr`、`FieldExpr`、`ComparisonExpr`、`ConjunctionExpr` 的绑定和求值接口。读懂后应能回答：表达式为什么能同时用于优化、过滤和投影。

### 10. `src/observer/sql/optimizer/logical_plan_generator.cpp`

重点看 `create_plan(SelectStmt)`、`create_plan(FilterStmt)`、`create_plan(InsertStmt)`、`create_plan(DeleteStmt)`。读懂后应能画出四类 SQL 的逻辑算子树。

### 11. `src/observer/sql/operator/logical_operator.h` + `table_get_logical_operator.h`

看 logical operator 的“做什么”语义、child 和 expressions，以及 `TableGetLogicalOperator::predicates`。读懂后应能解释逻辑计划为什么不直接决定全表扫还是索引扫。

### 12. `src/observer/sql/optimizer/optimize_stage.cpp` + `rewriter.cpp`

看逻辑计划、改写、普通物理计划和 Cascade 分叉；重点是 `PredicatePushdownRewriter`。读懂后应能回答：WHERE 什么时候下推到 TableGet？为什么无 where 的 SELECT 没有 Predicate 节点。

### 13. `src/observer/sql/optimizer/physical_plan_generator.cpp`

重点看 `create_plan(TableGetLogicalOperator)`、Project、Insert、Delete 的生成。读懂后应能回答：什么条件选择 IndexScan？默认 SELECT 的物理树是什么？

### 14. `src/observer/sql/operator/physical_operator.h` + `table_scan_physical_operator.cpp`

看 `open/next/current_tuple/close` 协议、TableScan 的 scanner 和 predicate 求值。读懂后应能回答：物理算子为什么是 iterator？一行数据在 operator 树中如何向上传递。

### 15. `src/observer/sql/operator/project_physical_operator.cpp`

看 Project 的 `open -> child->open`、`next -> child->next`、`current_tuple`。读懂后应能回答：SELECT list 如何在扫描结果上形成输出 tuple。

### 16. `src/observer/sql/executor/sql_result.cpp` + `src/observer/net/plain_communicator.cpp`

看真正执行时机：`ExecuteStage` 只设置 operator，`PlainCommunicator::write_result` 才 open/next/close。读懂后应能回答：为什么 SQL stage 返回成功时磁盘可能还没被读？事务何时提交。

### 17. `src/observer/storage/default/default_handler.*` + `src/observer/storage/db/db.*`

看 DefaultHandler、Db、opened DB/table、DB 初始化和恢复。读懂后应能回答：当前 Session 的 Table* 从哪里来？重启时哪些文件被扫描？一个 Db 为什么拥有独立 BufferPool、Log、TrxKit。

### 18. `src/observer/storage/table/table.*` + `table_engine.h`

看 Table 的 facade API、`make_record`、`create/open` 和 `TableEngine` 抽象。读懂后应能回答：SQL 层为什么不直接碰 RecordFileHandler？HEAP 和 LSM 的替换点在哪里。

### 19. `src/observer/storage/table/heap_table_engine.*`

看 Heap 引擎怎样把 Table 接到数据页、scanner、索引。读懂后应能回答：一个表如何打开自己的 `.data` 文件？插入为什么还要同步维护索引。

### 20. `src/observer/storage/record/record.h` + `record_manager.h`

看 `RID`、`Record`、`PageHeader`、`RecordPageHandler`、`RecordFileHandler` 的职责边界。读懂后应能回答：一行的物理格式是什么？RID 如何定位槽位？一个表有哪些页面由谁维护。

### 21. `src/observer/storage/record/record_manager.cpp`

重点看 `init_empty_page`、`RowRecordPageHandler::insert_record/get_record/delete_record`、`RecordFileHandler::insert_record`、`init_free_pages`。读懂后应能手算一个页面的 header/bitmap/slot 布局，并完整解释 INSERT/scan。

### 22. `src/observer/storage/record/heap_record_scanner.cpp`

看 BufferPoolIterator、RecordPageHandler、RecordPageIterator 的嵌套生命周期。读懂后应能回答：SELECT 如何跨 page 扫描？空槽如何跳过？MVCC 的 visit_record 放在哪里。

### 23. `src/observer/storage/buffer/page.h` + `frame.h`

看 Page 物理大小、LSN/checksum、Frame 的 dirty/pin/latch/access。读懂后应能回答：磁盘页和内存页有什么区别？为什么页面不能随意被淘汰。

### 24. `src/observer/storage/buffer/disk_buffer_pool.h` + `disk_buffer_pool.cpp`

重点看 `BPFileHeader`、`BPFrameManager`、`get_this_page`、`allocate_page`、`load_page`、`flush_page_internal`。读懂后应能解释 page id、页缓存命中、淘汰、分配和 flush。

### 25. `src/observer/storage/field/field_meta.*` + `table_meta.*`

看字段偏移、隐藏字段、record size、JSON serialize/deserialize。读懂后应能回答：CREATE TABLE 后 schema 放在哪？重启如何重建 FieldMeta？为什么 `SELECT *` 不包含 MVCC 字段。

## P1：重要

### 26. `src/observer/storage/index/bplus_tree_index.*` + `bplus_tree.h/cpp`

看索引文件头、key = field bytes + RID、内部页/叶页、分裂合并和 scanner。读懂后应能回答：WHERE 如何从索引得到 RID？得到 RID 后为什么还要回表。

### 27. `src/observer/storage/trx/trx.h` + `vacuous_trx.*` + `mvcc_trx.*`

对比无事务转发和 MVCC 隐藏字段/可见性/操作记录。读懂后应能回答：DELETE 何时是物理删除，何时只是逻辑标记？单语句事务在哪里开始和提交。

### 28. `src/observer/storage/clog/disk_log_handler.*` + `integrated_log_replayer.*`

看日志后台刷盘、LSN、checkpoint、按模块回放。读懂后应能回答：页面修改、记录修改、B+ Tree 修改和事务日志如何恢复。

### 29. `src/observer/storage/buffer/double_write_buffer.*` + `src/observer/catalog/catalog.*`

前者用于页原子写和 checksum 恢复；后者只保存当前内存中的 `TableStats`，不是 schema system catalog。读懂后应能明确 durability 和 optimizer statistics 的边界。

## P2：扩展

- `src/observer/sql/optimizer/cascade/`：Cascades memo、group、rule、task。理解普通 RBO/物理计划后再读。
- `src/observer/sql/operator/*_vec_physical_operator.*`、`storage/common/chunk.*`：chunk/vectorized 执行。
- `src/observer/storage/record/record_manager.cpp` 中 `PaxRecordPageHandler`：PAX 布局当前仍有未实现分支，适合作为存储格式扩展练习。
- `src/observer/storage/table/lsm_table_engine.*` 与 `src/oblsm/`：独立 LSM-tree、memtable、SSTable、WAL、compaction。
- `src/observer/net/mysql_communicator.*`、`src/observer/net/server.*`：协议细节、连接线程和 MySQL result packet。
- `src/observer/storage/index/bplus_tree_log.*`、`record/record_log.*`、`buffer/buffer_pool_log.*`：深入 WAL/redo 实现。

## 推荐阅读顺序

不要按文件系统目录顺序读。建议沿着一个 `SELECT * FROM student` 的数据流走：

| 顺序 | 文件/类 | 重点 | 读懂后应能回答 |
|---|---|---|---|
| 1 | `main.cpp`、`common/init.cpp` | 启动、DefaultHandler、Server | 进程怎样进入数据库服务 |
| 2 | `server.*`、`cli_communicator.*`、`sql_task_handler.*` | 请求进入、SessionEvent | SQL 字符串在哪里第一次被接收 |
| 3 | `parse.cpp`、`parse_stage.cpp`、`yacc_sql.y`、`parse_defs.h` | Parser 输出 | ParsedSqlNode 里到底保存什么 |
| 4 | `resolve_stage.cpp`、`stmt.cpp`、`select_stmt.cpp`、`expression_binder.cpp` | 语义绑定 | 表名/字段名何时变成对象指针 |
| 5 | `logical_plan_generator.cpp`、`logical_operator.h` | 逻辑树 | SELECT/WHERE 变成哪些“要做什么”节点 |
| 6 | `rewriter.cpp`、`predicate_pushdown_rewriter.cpp` | 谓词改写 | WHERE 为什么会靠近 TableGet |
| 7 | `physical_plan_generator.cpp`、`physical_operator.h` | 物理树选择 | 什么时候是 TableScan，什么时候是 IndexScan |
| 8 | `sql_result.cpp`、`project_physical_operator.cpp`、`table_scan_physical_operator.cpp` | iterator 生命周期 | open/next/close 和 tuple 如何流动 |
| 9 | `table.cpp`、`heap_table_engine.cpp` | Table facade 和 Heap | 物理表文件由谁打开、扫描、写入 |
| 10 | `record.h`、`record_manager.h/cpp`、`heap_record_scanner.cpp` | RID、PageHeader、slot、bitmap | 一条记录怎样放入/取出一个页 |
| 11 | `page.h`、`frame.h`、`disk_buffer_pool.*` | Page/Frame/Buffer Pool | 页怎样从文件进入内存、何时淘汰/flush |
| 12 | `table_meta.*`、`field_meta.*`、`meta_util.*` | schema 持久化 | 重启后表结构怎样恢复 |
| 13 | `bplus_tree_index.*`、`bplus_tree.*` | 索引回表 | 索引返回的 RID 怎样连接到记录 |
| 14 | `trx.*`、`sql_result.cpp` | 事务边界和删除语义 | Vacuous 与 MVCC 的差异 |
| 15 | `disk_log_handler.*`、`integrated_log_replayer.*`、`double_write_buffer.*` | durability/recovery | 崩溃后如何重做日志和恢复坏页 |
| 16 | `catalog.*`、`cascade/`、`*_vec_physical_operator.*`、`oblsm/` | 统计、优化器和替代引擎 | 如何在闭环之上扩展系统 |

每读完一步，建议用一个固定问题自测：当前对象的输入是什么、输出是什么、谁拥有它、谁释放它、是否直接指向 Buffer Pool 内存。尤其是 `Record` 是否 owner、Frame 是否 pinned，是 MiniOB 存储代码中最容易漏掉的生命周期细节。
