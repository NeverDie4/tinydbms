# CLI 升级设计：Plan 整理输出

状态：**已实现**（2026-09-15）。对应 [升级路线图](../升级路线图.md) 的 U3。
实现位置：`include/tinydbms/core.hpp`（`ExecutionMode`、`StatementStatus::kPlanOnly`、
`StatementResult::plan_only`、`ExecuteScriptRequest::mode`）、`src/core/plan_text.{hpp,cpp}`
（core 私有渲染器）、`src/core/script.cpp`（计划模式分支）、`src/app/{arguments,output,json_output,runner}.*`
（`--plan` 与展示）。本文是计划模式的权威格式契约；入口参数与展示差异另见
[第三阶段 CLI 与入口设计](第三阶段CLI与入口设计.md) 的第 3、6 节。

## 1. 背景与范围

目标：让用户看到一条语句被编译成的执行计划，用于教学、演示和排错。

前提：**compiler 视为固定**——不改公共头、不扩 SQL 方言。因此本项不做 SQL 层
`EXPLAIN <statement>`（那需要 compiler 解析新语法并公开 plan 渲染函数，性质与已舍弃的 U1 相同）。

包含：

- core 的"计划模式"：只编译不执行，把 Plan 整理成文本；
- core 私有的 Plan 渲染器与稳定的文本格式；
- 入口开关 `--plan`，以及与 U2 `--format json` 的组合；
- 测试与验收标准。

不包含：

- SQL 方言扩展（不做 SQL 层 `EXPLAIN`）；状态表示新增 `kPlanOnly` 已按第 9 节定稿落地；
- 执行统计、代价估算、真实行数——本项目没有代价模型，输出里也不假造；
- GUI 的"查看计划"按钮（core 模式可直接复用，但不在本次范围）。

## 2. 现状基线

| 位置 | 现状 |
| --- | --- |
| `include/tinydbms/compiler.hpp` | `Plan` = `CreateTablePlan / InsertPlan / DeletePlan / UpdatePlan / QueryPlan` 的 variant，全部是公共类型 |
| 同上 | `QueryPlan{root, outputs}`；`PlanNode` = `SeqScanNode / FilterNode / JoinNode / AggregateNode / SortNode / ProjectNode`；`QueryOutput{slot_id, name, type, nullable}` |
| 同上 | 表达式 `Expr` = `ColumnRef{slot_id} / Literal{Value} / Binary{CmpOp或LogicOp} / Unary{kNot} / NullTest` |
| `src/compiler/plan_formatter.cpp` | `compiler::internal::format_plan` 只被 compiler 自己的测试使用，不是公共接口 |
| `src/core/executor.cpp` | 通过 `compiler::compile` 得到 Plan 后直接进入执行；core 已持有 Catalog，可解析 TableId/ColumnId |
| `include/tinydbms/core.hpp` | `ExecuteScriptRequest{text, error_policy}`；`QueryResult{columns, rows}`；CLI 不包含 compiler 公共头 |

MiniOB 对照（依据 `docs/miniob-study/`）：MiniOB 在 Physical Operator 上提供 description 之类的
计划描述能力，`explain` 属于 SQL 层功能。tinydbms 没有独立的 Physical Operator 层，
compiler 产出的 `Plan` 就是执行形状，所以"整理输出"直接渲染 compiler 的 Plan，
由 core 承担渲染职责。与 MiniOB 的差异源于层级数量和"不改 compiler"的约束。

## 3. 计划模式

### 3.1 请求级模式

```cpp
// include/tinydbms/core.hpp
enum class ExecutionMode {
    kExecute,    // 默认：编译并执行，行为与现状一致
    kPlanOnly    // 只编译，返回整理后的计划文本，不产生任何副作用
};

struct ExecuteScriptRequest {
    std::string text;
    ScriptErrorPolicy error_policy{ScriptErrorPolicy::kStopOnFirstError};
    ExecutionMode mode{ExecutionMode::kExecute};
};
```

选择请求级模式而不是新方法，理由与入口改造保持一致：单一执行入口、复用分句与错误机制、
默认值保证现有调用方（批处理、GUI、测试）零改动。

