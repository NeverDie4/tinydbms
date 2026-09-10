# MiniOB SQL 调用链

本文把 Parser、语义绑定、计划和执行拆开，并给出当前代码中四类 SQL 的真实路径。示例默认使用 CLI/plain、默认 `vacuous` 事务、`heap` 表引擎、tuple iterator。

## 1. SQL 请求的公共骨架

```text
SQL 字符串
  -> Communicator::read_event
  -> SessionEvent
  -> SqlTaskHandler::handle_event
  -> SQLStageEvent
  -> QueryCacheStage
  -> ParseStage
  -> ResolveStage
  -> OptimizeStage
  -> ExecuteStage
  -> SqlResult 或 command executor
  -> Communicator::write_result
```

关键入口：

- `src/observer/net/sql_task_handler.cpp::SqlTaskHandler::handle_event`：读请求、创建 `SQLStageEvent`、调用 `handle_sql`、写响应。
- `src/observer/net/sql_task_handler.cpp::SqlTaskHandler::handle_sql`：按 `query_cache -> parse -> resolve -> optimize -> execute` 顺序串联。
- `src/observer/event/sql_event.h`：`SQLStageEvent` 持有 SQL 文本、`ParsedSqlNode`、`Stmt`、`LogicalOperator`、`PhysicalOperator`。
- `src/observer/event/session_event.*`：持有 `SessionEvent::query`、Session、SqlResult。

## 2. `SELECT * FROM student;` 的逐级调用链

### 2.1 入口和事件

CLI 情况：

```text
CliServer::serve
  -> CliCommunicator::read_event
  -> new SessionEvent
  -> event->set_query(command)
  -> SqlTaskHandler::handle_event
```

网络情况在 SQL 层之前多一步：

```text
NetServer::accept
  -> Communicator::init
  -> OneThreadPerConnectionThreadHandler::new_connection
  -> Worker::operator()
  -> SqlTaskHandler::handle_event
```

输入是原始 `string query`，输出是 `SessionEvent`。`SessionEvent` 构造时创建 `SqlResult`，所以后面所有成功的查询都能通过同一个结果对象回传。

### 2.2 Parser：字符串变成 ParsedSqlNode

```text
SqlTaskHandler::handle_sql
  -> ParseStage::handle_request
  -> parse(sql.c_str(), &ParsedSqlResult)
  -> sql_parse(...)                 // yacc_sql.y 生成的 parser
  -> ParsedSqlResult::sql_nodes()
  -> ParsedSqlNode.flag == SCF_SELECT
```

真实文件：

- `src/observer/sql/parser/parse_stage.cpp::ParseStage::handle_request`
- `src/observer/sql/parser/parse.cpp::parse`
- `src/observer/sql/parser/yacc_sql.y::select_stmt`
- `src/observer/sql/parser/parse_defs.h::ParsedSqlNode / SelectSqlNode`

对于这条 SQL，`yacc_sql.y::select_stmt` 把：

- `SELECT` 后的 `*` 放入 `SelectSqlNode::expressions`，实际对象类型是 `StarExpr`；
- `student` 放入 `SelectSqlNode::relations`；
- 没有 where，因此 `conditions` 为空；
- 没有 group by，因此 `group_by` 为空。

输出是 `unique_ptr<ParsedSqlNode>`，由 `SQLStageEvent::set_sql_node` 保存。它仍然只是语法层结构：表是字符串，字段尚未指向具体 `Table`/`FieldMeta`。

### 2.3 Resolve：ParsedSqlNode 变成绑定后的 SelectStmt

```text
ResolveStage::handle_request
  -> session->get_current_db()
  -> Stmt::create_stmt(db, *sql_node, stmt)
  -> SelectStmt::create(db, sql_node.selection, stmt)
       -> db->find_table("student")
       -> BinderContext::add_table(table)
       -> ExpressionBinder::bind_expression(StarExpr)
       -> bind_star_expression
       -> wildcard_fields(table, bound_expressions)
       -> FilterStmt::create(..., 0, filter_stmt)
  -> SQLStageEvent::set_stmt(stmt)
```

