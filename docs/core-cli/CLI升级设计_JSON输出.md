# CLI 升级设计：JSON 输出

状态：**已实现**（2026-09-15）。对应 [升级路线图](../升级路线图.md) 的 U2。
实现位置：`src/app/arguments.{hpp,cpp}`（`--format` 解析）、`src/app/json_output.{hpp,cpp}`
（行对象与转义）、`src/app/output.{hpp,cpp}`（格式分派，table 分支保持原实现）、
`src/app/runner.cpp`（诊断与结果的格式透传）。本文第 5、6 节是 JSON 输出的稳定契约；
入口参数与展示差异另见 [第三阶段 CLI 与入口设计](第三阶段CLI与入口设计.md) 的第 3、6.3 节。

## 1. 范围

包含：

- `--format table|json` 参数与校验；
- JSON 模式下的结果与诊断输出格式（schema、类型映射、转义、流分离）；
- 与现有退出码、错误策略、提示符行为的兼容约束；
- 测试设计。

不包含：

- 跨行 SQL（U1 已舍弃，记录见附录 A）；
- core 返回结构、执行语义、分页与取消（分别对应 U4、U5）；
- GUI 的展示改造（GUI 直接调用 core，可复用同一 schema，但不属于本次入口工作）。

## 2. 现状基线

| 位置 | 现状 |
| --- | --- |
| `src/app/arguments.hpp` | app 私有 `ParsedArguments{action, data_dir, error_policy}`，参数错误统一退出码 2 |
| `src/app/output.cpp` | 固定制表符文本；VARCHAR 用反斜杠转义成单行；诊断写 stderr |
| `src/app/runner.cpp` | 批处理一次 `execute_script`；REPL 逐行调用；两处共用同一展示逻辑 |
| `include/tinydbms/core.hpp` | `ExecuteScriptResult{script_error, statements}`；`QueryResult{columns, rows}`；`CommandResult{affected_rows, error?}`；`Error{kind, compile_stage?, source?, message, suggestion?, fix_it?}` |
| `include/tinydbms/common.hpp` | `Type{kInt, kVarchar, kBigInt, kDouble, kBoolean}`；`Value::data` 为 `monostate/int32/int64/double/bool/string` |
| `docs/core-cli/第三阶段CLI与入口设计.md` | 第 6.1 节规定"展示器只读结构化结果，不根据 message 文本推断错误种类"；第 6.2 节规定 stdout 只放真实结果、诊断写 stderr |

MiniOB 对照（依据 `docs/miniob-study/`）：MiniOB 的结果经 `SqlResult` 由 communicator 序列化
（plain / MySQL packet），协议层承担了机器可读输出的职责。tinydbms 没有网络协议，
JSON 就是最小的机器可读输出层，因此放在入口而不是新增协议模块，属于简化后的分工差异。

## 3. 参数

- 新增 `--format table|json`，默认 `table`，最多出现一次。
- 缺值、空值、未知值、重复出现均按参数错误处理，退出码 2（与 `--data-dir`、`--error-policy` 同规则）。
- `--help` 文本同步更新；解析结果放进 app 私有的 `ParsedArguments`，不进入 core。
- 不支持 `--format=json` 这类等号写法，与现有参数风格保持一致。

## 4. 输出模型

- **NDJSON**：每行一个 JSON 对象，行内不含换行；一次脚本调用的结果按语句顺序逐行输出。
- **流分离不变**：stdout 只输出结果对象（`query` / `command`），stderr 只输出诊断对象
  （`error` / `status`）。这样 `2>/dev/null` 的既有用法仍然有效，也满足第 6.2 节的流向约束。
- 交互 REPL 的提示符仍写 stderr，属于面向人的文本；JSON 消费者只解析 stdout。
- 退出码、`--error-policy`、批处理/REPL 的继续或停止规则完全不变。
- JSON 模式不改变结果顺序，也不改变"部分成功的 `CommandResult` 先输出已完成行数、再输出错误"的顺序。

## 5. 行对象 schema

结果对象：

```json
{"type":"query","statement_index":0,"range":"1:1-1:19","columns":[{"name":"id","type":"INT"}],"row_count":2,"rows":[[1],[2]]}
{"type":"command","statement_index":1,"range":"1:1-1:30","affected_rows":3}
```

诊断对象：

