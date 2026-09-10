# MiniOB 当前代码架构

> 本文档基于当前仓库 `D:\MiniOB\miniob` 的源码整理。这里的外层 `D:\MiniOB` 不是 Git 根目录；源码仓库是内层 `miniob`。本文只分析代码，不把仓库内已有设计文档当成调用关系的唯一依据。

## 1. 先看结论：MiniOB 是什么形状

当前版本不是“SQL 直接调用 Table”的单层实验程序，而是一个分层的教学数据库：

```text
进程/协议层
  main.cpp -> init() -> DefaultHandler -> Db -> Server
  CLI / plain / MySQL communicator -> SessionEvent

SQL 层
  SqlTaskHandler
    -> ParseStage: SQL 字符串 -> ParsedSqlNode
    -> ResolveStage: ParsedSqlNode -> Stmt / 绑定后的 Expression
    -> OptimizeStage: Stmt -> LogicalOperator -> PhysicalOperator
    -> ExecuteStage: 将物理算子交给 SqlResult，或执行 DDL CommandExecutor

执行层
  SqlResult::open/next/close
    -> PhysicalOperator 树
    -> Table / Trx / Index

存储层
  HeapTableEngine 或 LsmTableEngine
    -> RecordFileHandler / Lsm scanner
    -> DiskBufferPool
    -> Frame / Page
    -> table.data / index 文件
    -> 操作系统文件描述符

恢复层
  LogHandler / DiskLogHandler + IntegratedLogReplayer
  DoubleWriteBuffer 负责页写入的二次缓冲和崩溃恢复辅助
```

默认启动参数是：`vacuous` 事务、`heap` 存储引擎、`plain` 协议、tuple iterator。它们可以由 `src/observer/main.cpp` 的命令行参数切换。`StorageEngine::LSM`、MVCC、PAX、chunk iterator、Cascade optimizer 是同一份代码中的可选路径，不应和默认路径混在一起阅读。

## 2. 模块树

```text
miniob/
├─ src/observer/main.cpp                         进程入口
├─ src/observer/common/                          类型、Value、全局初始化
│  ├─ init.cpp                                   全局初始化/清理
│  ├─ global_context.*                           GCTX.handler_
│  └─ type/                                      数据类型和隐式转换
├─ src/observer/net/                              Server、协议、连接线程
│  ├─ server.*                                   NetServer / CliServer
│  ├─ communicator.*                             通信抽象
│  ├─ plain_communicator.*                       教学文本协议
│  ├─ mysql_communicator.*                      MySQL 协议
│  ├─ cli_communicator.*                         stdin/stdout CLI
│  └─ sql_task_handler.*                         一次 SQL 请求的入口
├─ src/observer/session/                          Session 和 SQL stage 串联
│  ├─ session.*                                  当前 DB、事务、执行模式
│  └─ session_stage.*                            老的/共享的 stage 串联逻辑
├─ src/observer/event/                            SessionEvent、SQLStageEvent
├─ src/observer/sql/
│  ├─ parser/                                    Flex/Bison + 语义绑定
│  ├─ stmt/                                      Statement 层
│  ├─ expr/                                      Expression、Tuple、Chunk
│  ├─ optimizer/                                 逻辑计划、改写、物理计划、Cascade
│  ├─ operator/                                  Logical/Physical Operator
│  ├─ executor/                                  DDL 命令和 SqlResult
│  ├─ plan_cache/、query_cache/                  请求前置 stage，目前不是主存储路径
│  └─ optimizer/cascade/                         可选 Cascades 优化器
├─ src/observer/storage/
│  ├─ default/                                   SQL 层到 Db 的旧 handler 入口
│  ├─ db/                                        一个目录对应一个 Db
│  ├─ table/                                     Table、TableMeta、表引擎
│  ├─ field/                                     FieldMeta / Field
│  ├─ record/                                    页内记录组织、扫描器
│  ├─ buffer/                                    Page、Frame、Buffer Pool
│  ├─ index/                                     Index 抽象、B+ Tree
│  ├─ trx/                                       Vacuous / MVCC / LSM 事务
│  ├─ clog/                                      commit/redo log 和 replay
│  ├─ persist/                                   较低层的通用文件读写封装
│  └─ common/                                    Chunk、codec、过滤器、元数据路径
├─ src/observer/catalog/                          运行时表统计 Catalog，不是表结构系统表
├─ src/oblsm/                                    独立的 LSM-tree 实现
├─ src/obclient/                                 客户端程序
├─ unittest/                                    GoogleTest 单元测试
├─ test/case/                                   SQL case + result
├─ test/integration_test/                        Python 集成测试框架
└─ benchmark/、tools/                            性能测试和 clog 工具
```