真实文件：

- `src/observer/sql/parser/resolve_stage.cpp::ResolveStage::handle_request`
- `src/observer/sql/stmt/stmt.cpp::Stmt::create_stmt`
- `src/observer/sql/stmt/select_stmt.cpp::SelectStmt::create`
- `src/observer/sql/parser/expression_binder.cpp::ExpressionBinder::bind_star_expression`
- `src/observer/sql/stmt/filter_stmt.cpp::FilterStmt::create`

`*` 不会作为一个最终输出字段留到执行阶段。`wildcard_fields` 从 `TableMeta::sys_field_num()` 开始遍历到 `field_num()`，只生成可见用户字段的 `FieldExpr`。因此如果 MVCC 开启，隐藏事务字段不会被 `SELECT *` 返回。

输出是 `SelectStmt`：

```text
tables_             = { Table* student }
query_expressions_  = { FieldExpr(field1), FieldExpr(field2), ... }
filter_stmt_        = 空 FilterStmt
group_by_           = 空
```

这一步完成了当前 SQL 的主要语义检查：数据库存在、表存在、通配字段可展开、字段可解析。

### 2.4 Logical Plan：SelectStmt 变成逻辑算子树

```text
OptimizeStage::handle_request
  -> create_logical_plan
  -> LogicalPlanGenerator::create(stmt, logical_operator)
  -> LogicalPlanGenerator::create_plan(SelectStmt, logical_operator)
```

对于无 where、无 group by 的单表查询，`logical_plan_generator.cpp` 的构造结果是：

```text
ProjectLogicalOperator(expressions = 所有 FieldExpr)
└─ TableGetLogicalOperator(table = student, mode = READ_ONLY)
```

`create_plan(FilterStmt, ...)` 因为 `filter_units` 为空，不产生 `PredicateLogicalOperator`；`create_group_by_plan` 也不产生 group-by 节点。

注意 `ProjectLogicalOperator` 是 SQL 中的 select-list 投影；`TableGetLogicalOperator` 只表示“从这张表取数据”，还没有决定全表扫还是索引扫。

### 2.5 Rewrite：where 在有条件时的实际位置

本例无谓词，所以 `Rewriter` 不改变上述树。带 where 时，初始形状通常是：

```text
Project
└─ Predicate(Conjunction AND)
   └─ TableGet
```

`Rewriter` 注册了三个规则：

1. `ExpressionRewriter`；
2. `PredicateRewriteRule`；
3. `PredicatePushdownRewriter`。

当谓词可以下推到单个 `TableGetLogicalOperator` 时，`PredicatePushdownRewriter` 会把比较表达式移动到 `table_get_oper->predicates()`；如果整个 Predicate 变成恒真，再由 `PredicateRewriteRule` 去掉它。这个设计使表扫描可以边读边过滤，也给物理计划选择索引留下了入口。

真实文件：`src/observer/sql/optimizer/rewriter.cpp`、`predicate_pushdown_rewriter.cpp`、`predicate_rewrite.cpp`。

### 2.6 Physical Plan：逻辑算子变成执行树

```text
OptimizeStage::generate_physical_plan
  -> PhysicalPlanGenerator::create(ProjectLogicalOperator)
       -> create(child TableGetLogicalOperator)
       -> ProjectPhysicalOperator
  -> PhysicalPlanGenerator::create_plan(TableGetLogicalOperator)
       -> 查找可用 equality index
       -> IndexScanPhysicalOperator 或 TableScanPhysicalOperator
```

对 `SELECT * FROM student`，没有 predicates，且默认使用 tuple iterator，所以结果是：

```text
ProjectPhysicalOperator
└─ TableScanPhysicalOperator(table = student, mode = READ_ONLY)
```

如果是 `WHERE id = 1` 且 `student.id` 有索引，`PhysicalPlanGenerator::create_plan(TableGetLogicalOperator, ...)` 会在 `table->find_index_by_field` 成功时创建 `IndexScanPhysicalOperator`；当前代码只在这个位置对简单等值/不等值表达式做非常直接的索引选择。否则仍是 TableScan。