```json
{"type":"error","scope":"statement","statement_index":0,"kind":"compile","stage":"syntax","range":"1:8-1:9","message":"...","suggestion":"...","fix_it":{"range":"1:8-1:9","replacement":"x"}}
{"type":"error","scope":"script","kind":"compile","message":"..."}
{"type":"status","statement_index":2,"status":"analyzed","range":"1:1-1:10"}
{"type":"status","statement_index":3,"status":"skipped","range":"1:1-1:10","reason":"policy"}
{"type":"status","statement_index":4,"status":"indeterminate","range":"1:1-1:10"}
```

固定规则：

- `type` 恒为第一个字段，其余字段顺序固定，便于 golden 测试。
- `range` 使用与 table 格式相同的 `line:column-line:column` 半开区间字符串；无来源时省略该字段。
- `suggestion`、`fix_it` 仅在存在时出现；`fix_it` 是对象，不再拆成 table 格式的 `FIX` 行。
- `status` 取自 `kAnalysisOnly` / `kSkippedExecution` / `kExecutionIndeterminate` 三个状态；
  `kSkippedExecution` 的 `reason` 为 `policy` 或 `aborted`，与 table 格式一致。
- `kExecuted` 与 `kPlanOnly`（U3 计划模式）都输出 `query`/`command` 结果对象：
  计划模式的结果是普通的单列 `plan` 查询，不为它增加 schema 字段。
- 语句级错误带 `statement_index`；`script_error` 不带。
- `statement_index` 与 `QueryResult`/`CommandResult` 的对应关系沿用 core 返回的
  `StatementResult.statement_index`，入口不重新编号。
- 实现的字段顺序与本节示例逐字段一致（`type` 恒为第一个字段），由
  `tests/app_cli_test.cpp` 的 golden 文本断言固定。

## 6. 类型映射与转义

