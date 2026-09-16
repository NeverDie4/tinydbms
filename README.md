# tinydbms

`tinydbms` 是一个单进程的教学数据库：同一个 `Database` 实例可以被多个执行流并发调用，core 内部按调用级互斥把调用安全地串行化（不承诺并行度）；storage 的公共 API 仍约定调用方串行调用，core 的串行化正是这条前提的来源。当前已经具备从 SQL 文本到页式持久化存储的完整模块边界和最小执行路径。

## 当前状态

- `app` 提供命令行入口：`--help`/`--version`/`--data-dir` 等选项解析、REPL 与 stdin 批处理
  （完整选项见下方「CLI 入口」）。
- `compiler` 提供分句、词法、语法、语义、优化和 Plan 生成。
- `core` 提供 `Database` 生命周期、Catalog 恢复、TableId 分配和脚本顺序执行；源码按 database（生命周期）、script、executor、expression、plan_text（计划渲染）与 diagnostics/analysis_catalog（诊断与 analyze 策略）拆分。
- `storage` 提供 typed heap storage、buffer pool、record/page 编解码、cursor，以及以 System Catalog 为 schema 权威的持久化元数据。
- `gui`（可选，`TINYDBMS_BUILD_GUI=ON`）是 Qt6 前端：结果表格与图表视图、结构化诊断列表、运行中取消、计划模式（只编译不执行）、结果上限档位与结果导出（CSV / 剪贴板）；与 CLI 一样只调用 core 公共 API。
- 默认构建仍通过不可用 Session 适配器验证 CLI 边界；完整 SQL 链路需要启用 `TINYDBMS_ENABLE_REAL_MODULES=ON`。

## 当前架构

```text
App (CLI)  /  GUI (Qt6, 可选)
  |
Core Database / Executor
  |- Compiler
  |   |- Lexer
  |   |- Parser
  |   |- Semantic
  |   |- Planner
  |   `- Optimizer
  `- Storage
      |- HeapTable
      |- BufferPool
      |- RecordPage
      |- SlottedPage
      |- RecordCodec
      `- PageFile