Cascade 路径的入口是 `OptimizeStage` 中的 `session->use_cascade()` 分支。默认 `use_cascade_ = false`，所以学习简单路径时先读普通 `PhysicalPlanGenerator`。

### 2.7 ExecuteStage 和真正输出

```text
ExecuteStage::handle_request
  -> 检测 SQLStageEvent::physical_operator()
  -> SqlResult::set_operator(std::move(physical_operator))
  -> 返回

PlainCommunicator::write_result
  -> SqlResult::open
       -> session->current_trx()
       -> trx->start_if_need()
       -> ProjectPhysicalOperator::open
       -> TableScanPhysicalOperator::open
       -> Table::get_record_scanner
       -> HeapTableEngine::get_record_scanner
       -> new HeapRecordScanner
       -> HeapRecordScanner::open_scan
  -> write_tuple_result
       -> SqlResult::next_tuple
       -> ProjectPhysicalOperator::next
       -> TableScanPhysicalOperator::next
       -> TableScanPhysicalOperator::current_tuple
       -> RowTuple / Project tuple
  -> SqlResult::close
       -> physical_operator->close
       -> trx->commit()
       -> session->destroy_trx()
```

这里有一个容易忽略的事实：`ExecuteStage` 没有调用 `open`。查询数据的真正读取发生在通信层写结果时。`src/observer/sql/executor/sql_result.cpp` 是执行生命周期的桥梁；`src/observer/net/plain_communicator.cpp::write_tuple_result` 和 `mysql_communicator.cpp::write_tuple_result` 负责不断取 tuple。

## 3. 四类 SQL 的路径比较

### 3.1 CREATE TABLE

```text
SQL
 -> Parser: ParsedSqlNode(SCF_CREATE_TABLE, CreateTableSqlNode)
 -> Resolve: CreateTableStmt::create
 -> Optimize: LogicalPlanGenerator 不支持 CREATE_TABLE，返回 UNIMPLEMENTED
 -> ExecuteStage: CommandExecutor::execute
 -> CreateTableExecutor::execute
 -> Session::get_current_db()->Db::create_table
 -> Table::create
 -> TableMeta::init / serialize
 -> BufferPoolManager::create_file(table.data)
 -> HeapTableEngine::open
```

这个 `UNIMPLEMENTED` 是有意被 stage 串联容忍的：`SqlTaskHandler::handle_sql` 对 optimize 允许 `RC::UNIMPLEMENTED`，然后继续 ExecuteStage。DDL 没有 Logical/Physical Operator。

### 3.2 INSERT

```text
SQL
 -> ParsedSqlNode(SCF_INSERT, InsertSqlNode)
 -> InsertStmt::create
      -> db->find_table
      -> 检查 value 数量
 -> LogicalPlanGenerator::create_plan(InsertStmt)
      -> InsertLogicalOperator(table, values)
 -> PhysicalPlanGenerator::create_plan(InsertLogicalOperator)
      -> InsertPhysicalOperator(table, values)
 -> ExecuteStage::set_operator
 -> SqlResult::open
 -> InsertPhysicalOperator::open
      -> Table::make_record
      -> Trx::insert_record
      -> VacuousTrx::insert_record 或 MvccTrx::insert_record
      -> Table::insert_record
      -> HeapTableEngine::insert_record
      -> RecordFileHandler::insert_record
      -> RecordPageHandler::insert_record
```

`InsertPhysicalOperator` 是“open 即完成一次插入、next 直接 EOF”的单次操作算子。结果没有 tuple schema；`SqlResult::close` 负责提交/销毁单语句事务。

### 3.3 SELECT ... WHERE ...

