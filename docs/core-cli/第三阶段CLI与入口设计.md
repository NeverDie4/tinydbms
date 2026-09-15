# 第三阶段 CLI 与入口设计（实现规格）

本文规定本分支 app/CLI 的实现范围。compiler 和 storage 由其他模块负责人实现，
本分支不实现它们的词法、语法、文件格式或物理存储逻辑；CLI 只通过 core 的公开
Database API 工作。

本文与 [core 与 CLI 实现设计](实现设计.md)、[消息契约详细设计](../消息契约详细设计.md)
和 [模块交互契约](../模块交互契约.md) 一起作为入口实现依据。

## 1. 阶段目标与边界

### 1.1 本阶段目标

- 完成命令行参数解析；
- 支持交互式 REPL 和 stdin 批处理；
- 正确管理 Database 的 open、execute_script、close 生命周期；
- 展示 core 返回的 QueryResult、CommandResult 和 Error；
- 固定参数错误、SQL 错误、输入输出错误和 close 错误的退出码；
- compiler 与 storage 均已提供可链接实现；保留真实 compiler + fake storage 的窄联调，
  并通过产品级测试完成真实链路联调。

### 1.2 不属于本阶段

- SQL 分句、解析、语义检查或 Plan 构造；
- 表、记录、游标、文件和持久化实现；
- 在 app 中直接调用 compiler/storage；
- 事务、并发、跨行 SQL、SQL 元命令和复杂格式化；
- 为了测试 CLI 而向 core.hpp 增加测试专用入口。

## 2. 模块边界与文件组织

app 只负责四件事：解析参数、读取输入、调用 core、展示结果。任何表查找、SQL 判断、
错误映射和存储操作都不得放入 app；`ErrorKind` 到稳定 CLI 文本的直接映射属于展示职责，
不改变 core 的错误语义。

本阶段按下面的职责拆分，公共契约之外的头文件和实现均保留在 src/app/：

- main.cpp：组装依赖、选择 Session 适配器并返回最终退出码；
- arguments.cpp 与内部头文件：参数解析和 help/version 文本；
- input.cpp 与内部头文件：REPL/批处理输入读取；
- output.cpp 与内部头文件：结果与错误展示；
- terminal.cpp 与内部头文件：平台相关的 stdin 终端检测；
- session.hpp/session_core.cpp：app 私有的 `Session` 抽象和 `core::Database` 适配器；
- session_unavailable.cpp：真实链路尚未启用时的明确不可用适配器；
- runner.cpp 与内部头文件：串联参数、输入、Session、展示和最终退出码；
- tests/app_cli_test.cpp：参数、输入模式、格式化和生命周期策略测试。

这些内部头文件放在 src/app/，不安装到 include/tinydbms/。如果代码量仍然很小，可以
暂不拆文件，但职责边界必须保持不变。

app 只包含 tinydbms/core.hpp 和 app 自己的内部头文件，不包含 compiler.hpp 或 storage.hpp。
产品 `tinydbms` target 显式链接 `tinydbms_core`、compiler 和 storage。由于本分支的
compiler 与 storage 均已有可链接实现；默认构建仍使用 `UnavailableSession`，在真正执行数据库操作时
返回 `internal` 错误。这只用于保持入口、参数和退出码可以独立构建和验证，不伪装成可用的 SQL 实现。
以 `-DTINYDBMS_ENABLE_REAL_MODULES=ON` 构建时，入口使用 `CoreSession` 并接通真实
core/compiler/storage 链路。CLI 测试只链接 app 私有实现、core
公共类型和测试 Session，不把 fake/real 模块同时带入同一测试目标。

为了让参数、I/O 和退出码测试不依赖真实模块，`runner` 使用 app 私有的最小 Session 接缝：
`Session` 只转发 `open(OpenDatabaseRequest)`、`execute_script(ExecuteScriptRequest)` 和
`close()` 三个操作，生产环境由 `CoreSession` 持有 `core::Database`，测试使用返回预置
结构化结果的 `FakeSession`。该接缝不安装到 `include/tinydbms/`，也不改变 core 公共 API；
产品级测试仍需用真实 compiler/storage 验证完整链路。