| 类型 / 值 | JSON 表现 |
| --- | --- |
| `Type::kInt` / `kBigInt` | `"INT"` / `"BIGINT"` |
| `Type::kVarchar` | `"VARCHAR"` |
| `Type::kDouble` | `"DOUBLE"` |
| `Type::kBoolean` | `"BOOLEAN"` |
| `monostate`（NULL） | `null` |
| `int32_t` / `int64_t` | JSON number（BIGINT 按 64 位整数输出；以 double 解析的消费者超过 2^53 会丢精度，见第 10 节） |
| `double` | JSON number；非有限值（NaN/Inf）输出 `null`，因为 JSON 没有对应字面量 |
| `bool` | `true` / `false` |
| `std::string` | JSON string，按 JSON 规则转义 `"`、`\` 与控制字符 |

- JSON 模式**不使用** table 模式的反斜杠转义（那是为单行制表符文本设计的）。
- 输出按 UTF-8 原样透传；遇到非法 UTF-8 字节序列替换为 U+FFFD，保证输出始终是合法 JSON。
- 不输出 schema 之外的字段（例如不附带诊断文本），避免消费者依赖非契约内容。
- `rows` 长度必须等于 `row_count`，两者由同一份数据生成，不允许分叉。

## 7. 参数错误与生命周期错误

- 参数错误：`--format` 是否生效取决于解析是否成功，因此保持纯文本 + 退出码 2
  （已确定；只有解析成功后的 open/close、执行与输入错误才按请求格式输出）。
- open / close 错误：格式已知，按 `error` 对象输出到 stderr，`scope` 为 `script`。
- 与 table 模式相同，任何 stdout/stderr 写入失败都标记为 I/O 错误并返回 1。

## 8. 测试设计

不依赖真实 compiler/storage：

- 每个结果类型与每个诊断类型的 golden 文本；
- 转义边界：引号、反斜杠、换行、制表符、控制字符、UTF-8 多字节字符、NULL、非有限 double；
- 顺序断言：语句顺序、`script_error` 插在首条 `kSkippedExecution` 之前、部分成功 command + error 的顺序；
- stdout/stderr 分流、退出码与 `--error-policy` 在 JSON 模式下不变；
- `--format` 的缺值、空值、未知值、重复值四种参数错误；
- 测试内使用极简 JSON 解析校验器（不引入第三方依赖），确认每行可解析、字段类型正确、
  `rows` 与 `row_count` 一致。

真实链路：在 `tinydbms.real_modules_integration` 中增加一次 `--format json` 的 CREATE/INSERT/SELECT 批处理，
确认输出可被逐行解析且不含提示符文本。

## 9. 实施顺序与验收

1. 扩展 `ParsedArguments` 与参数解析（含 help 文本）；
2. 抽出展示层的格式选择接缝，table 分支保持现有实现不动；
3. 实现 JSON 行对象生成与转义；
4. 补齐 golden 与解析校验测试；
5. JSON 输出格式契约转正（本文第 5、6 节为稳定 schema，第 10 节为决策记录），并更新
   [第三阶段 CLI 与入口设计](第三阶段CLI与入口设计.md) 的参数、展示与测试小节。

验收标准：

- 不带新参数时行为与现状完全一致（输出、退出码、stderr 归属、提示符）；
- 每行输出均可被标准 JSON 解析器解析，stdout 不含提示符与诊断文本；
- `--format` 的四种非法用法均返回退出码 2；
- 公共头文件零修改（本项不涉及 `include/`）。

验收证据（2026-09-15）：

- `tests/app_cli_test.cpp`：query/command/error/status 的 golden 文本（字段顺序逐字符断言）、
  转义边界（引号、反斜杠、换行、制表符、控制字符、非法 UTF-8、NULL、NaN/Inf、BIGINT）、
  顺序（script_error 插在首条 `kSkippedExecution` 之前、部分成功 command + error）、
  stdout/stderr 分流、REPL 提示符只写 stderr、open/close 错误的 JSON 形状、四种 `--format` 参数错误；
- `tests/json_check.hpp`：测试内置的极简 JSON 解析器（覆盖 RFC 8259 语法、`\uXXXX` 与代理对），
  用来确认每行可解析并取回字段做类型与 `rows`/`row_count` 一致性断言；
- `tests/real_modules_integration_test.cpp`：真实 compiler/storage 上跑 CREATE/INSERT/SELECT
  的 `--format json` 批处理，逐行解析并断言 stdout 不含提示符。

## 10. 已定稿决策

1. **BIGINT 输出 JSON number**，保持 `rows` 与 `columns` 的类型一致性，不按数值大小切换表示。
   代价与缓解写进契约：JSON 规范允许任意精度整数，但以 double 解析的消费者（JavaScript、jq）
   在超过 2^53 时会丢精度，这类消费者应把 BIGINT 当不透明文本处理，或改用 table 格式。
   若将来确有必要，再增加 `--json-bigint=number|string` 之类的开关，本次不做。
2. **字段命名统一 snake_case**，与 C++ 成员命名和常见 JSON 约定一致；不引入驼峰别名。
3. **保留 `row_count`**：每行对象自包含，消费者可提前分配并做结构性校验（必须与 `rows` 长度一致）。
4. **不承诺 GUI 复用**：schema 转正为独立的入口契约文档；GUI 目前直接调用 core，不需要这层，
   将来若增加 IPC 或工具链复用，再按该契约实现。

## 附录 A：已舍弃项 U1（跨行 SQL）的记录

**结论：舍弃（2026-09-15）。** 为一个入口体验提升去改公共头并拉上 compiler 的 parser 分类，
收益不抵成本。契约里"跨行 SQL 为后期优先升级项"的表述可以保留为未实现能力。

舍弃依据来自对当前 compiler 的实测（`split_statements` + `compile` 逐项探测）：

| 输入 | `split_statements` | 后续 `compile` |
| --- | --- | --- |
| `SELECT 1`（无分号） | 1 条语句，`sql` 不含分号 | 正常进入编译 |
| `SELECT 1;` | 1 条语句，`sql` 含分号 | 正常进入编译 |
| `SELECT 'abc` | 1 条语句，**不报错** | `kLex` unterminated string literal |
| `SELECT 'a;` | 1 条语句，文本以 `;` 结尾 | `kLex` unterminated string literal |
| `SELECT 1; -- note` | 1 条语句，注释不在语句范围内 | 正常进入编译 |
| `SELECT 1 /* note` | 1 条语句，**不报错** | `kLex` unterminated block comment |
| `SELECT * FROM (` | 1 条语句，**不报错** | `kSyntax` expected table identifier after FROM |
| 空输入 / 纯注释 | 0 条语句 | — |

推论：

- "输入未结束"的状态只存在于 `split_statements` 内部（`state == kInString || kInBlockComment`），
  当前接口既不报错也不暴露，无法在不改 compiler 的前提下拿到。
- "末尾是分号就完整"被 `SELECT 'a;` 证伪；"尾部还有非空白文本就是没写完"被 `SELECT 1; -- note` 证伪；
  括号或半句未写完时只拿到普通语法错误。
- 若将来重新提案，最小改动是让 compiler 暴露该状态（不改 parser 的话，括号续行仍然做不到）。