```

`core::Database` 持有运行时 Catalog 快照并执行 Compiler 生成的 Plan。Core 只调用 `include/tinydbms/storage.hpp` 的公共 API。Storage 负责 metadata、表文件、页面、BufferPool 和 cursor，既不知道 SQL，也不计算 WHERE 或 projection。

## 当前 SQL 范围

已支持：

- `CREATE TABLE`，含 `NULL` / `NOT NULL` 列约束
- `INSERT`，包括完整的显式列重排
- `SELECT`、列投影与 `WHERE`，包括比较、`AND`、`OR`、`NOT` 与 SQL 三值逻辑
- `UPDATE`，采用扫描、构造完整替换行、关闭 cursor、批量替换流程（非事务性）
- `ORDER BY`、`INNER JOIN`（含限定列引用）与 `GROUP BY` 的 `COUNT`/`SUM`/`AVG`/`MIN`/`MAX`
- `DELETE`，采用扫描、收集 RID、关闭 cursor、批量删除流程
- 正常 close/reopen 后的 schema 和记录持久化

public type 为 `INT`（`int32_t`）、`BIGINT`（`int64_t`）、`DOUBLE`、`BOOLEAN` 和 `VARCHAR`，
SQL NULL 由 `Value` 的 `std::monostate` 表示；列元数据带 `nullable`，SQL 建表省略约束时默认允许
NULL，`NOT NULL` 必须显式写出（`ColumnMeta` 的 C++ 默认值 `false` 只用于兼容 SQL v1 的非空语义，
与 SQL 省略约束的默认值不是同一件事，见 [SQL v2 扩展契约](docs/SQLv2扩展契约.md) §4.1）。
`VARCHAR` 必须是合法 UTF-8 且不超过 1024 字节，单行逻辑载荷不超过 4096 字节。
逻辑类型与物理编码的对应关系以
[docs/SQLv2扩展契约.md](docs/SQLv2扩展契约.md) §10 与 `include/tinydbms/common.hpp` 为准。

SQL v2 明确不包含：算术与负数字面量（`-1` 在词法阶段即报 `invalid character`，因此无法直接
表达负数）、表/列别名（`AS`）、`HAVING`、`DISTINCT`、子查询、`UNION`、`LIMIT`/`OFFSET`、
`LEFT`/`RIGHT`/`FULL`/`CROSS JOIN`、`PRIMARY KEY`/`FOREIGN KEY`/`UNIQUE`/`DEFAULT`/`CHECK`、
`DATE`/`TIME`/`DECIMAL`/`CHAR`/`TEXT`/`BLOB`、事务与回滚、WAL 与崩溃原子性、基于代价的优化器。
完整清单见 [SQL v2 扩展契约](docs/SQLv2扩展契约.md) §17。

`storage.meta` 是 V2 bootstrap，不保存用户 schema。`tdb_sys_tables`（TableId 0）与
`tdb_sys_columns`（TableId 1）是 Storage 管理的特殊 HeapTable，保存用户 schema；它们可由
`SELECT` 读取，但普通 `CREATE`、`INSERT` 与 `DELETE` 不能修改。普通用户表从 TableId 2 开始。

当前未实现 FSM、Index、WAL、Transaction、MVCC、复杂 SQL，以及多个数据库并发打开。一个进程中可以创建多个 `Database` 对象，但 Storage 是 singleton，同一时刻最多一个对象处于 open 或 cleanup-pending 状态。
storage 也不提供跨进程文件锁：两个进程同时打开同一 `--data-dir` 不在支持范围内，会互相覆盖且
不报错，跨进程互斥属于后续扩展。

## CLI 入口

CLI 入口选项：`--data-dir DIR`、`--error-policy stop|analyze`、`--format table|pretty|json`
（`json` 为每行一个对象的 NDJSON，stdout 只放结果、stderr 只放诊断；`pretty` 是终端里
给人看的等宽表格，输出不是终端时默认仍是 `table` 的制表符文本）、`--plan`
（只编译并打印执行计划，零副作用）、`--max-rows N`（单条语句在内存中物化的最大行数，
缺省为 `kMaxQueryRows` = 262144，`0` 与非法值按参数错误处理）、`--time`
（每次执行在 stderr 追加一行墙钟耗时，如 `TIME script 12.345 ms`，stdout 不受影响）、
`--stats`（每次执行在 stderr 追加一行 buffer pool 快照，如
`BUFFER fetch=8 hit=5 miss=3 miss_rate=37.50% evictions=0 flushes=1`，快照在 close
之前取，观测失败只写诊断、不影响退出码）。
`--format`、`--plan` 与 `--max-rows` 可组合，行为契约见
[docs/core-cli/第三阶段CLI与入口设计.md](docs/core-cli/第三阶段CLI与入口设计.md)。
展示格式与性能观测的完整规则（边框、CJK 显示宽度、数值列右对齐、超宽单元格截断到
48 列，以及 `--plan` 不截断计划文本、不打印行数行的例外）见
[docs/core-cli/CLI展示格式与性能观测设计.md](docs/core-cli/CLI展示格式与性能观测设计.md)。

运行中取消：CLI 在 `execute_script` 期间按 Ctrl+C 会请求取消（`CancelToken`），当前语句在
下一个无副作用检查点结束、剩余语句不再执行并记为 `CANCELLED`，本次调用退出码为 1；
空闲期或第二次 Ctrl+C 按默认处置终止进程。GUI 工具栏提供「取消」按钮：请求只置位令牌，
当前语句在下一个检查点结束后按 `kCancelled` 渲染（已实现，见
[docs/gui/GUI设计.md](docs/gui/GUI设计.md) §17）。规格见
[docs/core-cli/运行中取消设计.md](docs/core-cli/运行中取消设计.md)。

## 公共契约

正式公共入口是：

```text
include/tinydbms/common.hpp
include/tinydbms/diagnostic.hpp
include/tinydbms/compiler.hpp
include/tinydbms/core.hpp
include/tinydbms/storage.hpp
```

`diagnostic.hpp` 存放所有模块共享的诊断类型（`SourceRange`、`FixIt`、`CompileStage`）。
`src/include/tinydbms/{compiler,core,storage,types}.h` 只是兼容转发头文件，不是并行 API，仓库内
没有代码再包含它们。

字段级契约见 [docs/消息契约详细设计.md](docs/消息契约详细设计.md)，模块边界见 [docs/模块交互契约.md](docs/模块交互契约.md)，当前冻结决策见 [docs/技术决策.md](docs/技术决策.md)。

设计与契约文档入口：

- [升级路线图（U/P 系列候选与状态）](docs/升级路线图.md)
- [core 与 CLI 实现设计](docs/core-cli/实现设计.md)
- [第二阶段执行器设计](docs/core-cli/第二阶段执行器设计.md)
- [第三阶段 CLI 与入口设计](docs/core-cli/第三阶段CLI与入口设计.md)
- [CLI 升级设计：JSON 输出（U2，已实现）](docs/core-cli/CLI升级设计_JSON输出.md)
- [CLI 升级设计：Plan 整理输出（U3，已实现）](docs/core-cli/Plan整理输出设计.md)
- [CLI 升级设计：结果集上限与分页（U4，已实现第一阶段）](docs/core-cli/结果集上限与分页设计.md)
- [CLI 升级设计：运行中取消（U5，已实现第一阶段）](docs/core-cli/运行中取消设计.md)
- [进程内并发设计（U6，已实现第一阶段）](docs/core-cli/进程内并发设计.md)
- [高级诊断与脚本恢复升级设计（已落地）](docs/core-cli/高级诊断与脚本恢复升级设计.md)
- [展示格式与性能观测设计（P1/P2 已实现，P3 待 storage 提供统计接口）](docs/core-cli/CLI展示格式与性能观测设计.md)
- [查询执行优化设计（P4，第一阶段已实现）](docs/core-cli/查询执行优化设计.md)
- [联调准备与验收清单](docs/联调准备与验收清单.md)
- [GUI 设计（Qt6 可选前端）](docs/gui/GUI设计.md)
- [Storage 契约](docs/storage/storage-contract.md)
- [SQL v2 Release Notes](docs/SQLv2_RELEASE_NOTES.md)

## 构建与验证

本项目必须使用 g++。默认构建（`TINYDBMS_ENABLE_REAL_MODULES=OFF`，走不可用 Session 适配器，
验证参数、I/O 边界与全部单测）：

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
./build/debug/src/app/tinydbms --help
```