Session 接缝的语义固定为：`open` 成功后由 runner 负责最多调用一次 `close`；`open` 失败时
不调用 `close`；`execute_script` 只在 open 成功后调用。FakeSession 必须记录调用顺序，便于
断言参数错误不创建会话、open 失败不 close、运行时失败仍 close。

其最小接口只使用 core 的公共类型：

```cpp
class Session {
public:
    virtual ~Session() = default;
    virtual tinydbms::core::OpenDatabaseResult open(
        const tinydbms::core::OpenDatabaseRequest&) = 0;
    virtual tinydbms::core::ExecuteScriptResult execute_script(
        const tinydbms::core::ExecuteScriptRequest&) = 0;
    virtual tinydbms::core::CloseDatabaseResult close() = 0;
};
```

启用真实模块时，`CoreSession` 是唯一接触 `core::Database` 的 app 适配器；默认占位构建使用
同一接口的 `UnavailableSession`。`runner`、参数解析、输入和展示均只依赖该 app 私有接口。
`CoreSession` 内部惰性创建 `Database`，因此 help/version 和参数错误路径不会构造或打开数据库。

构建组合也保持这条边界：`tinydbms_cli_logic` 包含 arguments、input、output、runner 和
terminal，不链接 compiler/storage；产品入口再把 `main.cpp`、一个 Session
适配器、该私有库与 `tinydbms_core`、compiler/storage 组合。`app_cli_test` 只链接私有库和
FakeSession，避免测试目标同时出现 fake 与真实模块。占位构建与真实构建只切换适配器源文件，
不改变 runner 的行为契约。

## 3. 参数解析

### 3.1 支持的参数

支持以下参数：

- --help：打印帮助并退出；
- --version：打印版本并退出；
- --data-dir DIR：指定 UTF-8 数据目录，最多出现一次；
- --error-policy stop|analyze：脚本错误策略，最多出现一次；默认 stop；
- --format table|pretty|json：结果展示格式，最多出现一次；未显式给出时按输出目标选择——
  stdout 是终端用 pretty，否则用 table；显式给出时以显式值为准（见
  [JSON 输出设计](CLI升级设计_JSON输出.md) 与
  [展示格式与性能观测设计](CLI展示格式与性能观测设计.md)）；
- --time：每次 `execute_script` 调用在 stderr 追加一行墙钟耗时，最多出现一次；默认关闭
  （见 [展示格式与性能观测设计](CLI展示格式与性能观测设计.md)）；
- --plan：整个调用进入计划模式，只编译并输出执行计划，最多出现一次（见 [Plan 整理输出设计](Plan整理输出设计.md)）；
- --max-rows N：单条语句在内存中物化的最大行数，最多出现一次；N 为 `1..SIZE_MAX` 的十进制
  整数，缺省使用 core 的 `kMaxQueryRows`（见 [结果集上限与分页设计](结果集上限与分页设计.md)）；
- 未提供 --data-dir 时，使用 ./tinydbms-data。

--help 或 --version 单独出现时立即成功退出，不创建 Database，也不访问 data_dir。
它们与其他参数混用时属于参数错误，不打开数据库。

### 3.2 参数错误

以下情况统一返回退出码 2，并输出简短错误和帮助提示：

- 未知选项；
- --data-dir 缺少值；
- --data-dir 后的值为空；
- --data-dir 后紧邻另一个 `--` 选项，未提供目录值；
- --data-dir 重复出现；
- --error-policy 缺少值、值为空、未知值或重复出现；
- --format 缺少值、值为空、未知值（非 table/pretty/json）或重复出现；
- --time 重复出现；
- --plan 重复出现；
- --max-rows 缺少值、值为空、非十进制数字（含前导 `+`/`-`、十六进制）、尾随字符、
  数值为 `0` 或超出 `size_t` 范围，以及重复出现；
- --help/--version 与其他参数混用。

不支持位置参数、短选项、`--data-dir=DIR` 或未列出的 `--` 变体；它们均按未知参数处理。