### 3.2 语义

- `kPlanOnly` 下 core 仍然分句、编译（含语义检查），但**不进入执行器**；语句状态为
  `StatementStatus::kPlanOnly`（已定稿，见第 9 节），与 `kExecuted` 一样携带结果，
  但语义是"计划已生成"而不是"语句已执行"。
- 计划模式不产生 `kAnalysisOnly`/`kAnalysisError`：不进入执行器就不需要影子 Catalog 的
  "能否应用"判定；`--error-policy analyze` 下首错之后编译成功的语句照样输出计划并记
  `kPlanOnly`（已由 `tests/core_plan_text_test.cpp` 固化）。
- 每条语句返回 `QueryResult`：单列 VARCHAR，列名固定为 `plan`，每个元素是一行计划文本。
- 编译错误仍返回 `kCompileError`，语义错误位置与 table 模式的规则完全一致。
- 分句失败、语句数超限、致命中止等 `script_error` 规则不变。
- **不产生副作用**：`CREATE TABLE t(...); SELECT * FROM t;` 在同一脚本里，后一句会因表不存在而编译失败。
  这是期望行为，必须写进文档，不做特殊处理。
- 计划模式不需要 storage 参与；但入口仍按现有生命周期 open/close，行为不变。

## 4. 计划文本格式

### 4.1 总原则

- 一次输出多条语句的结果，每条语句的计划是若干行文本，行序即输出顺序。
- 两空格缩进一层，字段顺序固定，同一 Plan 的渲染结果稳定（供 golden 测试）。
- 编号尽量解析成人名：结合 catalog 把 `TableId` 解析成表名、`ColumnId` 解析成列名；
  确实解析不到时退化打印原始编号（例如 `table#3`、`slot#5`）。
- slot 名称映射的来源有两处：`SeqScanNode.columns`（列 → slot）与 `QueryPlan.outputs`
  （输出 slot → 名字与类型）。渲染前先收集映射，再做遍历。

### 4.2 表达式渲染

| 节点 | 渲染 |
| --- | --- |
| `ColumnRef` | `<列名>#<slot>`；解析不到列名时退化为 `slot#<slot>`（例如 `slot#5`） |
| `Literal` | INT/BIGINT 十进制；DOUBLE 用最短往返表示；VARCHAR 用单引号包裹并转义内部单引号；BOOLEAN 用 `TRUE`/`FALSE`；NULL 用 `NULL` |
| `Binary`（比较） | `(<lhs> = <rhs>)`，运算符为 `= <> < <= > >=` |
| `Binary`（逻辑） | `(<lhs> AND <rhs>)` / `(<lhs> OR <rhs>)` |
| `Unary` | `NOT (<operand>)` |
| `NullTest` | `(<operand> IS NULL)` / `(<operand> IS NOT NULL)` |

### 4.3 语句级计划

```text
CreateTable users(id INT NOT NULL, name VARCHAR)
Insert table=users columns=[id, name] rows=2
Delete table=users predicate=(id#0 = 1)
Update table=users assignments=[name = 'x'] predicate=ALL
```

规则：

- `CreateTable` 直接列出列名、类型与 `NOT NULL`；表还没有 TableId，保持 `table_name` 原样。
- `Insert` 的 `columns` 为空表示全列按建表列序，此时渲染成 `columns=[*]`；`rows` 输出行数。
- `Delete`/`Update` 的 `predicate` 为空时渲染成 `ALL`。

### 4.4 查询计划

以下三段是真实链路的输出（`--plan` + 真实 compiler，表为 `emp(id INT, dept INT, amount BIGINT, note VARCHAR)`
与 `dept(id INT, name VARCHAR)`）。

```text
-- SELECT * FROM emp WHERE id > 10;
QueryPlan outputs=[id:INT, dept:INT, amount:BIGINT, note:VARCHAR]
  Project [id#0, dept#1, amount#2, note#3]
    Filter (id#0 > 10)
      SeqScan emp
        columns=[id -> slot0, dept -> slot1, amount -> slot2, note -> slot3]
```