```text
SQL
 -> ParsedSqlNode.selection.conditions
 -> SelectStmt::create
      -> FilterStmt::create
      -> Field(table, FieldMeta) + Value
 -> LogicalPlanGenerator
      -> Project
         └─ Predicate(Conjunction)
            └─ TableGet
 -> Rewriter::rewrite
      -> PredicatePushdownRewriter
      -> TableGet.predicates = ComparisonExpr 列表
 -> PhysicalPlanGenerator
      -> IndexScanPhysicalOperator（满足简单索引条件）
         或 TableScanPhysicalOperator(predicates)
 -> TableScanPhysicalOperator::next
      -> RecordScanner::next
      -> RowTuple::set_record
      -> TableScanPhysicalOperator::filter
      -> Expression::get_value(tuple, value)
```

如果谓词没有被下推，计划中会保留 `PredicatePhysicalOperator`，它通过 child 的 `current_tuple()` 计算表达式；如果下推，`TableScanPhysicalOperator::filter` 在扫描每条记录时求值。当前代码的默认单表 `AND` 条件大多会走下推路径。

### 3.4 DELETE ... WHERE ...

```text
SQL
 -> ParsedSqlNode(SCF_DELETE, DeleteSqlNode)
 -> DeleteStmt::create
      -> db->find_table
      -> FilterStmt::create
 -> LogicalPlanGenerator::create_plan(DeleteStmt)
      -> DeleteLogicalOperator
         └─ Predicate -> TableGet(READ_WRITE)
 -> PhysicalPlanGenerator
      -> DeletePhysicalOperator
         └─ TableScan/IndexScan 或 Predicate
 -> DeletePhysicalOperator::open
      -> child->open / child->next
      -> 收集 RowTuple 中的 Record
      -> child->close
      -> trx_->delete_record(table_, record)
```

它先收集记录再删除，避免一边扫描一边改变扫描结构。删除的最终效果由事务实现：

- `VacuousTrx::delete_record` 直接 `table->delete_record(record)`。`HeapTableEngine::delete_record` 先删索引项，再调用 `RecordFileHandler::delete_record`，最终清记录位图；这是物理槽位删除，页面中的字节不一定清零。
- `MvccTrx::delete_record` 通过 `Table::visit_record` 找到记录，写隐藏的 `__trx_xid_end`，追加事务日志，不立即清掉槽位；扫描时 `MvccTrx::visit_record` 决定记录对当前事务是否可见。

## 4. 四类 SQL 共用与分叉

| 阶段 | CREATE TABLE | INSERT | SELECT | DELETE |
|---|---|---|---|---|
| Parser / ParsedSqlNode | 共用 | 共用 | 共用 | 共用 |
| Resolve / Stmt | `CreateTableStmt` | `InsertStmt` | `SelectStmt` | `DeleteStmt` |
| 语义检查 | 属性定义、格式 | 表、字段数、值 | 表、字段绑定、where 类型 | 表、字段、where |
| Logical/Physical plan | 不走，返回 `UNIMPLEMENTED` | `InsertLogical/Physical` | `Project/TableGet/...` | `Delete/...` |
| 存储访问 | 创建 `.table/.data` | 写 Record/Page/Index | 扫 Page/Record/Index | 扫描后删或标记删除 |
| 结果生命周期 | CommandExecutor 直接返回状态 | operator open 后无行 | communicator 拉取行/Chunk | operator open 后无行 |

## 5. 这条 SELECT 最终如何到磁盘

当 `HeapRecordScanner` 扫到未缓存页时，调用链是：

```text
HeapRecordScanner::fetch_next_record
  -> BufferPoolIterator::next                 // 取已分配的 page number
  -> RecordPageHandler::init
  -> DiskBufferPool::get_this_page
  -> BPFrameManager::get 或 alloc
  -> DiskBufferPool::load_page
  -> DiskDoubleWriteBuffer::read_page
     或 readn(file_desc, Page, BP_PAGE_SIZE)
  -> RecordPageIterator::next
  -> RowRecordPageHandler::get_record
  -> Record::set_data(页内记录指针)
```

`RowRecordPageHandler::get_record` 默认不复制记录数据，只把 `Record::data_` 指向 Frame 内存；`RecordPageHandler::cleanup` 释放读写 latch 并 `unpin_page`。因此扫描器、页锁、pin count 的生命周期必须一起理解。