参数解析结果使用 app 私有的 ParsedArguments，不把命令行状态放进 core。
参数错误必须在 Database 构造或 open 之前发现。

### 3.3 跨平台路径

Linux 直接把 UTF-8 字符串传给 core。Windows 入口使用宽字符参数读取系统命令行，
转换为 UTF-8 后再填入 OpenDatabaseRequest；core 和 storage 只接收 UTF-8 字符串。

## 4. 输入模式

### 4.1 模式判定

- stdin 连接交互终端时进入 REPL；
- stdin 非交互时进入批处理；
- 模式判定只发生一次，不在 app 中解析 SQL；
- REPL 初版按行提交，跨行 SQL 直接作为两次独立输入，不自行拼接。

终端检测使用平台抽象函数，不让 Linux 专用系统调用扩散到结果展示和 core 代码。

### 4.2 批处理

批处理模式一次读取 stdin 全部文本，并调用一次 execute_script。

- 读取成功后把原文完整交给 core；
- 输入读取发生非 EOF 错误时返回退出码 1；
- core 返回的 statements 按顺序展示；script_error 插在首条 kSkippedExecution 之前；
- 默认 stop 策略在首错后停止剩余 SQL；analyze 策略下 core 继续静态分析但不执行，
  app 不自行执行剩余文本；
- 展示完成后调用一次 close，再根据 close 结果确定最终退出码。

### 4.3 REPL

REPL 循环使用 getline 读取一行，每读到一行调用一次 execute_script：

1. 读取一行；
2. 调用 core；
3. 按顺序展示 statements 与 script_error；
4. 结构化错误（编译/执行/分析）设置累计错误标记；任何 script_error 按致命处理：
   停止读取后续输入并进入 best-effort close；
5. 否则继续读取下一行。

读到 EOF 后退出循环并调用 close。非 EOF 的输入错误设置累计错误标记，但仍应尝试
执行 close。REPL 不提供 quit、help 等 SQL 之外的元命令。

TTY 下的提示符写到 stderr，批处理模式不得输出提示符，以免污染管道结果。
提示符固定为 `tinydbms> `，每次读取前输出并刷新 stderr；提示符写失败同样记为 I/O 错误。

## 5. Database 生命周期

参数解析成功且不是 help/version 后，main 创建 Session；真实 CoreSession 在 open 时才创建 Database：

    parse arguments
    construct Session
    Session::open(OpenDatabaseRequest)  # 真实适配器此时惰性创建 Database
    read and execute input
    Session::close()
    return exit code

open 失败时展示 Error 并返回 1，不进入输入循环。由于 core 的 open 失败路径已经负责
释放 storage 状态，app 不重复调用 close。

open 成功后，无论批处理、REPL、读取错误还是 execute_script 返回错误，退出前都必须调用
一次 close。close 的错误不能覆盖已经发生的参数错误；参数错误不会创建 Database。

app 不捕获后继续执行 core 内部异常；Database 公开 API 按契约不抛异常。若 Session 调用或
app 自身的输入输出操作抛出异常，转换为 stderr 错误并返回 1，同时对已经打开的 Session 做
best-effort close。`runner` 对 open 成功的 Session 保证最多调用一次 close；即使 execute、
输入或展示失败也必须走同一清理路径。core 在 storage 异常后进入 cleanup-pending 时，runner
仍照常调用一次 close 消费该状态。close 本身抛异常时 runner 不循环重试，
由 core 对 cleanup-pending 保留重试状态，并由后续显式 close 或 Database 析构做 best-effort 回收。

## 6. 结果展示

### 6.1 展示原则

展示器只读取结构化结果，不根据 message 文本推断错误种类，不重新解析 SQL。
同一 ExecuteScriptResult 中的 statements 保持原顺序；script_error 固定插在首条
`kSkippedExecution` 之前，批处理与 REPL 共用同一展示逻辑。

### 6.2 初版稳定格式

为了便于脚本测试，初版采用简单的制表符格式：