```text
-- SELECT dept, SUM(amount) FROM emp WHERE note IS NULL GROUP BY dept;
QueryPlan outputs=[dept:INT, SUM(amount):BIGINT]
  Project [dept#1, SUM(amount)#4]
    Aggregate group=[slot1] calls=[SUM(amount#2) -> slot4 BIGINT]
      Filter (note#3 IS NULL)
        SeqScan emp
          columns=[id -> slot0, dept -> slot1, amount -> slot2, note -> slot3]
```

```text
-- SELECT dept.name, emp.note FROM dept JOIN emp ON dept.id = emp.dept;
QueryPlan outputs=[name:VARCHAR, note:VARCHAR]
  Project [dept.name#1, emp.note#5]
    Join kind=INNER condition=(dept.id#0 = emp.dept#3)
      left:
        SeqScan dept
          columns=[id -> slot0, name -> slot1]
      right:
        SeqScan emp
          columns=[id -> slot2, dept -> slot3, amount -> slot4, note -> slot5]
```

- `Sort`：`Sort keys=[slot1 ASC, slot2 DESC]`，随后缩进子节点。排序键与聚合的分组键/输出槽位
  是结构性 slot 引用，固定写成 `slotN`；只有表达式位置才做名字解析。
- `Aggregate`：`Aggregate group=[...] calls=[<聚合名>(<表达式或 *>) -> slotN <TYPE>]`，
  聚合输入槽位走表达式渲染（`amount#2`），输出槽位写成 `slotN`。
- `Project`：`Project [slot 名字列表]`，随后缩进子节点。每个条目按 `<名字>#<slot>` 渲染，
  名字优先取扫描列（多表时带表名限定），否则取 `QueryPlan.outputs` 的 `name`，
  两者都取不到时退化为 `slot#N`。
- 多表场景（算子树里有 2 个及以上 `SeqScan`）列名前加表名限定（`dept.name`），
  单表场景只写列名；限定名只用于扫描列解析出的槽位。
- `QueryPlan.outputs` 的名字原样来自 compiler 的 `QueryOutput.name`：普通列是列名，聚合输出是
  `SUM(amount)`、`COUNT(*)` 这类形状，因此多表查询里 `outputs=` 仍可能与兄弟列同名
  （`[name:VARCHAR, note:VARCHAR]`）。这是 compiler 给出的名字，core 不改写。
- 该格式由 core 私有渲染器产出，属于本项目自有的展示契约，与 compiler 内部
  `format_plan` 无关；两者格式不同是接受的结果。

## 5. 入口行为

- CLI 新增 `--plan`：整个调用进入计划模式；缺值、重复、与 `--help/--version` 混用等按现有参数
  规则处理（退出码 2）。`--plan` 不带参数，因此不存在"值未知"的错误类型。
- 展示：table 格式下按普通 `QueryResult` 打印（列头 `plan`，每行一行文本）；
  `--format json` 下按 U2 的 `query` 对象输出，不新增 schema 字段。
- 退出码、`--error-policy`、批处理/REPL 的继续与停止规则完全不变。
- REPL 下 `--plan` 对整个会话生效（每行都只输出计划）。

## 6. 与 U2 的关系

- `--plan` 决定 core 的模式，`--format` 决定展示格式，两者正交，可任意组合。
- JSON 消费者拿到的是普通的 `query` 对象，列名 `plan`，无需为计划输出增加新字段。

## 7. 测试设计

core 与渲染器：

- 每种 Plan（CREATE/INSERT/DELETE/UPDATE/QUERY）的 golden 文本；
- 每种 PlanNode 与每种 Expr 节点的 golden 文本，含 `NULL`、布尔、字符串转义、DOUBLE；
- slot 名称解析成功与失败两条路径（构造一个无法解析的 slot，断言退化为编号）；
- 计划模式不产生副作用：同一脚本 `CREATE TABLE` + `SELECT` 的第二句返回 `kCompileError`；
- 编译错误、语义错误位置在计划模式下与执行模式一致。

