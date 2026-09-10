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
- core 返回的 outcomes 按顺序展示；
- core 在第一条错误后停止后续 SQL，app 不自行继续执行剩余文本；
- 展示完成后调用一次 close，再根据 close 结果确定最终退出码。

### 4.3 REPL

REPL 循环使用 getline 读取一行，每读到一行调用一次 execute_script：

1. 读取一行；
2. 调用 core；
3. 按顺序展示 outcomes；
4. 若有 Error 或携带 error 的 CommandResult，设置累计错误标记；
5. 继续读取下一行。

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
同一 ExecuteScriptResult 中的 outcomes 保持原顺序。

### 6.2 初版稳定格式

为了便于脚本测试，初版采用简单的制表符格式：

- CommandResult：stdout 输出 `OK `、affected_rows 和换行；
- QueryResult：第一行输出列名，以制表符分隔；后续每行输出对应 Value，以制表符分隔；
- 空结果仍输出列头，不额外输出“无结果”文本；
- INT 按十进制输出；
- VARCHAR 输出 UTF-8 文本；反斜杠、制表符、换行和回车分别编码为 `\\`、`\\t`、`\\n`、`\\r`，
  保证一行一个结果记录且不破坏制表符分隔；
- Error：stderr 输出 `ERROR <kind> <message>\\n`；编译错误在 kind 后先输出
  `line:column` 和一个空格，即 `ERROR compile 2:3 <message>\\n`，其他错误例如
  `ERROR storage <message>\\n`。kind 固定为 `compile`、`execute`、`storage` 或 `internal`；
  错误消息使用与 VARCHAR 相同的单行转义。

CREATE TABLE 成功时 affected_rows 为 0。携带 error 的 CommandResult 先输出已完成的
affected_rows，再输出对应错误，并把本次入口状态标记为失败。

help 和 version 文本属于 CLI 自身输出，写入 stdout；参数错误和执行错误写入 stderr。
格式化实现不得修改 QueryResult 或 CommandResult 的所有权和内容；任何 stdout/stderr 写入
失败都标记为 I/O 错误并返回 1，不能把写入失败误判为 SQL 成功。

## 7. 退出码与错误策略

退出码固定为：

- 0：参数、输入、SQL 执行和 close 全部成功；
- 1：open/close 失败、SQL 编译/执行失败、storage 错误或输入输出错误；
- 2：命令行参数错误。

优先级如下：

1. 参数错误直接返回 2，不创建 Database；
2. 参数成功但 open 失败返回 1；
3. open 成功后，任何 execute_script、读取或展示错误都累计为失败；
4. close 失败也返回 1；
5. REPL 遇到 SQL 错误继续读取，EOF 后按累计错误标记和 close 结果返回；
6. 批处理遇到错误不重复执行剩余 SQL，但仍展示 core 已返回的结果并执行 close。

## 8. CLI 测试设计

### 8.1 不依赖真实 compiler/storage 的测试

以下测试不依赖真实 compiler/storage：

- help/version 参数的成功路径；
- 未知参数、缺少值、重复 data_dir、空值和参数混用；
- 解析结果和默认 data_dir；
- 批处理与 REPL 的模式判定；
- 不输出 REPL 提示符的批处理路径；
- QueryResult、CommandResult、Error 的格式化，包括制表符/换行/反斜杠转义；
- FakeSession 的 open/execute/close 调用顺序与“最多 close 一次”约束；
- 批处理遇错停止、REPL 遇错继续、close 失败覆盖成功码但不改变既有失败码；
- 输入读取失败、输出写入失败和异常转换为退出码 1。

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
- REPL 和批处理的错误继续策略符合公共契约；
- runner 在所有 open 成功路径上最多显式调用一次 close；若 core 的 close_storage 自身抛异常，
  core 允许在后续 close 或 Database 析构中做一次清理重试；
- fake/模拟数据测试与真实模块联调测试明确分层；
- 真实 compiler + fake storage 的联调目标不链接 fake compiler 或真实 storage；
- 不修改 include/tinydbms/；若确需修改，登记公共文件变更并通知模块成员。