- `kExecuted` + CommandResult：stdout 输出 `OK `、affected_rows 和换行；
- `kExecuted` + QueryResult：第一行输出列名，以制表符分隔；后续每行输出对应 Value，以制表符分隔；
- 空结果仍输出列头，不额外输出“无结果”文本；
- INT 按十进制输出；
- VARCHAR 输出 UTF-8 文本；反斜杠、制表符、换行和回车分别编码为 `\\`、`\\t`、`\\n`、`\\r`，
  保证一行一个结果记录且不破坏制表符分隔；
- 范围格式固定为 `line:column-line:column`（半开区间，来自 SourceRange 的 begin/end）；
- 语句级错误（`kCompileError`/`kExecutionError`/`kAnalysisError`）：stderr 输出
  `ERROR <label> <range> <message>\\n`。label 固定为：编译错误的 `lex`/`syntax`/`semantic`
  （取自 CompileStage），以及 `execute`/`storage`/`analysis`；
- `script_error`：stderr 输出 `ERROR <kind> <range?> <message>\\n`，kind 为
  `compile`/`execute`/`storage`/`analysis`/`internal`；有 source 时输出范围，
  空插入点渲染为 `1:1-1:1`，无 source（moved-from/unopened）时省略范围；
- suggestion：`SUGGESTION <message>\\n`；
- fix-it：`FIX <range> <replacement>\\n`；
- `kAnalysisOnly`：stderr 输出 `ANALYZED <range>\\n`，不输出 OK 或查询行；
- `kSkippedExecution`：stderr 输出 `SKIPPED <range> policy|aborted\\n`；本次脚本存在
  script_error 时为 `aborted`，仅因策略跳过时为 `policy`；
- `kExecutionIndeterminate`：stderr 输出 `INDETERMINATE <range>\\n`；
- `kCancelled`：stderr 输出 `CANCELLED <range>\\n`，并把本次调用标记为失败（退出码 1）；
  JSON 模式下输出 `{"type":"status",...,"status":"cancelled",...}`，同样写 stderr；
- `kPlanOnly`：与 `kExecuted` + QueryResult 一样输出查询结果（列名固定为 `plan`），
  但它表示“计划已生成”而不是“语句已执行”，只在 `--plan` 下出现；
- 以上诊断文本（message/suggestion/replacement）使用与 VARCHAR 相同的单行转义。

CREATE TABLE 成功时 affected_rows 为 0。携带 error 的 CommandResult 先输出已完成的
affected_rows，再输出对应错误，并把本次入口状态标记为失败。

help 和 version 文本属于 CLI 自身输出，写入 stdout；参数错误和执行错误写入 stderr。
格式化实现不得修改 QueryResult 或 CommandResult 的所有权和内容；任何 stdout/stderr 写入
失败都标记为 I/O 错误并返回 1，不能把写入失败误判为 SQL 成功。

### 6.3 JSON 模式与计划模式

- `--format json` 切换到 NDJSON：stdout 只写结果对象（`query`/`command`），stderr 只写诊断对象
  （`error`/`status`）。行对象 schema、类型映射、转义与测试见
  [JSON 输出设计](CLI升级设计_JSON输出.md)；该 schema 已随实现转正为稳定契约。
- 参数错误与 `--help/--version` 输出保持纯文本：`--format` 是否生效取决于解析是否成功，
  只有解析成功后的 open/close、执行与输入错误才使用请求的格式。
- `--plan` 只改变 core 的执行模式，不改变展示层：table 模式下按普通 QueryResult 打印
  （列头 `plan`，每行一行计划文本），JSON 模式下按普通 `query` 对象输出，不新增 schema 字段。
- `--plan` 与 `--format` 正交，可任意组合；REPL 下 `--plan` 对整个会话生效。
- `--max-rows N` 只透传到 `ExecuteScriptRequest::max_query_rows`：批处理与 REPL 的每一行都使用
  同一个值，`--plan` 下该参数不生效但不报错。超限错误沿用既有通道，不新增 schema：
  table 模式输出 `ERROR execute <range> <message>` 与 `SUGGESTION ...` 行，
  JSON 模式在 `message` 之外附 `suggestion` 字段。