CLI：

- `--plan` 的参数错误与帮助文本；
- table 与 JSON 两种展示；批处理与 REPL 行为一致；
- 退出码不变；提示符仍写 stderr。

真实链路：`--plan` 跑一次 CREATE/SELECT/JOIN/聚合脚本，确认没有新建表文件、没有写入数据页
（用临时目录前后对比断言）。

## 8. 实施顺序与验收

实施顺序（已完成）：

1. 在 `core.hpp` 增加 `ExecutionMode`、`StatementStatus::kPlanOnly`、`StatementResult::plan_only`；
2. `src/core/script.cpp` 在编译成功后分流到计划模式，不进入执行器、不更新影子 Catalog；
3. 实现 core 私有渲染器 `src/core/plan_text.{hpp,cpp}`（含 catalog 名称解析与退化规则、
   Plan 契约校验）；
4. 补齐 core golden 测试与"无副作用"测试（`tests/core_plan_text_test.cpp`）；
5. 入口增加 `--plan` 并在 U2 的展示层下复用（`src/app/*`）；
6. 更新 [第三阶段 CLI 与入口设计](第三阶段CLI与入口设计.md) 的参数与展示小节，以及
   [消息契约详细设计](../消息契约详细设计.md) 中 `ExecuteScriptRequest` 与状态表的说明。

验收标准：

- 默认模式行为零变化；
- 计划模式零副作用，且同一 Plan 的渲染结果稳定；
- 不改 compiler 任何文件（`src/compiler/`、`include/tinydbms/compiler.hpp` 均不动）；
- `include/tinydbms/core.hpp` 的修改已登记并通知其他成员。

验收证据（2026-09-15）：

- `tests/core_plan_text_test.cpp`：每种 Plan 与每个 PlanNode/Expr 的 golden 文本、slot 名称
  解析失败退化、Plan 契约违反返回 `kInternal`、计划模式不调用 Storage、编译错误位置与执行模式一致；
- `tests/app_cli_test.cpp`：`--plan` 的参数错误与 mode 透传（批处理与 REPL）、kPlanOnly 在
  table 与 JSON 两种格式下的展示；
- `tests/real_modules_integration_test.cpp`：真实 compiler/storage 上跑 CREATE/SELECT/聚合/JOIN
  计划，用临时目录前后文件清单（含文件大小）与后续数据查询断言零副作用；
- 全套 CTest 通过（fake 模块 56 项、`TINYDBMS_ENABLE_REAL_MODULES=ON` 58 项）。

## 9. 未决问题

1. **状态表示**：**已定稿**——新增 `StatementStatus::kPlanOnly`。`kExecuted` 的契约含义是
   "语句进入执行器并产生了结果"，计划模式没有执行，复用会让调用方把零副作用的语句当成成功执行
   （与 [公共契约升级设计](../sql-v2/公共契约升级设计.md) 的"不得伪装成成功执行"冲突）。
   代价是状态消费点（CLI 展示、GUI 展示、测试辅助）都要新增分支，已随本次实现一起更新。
2. **开关命名**：**已定稿**——`--plan`。`--explain` 更贴近业界习惯，但本项目没有 SQL 层 EXPLAIN，
   `--plan` 与实际行为（输出编译出的执行计划）一致，也不会与未来可能的 `EXPLAIN` 语法争名字。
3. **是否提示"这是计划而非执行结果"**：**已定：不额外提示**。输出形状本身就是证据——列名固定为
   `plan`，内容是 `QueryPlan`/`CreateTable` 这样的计划文本；`--help` 里说明 `--plan` 只编译不执行。
4. **REPL 元命令**：不做。契约禁止 SQL 之外的元命令，`--plan` 对整个会话生效已经覆盖演示需求。
5. **多表列名限定规则**：**已定：按扫描数量判定**——算子树里有 2 个及以上 `SeqScan` 时加表名前缀，
   与是否 JOIN 解耦（当前 compiler 只产生 JOIN 这种多表形状，判定方式对将来的多表扩展同样成立）。