## 3. 启动过程

真实入口是 `src/observer/main.cpp::main`：

1. `parse_parameter` 读取 `-P` 协议、`-t` 事务模型、`-E` 存储引擎、`-d` durability 等参数。
2. `init(the_process_param())` 位于 `src/observer/common/init.cpp`。
3. `init` 加载 ini、初始化日志，然后 `init_global_objects` 创建 `DefaultHandler` 并调用：
   `DefaultHandler::init("miniob", trx_kit_name, durability_mode, storage_engine)`。
4. `DefaultHandler::init` 创建/打开 `sys` 数据库，并让 `Session::default_session()` 选中 `sys`。
5. `Db::init` 创建事务 kit、`BufferPoolManager`、double-write buffer、`LogHandler`；读取 DB checkpoint；打开所有 `.table`；恢复日志。
6. `init_server` 创建 `CliServer` 或 `NetServer`，`Server::serve` 开始接收请求。
7. 服务停止后 `cleanup`，销毁 `DefaultHandler`；`DefaultHandler::destroy` 调用 `sync`，再关闭数据库和表。

`Db::init` 的顺序尤其值得注意：它先建立缓冲池和日志，再 `init_meta`、`open_all_tables`、double-write recovery、`recover`。因此“打开数据库”不仅是读一个数据库名，还会重建可查询的内存对象图。

## 4. SQL 请求从哪里进入

当前常用路径是：

```text
CliServer::serve 或 NetServer::serve
  -> Communicator::read_event
  -> SessionEvent { query, Session, SqlResult }
  -> SqlTaskHandler::handle_event
  -> SqlTaskHandler::handle_sql
```

网络模式中，`NetServer::accept` 创建 `Communicator`，再交给 `OneThreadPerConnectionThreadHandler` 或 `JavaThreadPoolThreadHandler`。每个连接最终都调用 `SqlTaskHandler::handle_event`。CLI 模式则在 `CliServer::serve` 中循环调用同一个 `SqlTaskHandler`。

`SessionStage::handle_sql` 有一份基本相同的 stage 串联，`SessionStage::handle_request2` 还会设置线程当前 Session；但当前 `SqlTaskHandler::handle_event` 会自己创建 `SQLStageEvent` 并直接执行自己的 `handle_sql`。阅读主调用链时应以 `src/observer/net/sql_task_handler.cpp` 为准，同时知道 `SessionStage` 是保留下来的重复/兼容路径。

## 5. SQL 层的层次和依赖

### Parser

`src/observer/sql/parser/yacc_sql.y` 定义 SQL 语法，`lex_sql.l` 定义词法，`parse.cpp` 暴露 `parse(const char *, ParsedSqlResult *)`。Parser 输出的是 `ParsedSqlResult`，其中含 `ParsedSqlNode`，它按 `SqlCommandFlag` 保存 `SelectSqlNode`、`InsertSqlNode`、`CreateTableSqlNode` 等结构。

它不是最终 AST：例如字段仍是名字，表也只是字符串；表达式先是 `StarExpr`、`UnboundFieldExpr` 等未绑定表达式。

### Resolve / Statement

`ResolveStage::handle_request` 调用 `Stmt::create_stmt`。各个 `Stmt::create` 用当前 `Db` 查表、查字段、检查字段数，并把 SQL 名字变成 `Table *`、`Field`、绑定后的 `Expression`。`SelectStmt::create` 还调用 `ExpressionBinder` 展开 `*` 为所有可见字段，并生成 `FilterStmt`。

因此 `Stmt` 是“语义检查 + 名称绑定”的中间结构，而不是简单复制 Parser 结构。