### 6.4 运行中取消（SIGINT）

- CLI 在 `main` 启动时安装 SIGINT 处理器：处理器只读取进程级"当前活动令牌"指针，非空且
  尚未请求取消时调用一次 `request_cancel()`，不做展示、不写流、不分配内存。
- 令牌生命周期：批处理在读取完 stdin 后构造一个令牌，REPL 每读取一行构造新令牌；
  令牌由 RAII guard 在 `execute_script` 调用前后设置/清除，因此空闲期按下的 Ctrl+C
  不会毒化后续执行，也不会让处理器读到悬垂指针。
- 指针为空（空闲期、读输入期间）或该令牌已经请求过取消（执行期间第二次 Ctrl+C）时，
  处理器恢复默认处置并重新触发 SIGINT，进程按信号默认处置终止（shell 观察到 130），
  不引入新的应用退出码。
- 首次 Ctrl+C 的效果：当前语句在下一个检查点结束并记 `kCancelled`，后续语句不再编译或
  执行，同样记 `kCancelled`；`script_error` 为空。REPL 继续读取下一行，最终退出码为 1；
  批处理按失败结束（退出码 1）。INSERT 首版不设检查点，因此长 INSERT 结束后才在语句
  边界生效。
- 取消的输出与 `SKIPPED`/`INDETERMINATE` 同流向（stderr），不改变 stdout 结果对象。

### 6.5 pretty 模式与 --time

- `--format pretty` 是**展示层**格式，不改 core 调用、不改退出码、不改 stderr 诊断文本：
  查询结果渲染成等宽边框表格并追加行数行，命令结果渲染成 `OK, N rows affected`；
  诊断与状态行与 table 模式字节级一致。
- pretty 输出不承诺机器可解析；需要稳定解析请用 `table` 或 `json`。
- 默认格式在 `runner` 层按 `CliEnvironment::output_is_terminal` 决定，终端用 pretty、
  非终端用 table；测试与 GUI 不设置该字段，因此既有 golden 测试全部走 table。
- `--time` 只写 stderr，stdout 字节流在开关前后完全一致；批处理 scope 为 `script`，
  REPL scope 为 `line <序号>`。

## 7. 退出码与错误策略

退出码固定为：

- 0：参数、输入、SQL 执行和 close 全部成功；
- 1：open/close 失败、SQL 编译/执行失败、分析错误、storage 错误、输入输出错误，
  以及任何 script_error（分句失败、语句数超限、致命中止）；
- 2：命令行参数错误。

`kAnalyzeRemaining` 中后续语句分析成功不能把退出码从 1 恢复为 0；`kAnalysisOnly`、
`kSkippedExecution`、`kExecutionIndeterminate` 行本身不改变退出码，因为致命中止必然伴随
`script_error`。`kCancelled` 例外：它不伴随 `script_error`，但取消是调用方的主动行为，
本次调用按失败处理（退出码 1）。

优先级如下：

1. 参数错误直接返回 2，不创建 Database；
2. 参数成功但 open 失败返回 1；
3. open 成功后，任何 execute_script、读取或展示错误都累计为失败；
4. close 失败也返回 1；
5. REPL 遇到结构化错误继续读取；遇到任何 script_error 停止读取后续输入并 best-effort close，
   EOF 或停止后按累计错误标记和 close 结果返回；
6. 批处理遇到错误不重复执行剩余 SQL，但仍展示 core 已返回的结果并执行 close。

## 8. CLI 测试设计

### 8.1 不依赖真实 compiler/storage 的测试

以下测试不依赖真实 compiler/storage：

- help/version 参数的成功路径；
- 未知参数、缺少值、重复 data_dir、空值和参数混用；--error-policy 的默认值、合法值、
  缺值、空值、未知值和重复值；
- 解析结果和默认 data_dir；
- 批处理与 REPL 的模式判定；
- 不输出 REPL 提示符的批处理路径；
- QueryResult、CommandResult 与九态 StatementStatus（含 U5 的 kCancelled）的 stdout/stderr
  分流，包括制表符/换行/反斜杠转义；kAnalysisOnly、kSkippedExecution 与 kCancelled
  不输出 OK 或查询行；