真实 compiler + core + storage 构建：

```bash
cmake -S . -B build/real-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DTINYDBMS_ENABLE_REAL_MODULES=ON
cmake --build build/real-debug
ctest --test-dir build/real-debug --output-on-failure
```

真实模块构建下的 `tinydbms.real_modules_integration` 使用临时目录自动验证
`CREATE TABLE -> INSERT -> SELECT -> DELETE -> close -> reopen -> SELECT`、批处理停止、
REPL 恢复、超大 SQL 边界、UTF-8 数据目录、编译与语义错误位置、storage 运行期错误和 open
失败路径。

该构建可以直接跑一段脚本（stdin 不是终端时按批处理执行）：

```bash
./build/real-debug/src/app/tinydbms --data-dir /tmp/tinydbms-demo <<'SQL'
CREATE TABLE students (id INT NOT NULL, name VARCHAR);
INSERT INTO students VALUES (1, 'ada');
SELECT * FROM students;
SQL
```

可选的 Qt6 GUI（默认不参与构建，只有本机装了 Qt6 时才有意义）：

```bash
cmake --preset gui
cmake --build build/gui-debug --target tinydbms-gui tinydbms_gui_ui_test
./build/gui-debug/src/gui/tinydbms-gui
ctest --test-dir build/gui-debug -L gui --output-on-failure
```

GUI 与 CLI 一样只调用 core 的公开 API，`TINYDBMS_BUILD_GUI` 默认为 `OFF`，未开启时不会查找
Qt、也不会新增目标或测试。未启用 `TINYDBMS_ENABLE_REAL_MODULES` 时 GUI 使用不可用后端：窗口
可以打开与调试，但不会伪造数据。接口与线程模型见
[docs/gui/GUI设计.md](docs/gui/GUI设计.md)。

并发的 ThreadSanitizer 回归（预设与 `debug` 相同，只是追加 `-fsanitize=thread`）：

```bash
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan --output-on-failure
```

Storage 的实验 benchmark 默认不构建（`TINYDBMS_BUILD_STORAGE_BENCHMARKS=OFF`），需要时显式打开：

```bash
cmake -S . -B build/bench -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DTINYDBMS_BUILD_STORAGE_BENCHMARKS=ON
cmake --build build/bench --target tinydbms_io_benchmark tinydbms_prefetch_benchmark
```

开发时参考 [docs/开发守则.md](docs/开发守则.md) 和 [docs/miniob-study/](docs/miniob-study/) 的分层与调用链，不复制其事务、日志或多引擎范围。