### Plan 和 Operator

`OptimizeStage` 先让 `LogicalPlanGenerator` 根据 `Stmt` 创建逻辑算子树，再使用 `Rewriter` 做表达式改写和谓词下推，最后由 `PhysicalPlanGenerator` 生成物理算子树。默认 session 是 tuple iterator、非 Cascade；若 session 开启 `use_cascade`，则转由 `sql/optimizer/cascade/Optimizer` 选择物理表达式。

逻辑算子描述“要做什么”：`TABLE_GET`、`PREDICATE`、`PROJECTION`、`INSERT`、`DELETE` 等；物理算子描述“怎么做”：`TABLE_SCAN`、`INDEX_SCAN`、`PROJECT`、`INSERT`、`DELETE` 等。`TableGetLogicalOperator` 的谓词可能在生成物理计划时选择 B+ Tree，也可能选择全表扫描。

### Executor

有物理计划的 DML 由 `ExecuteStage` 把 root operator 放进 `SqlResult`，它本身不立即扫描数据。结果输出阶段才调用 `SqlResult::open`、`next_tuple`/`next_chunk`、`close`。

DDL 和其它 command 没有对应查询计划：`OptimizeStage` 对它们返回 `RC::UNIMPLEMENTED`，stage 串联明确允许这个返回码继续；`ExecuteStage` 随后调用 `CommandExecutor`，由 `CreateTableExecutor` 等直接操作 `Db`。

## 6. 存储、事务和恢复的依赖关系

```text
Db
 ├─ unordered_map<string, Table *> opened_tables_
 ├─ BufferPoolManager             管所有 data/index/dblwr 文件的 Frame
 ├─ LogHandler                    管 CLog 文件、后台刷日志线程
 └─ TrxKit                        生成当前 Session 使用的 Trx

Table
 └─ TableEngine
     ├─ HeapTableEngine -> RecordFileHandler -> DiskBufferPool
     │                    -> BplusTreeIndex -> BplusTreeHandler
     └─ LsmTableEngine  -> src/oblsm 的 LSM 迭代器和 WAL

DiskBufferPool
 ├─ BPFileHeader (page 0 的分配位图)
 ├─ Page/Frame
 └─ DoubleWriteBuffer + BufferPoolLogHandler

LogHandler
 └─ IntegratedLogReplayer
     ├─ BufferPoolLogReplayer
     ├─ RecordLogReplayer
     ├─ BplusTreeLogReplayer
     └─ Vacuous/MvccTrxLogReplayer
```

事务接口位于 `storage/trx/trx.h`。默认 `VacuousTrx` 只把插入/删除转发给 `Table`；`MvccTrx` 会把隐藏的 begin/end xid 字段放入记录，并通过 `MvccTrxLogHandler` 记录事务操作。

## 7. 测试体系

- `unittest/observer`：GoogleTest 单元测试，覆盖 parser、expression、catalog、record manager、disk buffer pool、B+ Tree、日志、事务和并发等。
- `test/case`：SQL 文件和 result 文件，`test/case/miniob_test.py` 驱动 case 回归。
- `test/integration_test`：Python 框架，可启动 MiniOB 和 MySQL，对比相同 SQL 的输出，也能运行单测和性能测试。
- `benchmark`：服务器并发、record manager 并发、B+ Tree 并发、算子性能等。
- `tools/clog_dump.cpp`：用于观察日志文件。

构建入口是根 `CMakeLists.txt` 和 `build.sh`；单测目标在 `unittest/CMakeLists.txt` 以及各子目录中注册。不要把 `Catalog` 单元测试误认为系统表测试：当前 `Catalog` 只保存内存中的表统计。

## 8. 阅读时的两个边界

1. “物理算子执行”与“SQL stage 完成”是两件事。`ExecuteStage` 只是设置 `SqlResult::operator_`；真正的表扫描发生在 communicator 输出结果时。
2. “删除一行”取决于事务 kit。Vacuous 路径清掉记录位图并维护索引；MVCC 路径写隐藏 end xid，记录仍在页面中，之后由可见性判断过滤。后文会分别展开。