- `--format` 的缺值、空值、未知值、重复值四类参数错误，以及 `--plan` 的重复与混用参数错误；
- `--max-rows` 的缺值与空值、非数字、带符号、尾随字符、`0`、溢出与重复八类参数错误，
  以及批处理/REPL 的透传值（未提供时为 `kMaxQueryRows`）与两种格式下的超限错误展示；
- JSON 模式的 golden 文本、字段顺序、转义边界与 rows/row_count 一致性，配合测试内置的
  极简 JSON 解析校验器；kPlanOnly 在 table 与 JSON 两种格式下都按查询结果展示；
- pretty 模式的 golden 文本：边框对齐（含 CJK 宽字符）、数值列右对齐、NULL、空结果行数、
  超宽单元格截断、命令结果行；诊断与状态行在 pretty 下与 table 一致；
- 默认格式选择：`output_is_terminal` 为真且未给 `--format` 时用 pretty，为假时用 table；
  显式 `--format` 覆盖终端判定；
- `--time`：打开后 stdout 与关闭时逐字节相同，stderr 多出 `TIME script|line …` 行，
  批处理与 REPL 各一条、不影响退出码；
- SKIPPED（policy/aborted）、INDETERMINATE 与 script_error 的插入顺序和 stderr 归属；
- 语句级 lex/syntax/semantic、执行/存储、analysis 标签与范围格式；脚本级 compile/internal
  标签与空插入点；suggestion、fix-it 与单行转义；
- CommandResult 部分成功时 stdout 的 `OK <affected_rows>` 与 stderr 错误行的顺序；
- FakeSession 的 open/execute/close 调用顺序与“最多 close 一次”约束；
- 批处理遇错停止、首错后继续分析但最终退出码仍为 1、REPL 结构化错误后继续、
  script_error 后停止读取并 best-effort close、close 失败覆盖成功码但不改变既有失败码；
- 输入读取失败、输出写入失败、bad_alloc 与异常转换为退出码 1。

这类测试使用注入的输入流、输出流、交互模式和 FakeSession，不构造真实数据库文件，也不链接
真实 compiler/storage。

### 8.2 真实 compiler + fake storage 窄联调测试

此阶段使用真实 compiler、真实 core、真实 `CoreSession` 和 fake storage，测试目标不得链接真实
storage，也不得链接 fake compiler。至少覆盖：

- `CREATE TABLE → INSERT → SELECT → WHERE` 的真实 compiler/core/CLI 路径；
- compiler 语义错误到 core `ErrorKind::kCompile`、绝对位置和 CLI stderr 的转换；
- DDL 成功后 Catalog 对后续真实 compiler 调用可见；
- CLI 的批处理结果展示、退出码、open/execute/close 生命周期。

对应测试目标为 `tinydbms_compiler_core_cli_integration_test`，构建和验收命令见
[联调准备与验收清单](../联调准备与验收清单.md)。

### 8.3 真实 storage 产品级联调测试

真实 storage 构建下，`tinydbms.real_modules_integration` 产品级测试的目标范围包括：

- --data-dir 临时目录下的 CREATE、INSERT、SELECT、DELETE；
- 批处理遇错停止，REPL 遇错继续；
- close 后重新运行程序能够读到正常持久化结果；
- UTF-8 路径和 UTF-8 VARCHAR 展示；
- open、SQL、storage、close 错误的退出码和 stderr；
- stdout 不混入提示符或错误文本。

当前自动化目标已覆盖持久化、批处理停止、REPL 恢复、超大 SQL 不使会话失效、UTF-8 数据目录、
UTF-8 VARCHAR 展示、编译与语义错误位置、storage 运行期错误（超行宽插入）以及 open 失败路径。
仍未由该目标覆盖的是 close 的设备级失败：真实 storage 没有故障注入接缝，该路径由 fake storage
注入的 core/CLI 测试覆盖；CLI 层的 `ERROR execute` 标签在真实入口不可达，由 core/app 单元测试覆盖。
这些测试使用测试生成的 SQL 和临时目录，不依赖真实业务数据。它们验证的是外部模块已经
符合公共契约，而不是由 app 测试代替 compiler/storage 单元测试。

