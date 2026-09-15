# tinydbms

`tinydbms` 是一个单进程、单线程的教学数据库。当前已经具备从 SQL 文本到页式持久化存储的完整模块边界和最小执行路径。

## 当前状态

- `app` 提供 `--help`、`--version`、`--data-dir`、REPL 和 stdin 批处理入口。
- `compiler` 提供分句、词法、语法、语义、优化和 Plan 生成。
- `core` 提供 `Database` 生命周期、Catalog 恢复、TableId 分配和脚本顺序执行，源码按 lifecycle、script、executor、expression 拆分。
- `storage` 提供 typed heap storage、buffer pool、record/page 编解码、cursor，以及以 System Catalog 为 schema 权威的持久化元数据。
- 默认构建仍通过不可用 Session 适配器验证 CLI 边界；完整 SQL 链路需要启用 `TINYDBMS_ENABLE_REAL_MODULES=ON`。

## 当前架构

```text
App
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
SQL NULL 由 `Value` 的 `std::monostate` 表示；列元数据带 `nullable`，默认 NOT NULL。
`VARCHAR` 必须是合法 UTF-8 且不超过 1024 字节，单行逻辑载荷不超过 4096 字节。
逻辑类型与物理编码的对应关系以
[docs/SQLv2扩展契约.md](docs/SQLv2扩展契约.md) §10 与 `include/tinydbms/common.hpp` 为准。

SQL v2 明确不包含：`HAVING`、`DISTINCT`、`AS` 别名、外连接、子查询、`UNION`、`LIMIT`/`OFFSET`、
窗口函数、算术表达式、事务、WAL 与崩溃原子性。

`storage.meta` 是 V2 bootstrap，不保存用户 schema。`tdb_sys_tables`（TableId 0）与
`tdb_sys_columns`（TableId 1）是 Storage 管理的特殊 HeapTable，保存用户 schema；它们可由
`SELECT` 读取，但普通 `CREATE`、`INSERT` 与 `DELETE` 不能修改。普通用户表从 TableId 2 开始。

当前未实现 FSM、Index、WAL、Transaction、MVCC、复杂 SQL，以及多个数据库并发打开。一个进程中可以创建多个 `Database` 对象，但 Storage 是 singleton，同一时刻最多一个对象处于 open 或 cleanup-pending 状态。

CLI 入口选项：`--data-dir DIR`、`--error-policy stop|analyze`、`--format table|json`
（JSON 为每行一个对象的 NDJSON，stdout 只放结果、stderr 只放诊断）、`--plan`
（只编译并打印执行计划，零副作用）、`--max-rows N`（单条语句在内存中物化的最大行数，
缺省为 `kMaxQueryRows` = 262144，`0` 与非法值按参数错误处理）。`--format`、`--plan`
与 `--max-rows` 可组合，行为契约见
[docs/core-cli/第三阶段CLI与入口设计.md](docs/core-cli/第三阶段CLI与入口设计.md)。

运行中取消：CLI 在 `execute_script` 期间按 Ctrl+C 会请求取消（`CancelToken`），当前语句在
下一个无副作用检查点结束、剩余语句不再执行并记为 `CANCELLED`，本次调用退出码为 1；
空闲期或第二次 Ctrl+C 按默认处置终止进程。GUI 侧只登记该能力边界，本版不提供取消按钮。
规格见 [docs/core-cli/运行中取消设计.md](docs/core-cli/运行中取消设计.md)。

## 公共契约

正式公共入口是：

```text
include/tinydbms/common.hpp
include/tinydbms/compiler.hpp
include/tinydbms/core.hpp
include/tinydbms/storage.hpp
```

`src/include/tinydbms/*.h` 只是兼容转发头文件，不是并行 API。

字段级契约见 [docs/消息契约详细设计.md](docs/消息契约详细设计.md)，模块边界见 [docs/模块交互契约.md](docs/模块交互契约.md)，当前冻结决策见 [docs/技术决策.md](docs/技术决策.md)。

core/CLI 的阶段设计入口：

- [升级路线图（U1–U6 候选与状态）](docs/升级路线图.md)
- [core 与 CLI 实现设计](docs/core-cli/实现设计.md)
- [第二阶段执行器设计](docs/core-cli/第二阶段执行器设计.md)
- [第三阶段 CLI 与入口设计](docs/core-cli/第三阶段CLI与入口设计.md)
- [CLI 升级设计：JSON 输出（U2，已实现）](docs/core-cli/CLI升级设计_JSON输出.md)
- [CLI 升级设计：Plan 整理输出（U3，已实现）](docs/core-cli/Plan整理输出设计.md)
- [CLI 升级设计：结果集上限与分页（U4，设计中）](docs/core-cli/结果集上限与分页设计.md)
- [CLI 升级设计：运行中取消（U5，设计中）](docs/core-cli/运行中取消设计.md)
- [联调准备与验收清单](docs/联调准备与验收清单.md)
- [GUI 设计（Qt6 可选前端）](docs/gui/GUI设计.md)

## 构建与验证

本项目必须使用 g++。默认构建：

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

开发时参考 [docs/开发守则.md](docs/开发守则.md) 和 [docs/miniob-study/](docs/miniob-study/) 的分层与调用链，不复制其事务、日志或多引擎范围。