## 9. 实施顺序与验收标准

实施顺序：

1. 提取参数解析和展示的 app 私有类型；
2. 完成 help/version 和参数错误路径；
3. 完成批处理输入和 REPL 循环；
4. 接入 Database 生命周期及退出码累计；
5. 完成结构化结果展示；
6. 先运行不依赖外部模块的 CLI 测试；
7. 保留真实 compiler + fake storage 的 core/CLI 联调测试，并以
   `tinydbms.real_modules_integration` 覆盖真实 compiler/storage 产品级联调；
8. 执行完整构建、CTest 和工作区差异审查。

本阶段验收必须满足：

- app 不包含 SQL 或 storage 业务逻辑；
- app 不直接包含 compiler/storage 公共头文件；
- 默认占位构建不链接可执行的 SQL 运行链路，真实模块联调必须显式启用
  `TINYDBMS_ENABLE_REAL_MODULES=ON`；
- 参数错误返回 2，运行时错误返回 1；
- `--error-policy` 默认 stop；analyze 只影响后续语句的静态分析，不恢复退出码、不执行后续语句；
  任何 script_error 使 REPL 停止读取并使退出码为 1；
- REPL 和批处理的错误继续策略符合公共契约；
- runner 在所有 open 成功路径上最多显式调用一次 close；若 core 的 close_storage 自身抛异常，
  core 允许在后续 close 或 Database 析构中做一次清理重试；
- fake/模拟数据测试与真实模块联调测试明确分层；
- 真实 compiler + fake storage 的联调目标不链接 fake compiler 或真实 storage；
- `--format json` 与 `--plan` 的验收：默认行为零变化、JSON 每行可被标准解析器解析、
  计划模式零副作用（真实链路用临时目录前后文件清单与数据校验断言）；
- `--max-rows` 的验收：不提供时默认行为与现状一致（唯一有意变化是超限消息新增 `(limit N)`
  与 suggestion）；提供后上限真实生效、超限语句使退出码为 1 且会话仍可用、数据文件未被修改；
- U5 取消的验收：不请求取消时全部现有测试与输出零变化；请求取消后无 `script_error`、
  无副作用、cursor 无泄漏、Database 仍可继续使用；table 与 JSON 两种格式的 `CANCELLED`
  输出与退出码 1；REPL 每行使用独立令牌（第一行取消后第二行正常执行）；
  真实链路用预置令牌验证数据文件未被修改，SIGINT 的首次/空闲期/第二次三条路径由手工冒烟覆盖；
- 本轮为 U3（计划模式）修改了 include/tinydbms/core.hpp：新增 `ExecutionMode`、
  `StatementStatus::kPlanOnly`、`StatementResult::plan_only` 与 `ExecuteScriptRequest.mode`，
  已登记到 [消息契约详细设计](../消息契约详细设计.md) 并需要通知其他模块成员；
  除此之外不修改 include/tinydbms/。
- 本轮为 U4（结果集上限）再次修改 include/tinydbms/core.hpp：新增
  `ExecuteScriptRequest::max_query_rows`（带默认值，源兼容）；`kMaxQueryRows` 常量保留为
  默认值锚点，已登记到 [消息契约详细设计](../消息契约详细设计.md) 与
  [SQLv2 扩展契约](../SQLv2扩展契约.md) §7.3。
- 本轮为 U5（运行中取消）第三次修改 include/tinydbms/core.hpp：新增 `CancelToken`、
  `ExecuteScriptRequest::cancel`（带默认值）与 `StatementStatus::kCancelled`；新增枚举项会让
  穷举 `StatementStatus` 的 switch 编译告警/报错，调用方需要补分支（已同步 core、app、GUI
  与测试 fake）。请求对象因此不再是字面类型，`constexpr` 场景需要改用运行期校验。
