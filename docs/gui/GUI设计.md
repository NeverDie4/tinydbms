# GUI 设计（Qt6 Widgets 可选前端）

本文规定 tinydbms GUI 前端的定位、依赖边界、线程模型、展示映射、构建开关与测试策略。

GUI 是可选展示入口，不改变公共契约，也不替代 CLI：按课程指导书的核对结论，正式验收入口
仍是 CLI 与公共 API；GUI 的价值是可视化演示与交互体验，不是新的业务层。

本文与 [core 与 CLI 实现设计](../core-cli/实现设计.md)、[消息契约详细设计](../消息契约详细设计.md)、
[模块交互契约](../模块交互契约.md) 一起使用。GUI 不修改 `include/`，因此不需要在公共契约变更中
新增条目。

## 1. GUI 连接 core，不连接 CLI

GUI 在**同一进程内直接链接 core**，只通过 `tinydbms::core::Database` 工作：

```text
Qt 主线程（UI 控件、编辑器、结果表格）
  │  用户点击「执行」/「打开数据库」
  ▼
GUI 工作线程（单线程，串行）
  │  Backend::open / execute_script / close
  ▼
tinydbms::core::Database          ← 公共契约 include/tinydbms/core.hpp
  │  core 内部再调用 compiler 与 storage
  ▼
compiler / storage
```

GUI 与 CLI 是**同级的两个入口**，都只调用 core 的公开 API。GUI 不启动 `tinydbms` 子进程，
不解析 CLI 的文本输出，也不复用 `src/app` 的私有类型。

### 1.1 被否决的方案

| 方案 | 做法 | 否决理由 |
| --- | --- | --- |
| CLI 子进程 + 解析文本 | 启动 `tinydbms`，把 stdout/stderr 解析成表格 | CLI 输出是给人看的固定文本（`OK <n>`、制表符表格、`ERROR <label> <range> <message>`），没有机器可读模式；诊断、suggestion、fix-it 都要重新解析，展示格式一调整 GUI 就静默错位；还额外承担进程启动与退出码语义 |
| 复用 `src/app` 的 `Session` | GUI include `src/app/session.hpp` | `src/app/*.hpp` 是 app 私有头，`tinydbms_cli_logic` 只导出 `include/`。复用需要改 app 的 CMake 与头文件可见性，把私有适配层变成事实上的公共接口，侵入其他模块 |
| GUI 直接调用 compiler + storage | GUI 自己分句、编译、执行并访问表文件 | 绕过 core 的 Catalog、TableId 分配、脚本顺序与错误恢复职责，等于在 GUI 里重做 core；违反“入口只调 core”的分层 |
| 复用 CLI 展示代码 | include `src/app/output.{hpp,cpp}` 复用转义与范围格式化 | 同上的私有头问题；且 GUI 展示形态与 CLI 固定文本差异大，可共享的只有单行转义，收益小于耦合成本 |

### 1.2 依赖边界

允许使用：

- `include/tinydbms/core.hpp`：`Database`、`StatementResult`、`ExecuteScriptResult`、`Error`、
  `QueryResult`、`CommandResult`、`ScriptErrorPolicy`；
- `include/tinydbms/diagnostic.hpp`：`SourceRange`、`FixIt`、`CompileStage`；
- `include/tinydbms/common.hpp`：`Value`、`Type`、`SourceLocation`、各类上限常量；
- `Qt6::Widgets`。

禁止：

- include `src/app/`、`src/core/`、`src/compiler/`、`src/storage/` 的私有头；
- 在 GUI 内分句 SQL、判断语句类型、读写表文件；
- 通过 `message` 文本匹配推断错误种类；分类只读结构化字段（`status()`、`ErrorKind`、
  `compile_stage`、`source`）。

GUI 的私有头（`src/gui/*.hpp`）不导出给其他模块，也不进入 `include/`。

### 1.3 后端接缝

GUI 定义一个只含三个方法的私有接口，与 core 的三个入口一一对应，不引入额外概念：

```cpp
// src/gui/backend.hpp（GUI 私有，不进入 include/）
class Backend {
public:
    virtual ~Backend() = default;
    virtual core::OpenDatabaseResult open(const core::OpenDatabaseRequest& request) = 0;
    virtual core::ExecuteScriptResult execute_script(const core::ExecuteScriptRequest& request) = 0;
    virtual core::CloseDatabaseResult close() = 0;
};
```

- `CoreBackend`（`core_backend.cpp`）：持有 `std::unique_ptr<tinydbms::core::Database>`，
  只在 `open` 时构造，是唯一引用 core 的 GUI 文件；
- `UnavailableBackend`（`unavailable_backend.cpp`）：未接入 compiler/storage 时使用，
  与 app 的 `UnavailableSession` 同语义，只返回明确的不可用错误；
- `FakeBackend`（`tests/fakes/gui_backend_fake.*`）：只存在于测试，记录调用顺序与参数。

设立接缝的理由不是抽象分层，而是两个现实约束：Storage 是进程级单例，UI 测试不能写真实数据
目录；以及真实链路在 compiler 完成公共契约适配前无法链接（见 §9.5）。FakeBackend 无论如何
都要存在，接缝只是让它可用。

不复用 `tinydbms::app::Session` 的原因见 §1.1；GUI 与 app 各自持有自己的适配层，与
“入口负责组装依赖”的现有分层一致。

## 2. 一次执行的调用链

1. 用户在编辑器输入脚本文本，点击「执行」；
2. UI 线程生成文本快照（含版本号），把 `ExecuteScriptRequest{text, error_policy}` 投递到工作线程；
3. 工作线程调用 `Database::execute_script`，得到 `ExecuteScriptResult`；
4. 工作线程把结果连同快照版本号排队投递回 UI 线程；
5. UI 线程渲染：`script_error` → 顶部横幅；`statements` → 结果区与诊断列表；
   `kExecuted + QueryResult` → 结果表格；
6. 若结果携带 `fix_it`，且编辑器文本仍等于该次执行的快照，则「应用修复」可用。

编辑器文本只在 UI 线程修改，结果只应用于匹配的快照。执行串行，不存在结果与文本错配。

GUI 不自己分句：整段编辑器内容作为一次 `ExecuteScriptRequest.text` 提交，由 core 分句并对每条
语句返回带 `statement_index` 与 `SourceRange` 的结果。

## 3. 线程与生命周期模型

### 3.1 单工作线程

- 只有一个工作线程，`Database` 的构造、`open`、`execute_script`、`close` 都在该线程上发生；
  UI 线程从不触碰 `Database`；
- 队列顺序即执行顺序；core 自身也按调用级互斥串行化，因此"只用一条工作线程"是入口层的
  选择，而不是 core 强加的限制（见 [进程内并发设计](../core-cli/进程内并发设计.md)）；
- 结果与错误通过队列连接回到 UI 线程，UI 线程只做展示与本地编辑。

依据：core 契约承诺同一实例的公开方法可被并发调用但**串行执行**，不会给出并行度；
Storage 是进程级单例，同一时刻最多一个 `Database` 实例处于 open 或 cleanup-pending 状态。
单工作线程让结果顺序与 UI 快照天然对齐，仍然是本设计选择的入口层结构。

实现约定：

- 工作线程用 `QThread` 加一个 `QObject` 工作对象（`moveToThread`），请求以队列信号投递；
- `Backend` 实例由 `main` 通过 `make_backend()` 持有，但只允许在工作线程调用；
  `Database` 的构造发生在工作线程的首次 `open()`，窗口线程只持有接口指针用于投递请求；
- 跨线程信号的参数必须是已注册的可复制类型：`ExecuteScriptResult` 不直接上信号，改用 GUI 私有
  payload 承载「快照版本号 + 结果」，在该私有头内用 `Q_DECLARE_METATYPE` 声明、在 `main` 中
  `qRegisterMetaType` 注册；
- payload 内用 `std::shared_ptr<const ...>` 持有结果，避免队列连接复制大结果集；
- Qt 的类型与宏只允许出现在 `src/gui/`，公共头文件保持 Qt 无关。

### 3.2 任务进行中的 UI 状态

- 任务进行中禁用「执行」与「切换数据库」按钮；
- 第一版**不提供取消按钮**：core 自 U5 起提供 `CancelToken`（见
  [运行中取消设计](../core-cli/运行中取消设计.md)），`Backend` 可以在 worker 线程持有的
  请求上传入同一个令牌，UI 线程调用 `request_cancel()` 即可跨线程取消；本版只登记该边界，
  按钮与交互留作 GUI 侧的独立工作项。「分析模式」开关只决定 `ScriptErrorPolicy`，不是取消；
- 运行期间状态栏显示「执行中」，长脚本由 core 的上限（SQL 字节数、语句数、表达式深度）约束。

### 3.3 open / close 时机

- 启动后由用户选择或确认数据目录，然后 `open`；失败时显示错误、禁用编辑器，但允许改目录重试；
- 运行期间不重复 `open`，不并发 `close`；
- 关闭窗口时先在工作线程完成 `close`（期间常驻状态标签显示「正在关闭」），再退出进程；
  `close` 失败时提示失败原因，然后仍退出，不做阻塞式重试；
- `close` 之后如需继续使用，必须重新 `open`；GUI 不假设 Storage 可重入；
- 任务进行中收到关闭窗口请求时，拒绝关闭并提示「正在执行」，等任务结束后再关闭，不强制中断
  工作线程；
- 重新打开流程固定为「先 `close()`，成功后再 `open(目录)`」：`close()` 是 cleanup-pending 的
  唯一重试入口，core 已把 Storage 的 `kInvalidRequest` 视为清理完成，GUI 不需要区分该细节；
- 切换数据库与重新打开走同一条流程：`close()` 成功后 `open(新目录)`；`close()` 失败则不
  `open`，按 §3.5 进入「需要重新打开」状态；
- `open` 失败后**也**先 `close()` 再重试 `open`：core 在「open 失败且 storage 清理也失败」时
  会进入 `forced_close_pending` 并从此拒绝 `open`，而公共契约不暴露这个标志，GUI 无法区分
  「失败但干净」与「失败但脏」。统一先清一次是唯一不需要文本推断的做法，代价是干净失败时
  多一次空转的 `close()`；
- 上述空转由后端接缝消化，不由 `MainWindow` 判断：`CoreBackend` 在「上一次 `open` 没有成功
  过」时把 `close()` 的 `kExecute`（`database is not open`）视为清理完成，`kStorage` /
  `kInternal` 仍然上抛为真实失败（判定函数见 `src/gui/close_policy.hpp`）。
- 关闭窗口时若只剩「可能有 pending 清理」，直接退出：`Database` 析构会做一次兜底
  `close_storage`，进程随后结束，不阻塞用户关闭。

### 3.4 异常边界

core 已把 storage/compiler 异常与结果聚合失败转换为结构化错误（`script_error{kInternal}` 或
`kExecutionIndeterminate`），GUI 不依赖异常做流程控制。GUI 仍在工作线程入口捕获
`std::exception` 与未知异常，转换为内部错误并展示，作为防止意外异常带走进程的兜底。

### 3.5 会话健康与重新打开

脚本级错误分两类，GUI 的连接状态必须跟着区分，不能一律按「重试即可」处理：

| 收到的结构化信号 | 会话状态 | GUI 行为 |
| --- | --- | --- |
| `script_error` 且 kind 为 `kCompile`（分句失败、语句数超限） | 仍然健康 | 横幅提示；用户修改文本后可直接重新执行，不需要重开 |
| `script_error` 且 kind 为 `kInternal`（致命中止，可能伴随 `kExecutionIndeterminate`） | 会话已失效或进入 cleanup-pending | 横幅提示「会话状态未知，需要重新打开数据库」；禁用执行与刷新，只保留重新打开与关闭窗口 |
| 仅语句级 `kCompileError` / `kExecutionError`，无 `script_error` | 仍然健康 | 修改后可直接重新执行 |

`kExecutionIndeterminate` 必须提示「可能已产生副作用且不承诺回滚」，不能静默当成普通跳过。
「重新打开数据库」按 §3.3 的固定流程执行：先 `close()`（返回错误则提示并允许重试），成功后再
`open()`。

会话失效时同时清空表浏览器：`kCloseFailed` 与 `kNeedsReopen` 下的旧结构可能来自已经失效或
部分回滚的 storage 状态，继续展示会误导用户；此时只保留失败说明文字。

## 4. 界面结构与第一版范围

第一版是单窗口，采用手写布局（不使用 `.ui` 文件与 `.qrc` 资源，减少构建环节）：

```text
工具栏        [打开/切换数据库] [执行] [分析模式] [刷新表结构]   当前数据目录
左侧上半区    SQL 编辑器（等宽字体、诊断范围高亮）
左侧下半区    诊断列表（语句序号 / 阶段 / 范围 / message / suggestion / [应用修复]）
右侧主区      结果区（脚本级横幅 + 结果表格 + 语句状态行）
底部          状态栏（左侧临时消息：最近一次执行耗时等；右侧常驻连接状态标签）
可停靠面板    表浏览器（系统表的只读视图，见 §8）
```

表浏览器是可停靠面板，通过只读 SELECT 读取系统表，细节见 §8。

第一版必须能完成一条完整演示路径：打开数据目录 → 输入多条 SQL → 执行 → 看到结果表格与诊断
高亮 → 触发一次编译错误 → 应用 fix-it → 重新执行 → 关闭并重新打开确认持久化。

### 4.1 文件与类职责

| 文件 | 内容 | 关键职责 |
| --- | --- | --- |
| `main.cpp` | `QApplication`、payload 类型注册、调用 `make_backend()` | 只做装配，不含业务判断 |
| `main_window.{hpp,cpp}` | `MainWindow` | 工具栏、状态栏、连接状态机、把请求投递到工作线程、把结果分派给各面板 |
| `backend.hpp` | `Backend` 接口（见 §1.3） | 三个方法与 core 一一对应 |
| `close_policy.hpp` | `close_succeeded()` 纯判定函数 | 决定 `close()` 结果算不算「清理完成」（§3.3） |
| `core_backend.cpp` | `CoreBackend` 与 `make_backend()` 的真实实现 | 唯一引用 `tinydbms::core::Database` 的文件 |
| `unavailable_backend.cpp` | `UnavailableBackend` 与 `make_backend()` 的兜底实现 | 未接入真实模块时明确报不可用，不伪造数据 |
| `worker.{hpp,cpp}` | `Worker`（`QObject`） | 在工作线程串行调用 `Backend`，发出 payload 信号 |
| `execution_payload.hpp` | GUI 私有 payload 与 metatype 声明 | 承载快照版本号与结果（§3.1） |
| `script_editor.{hpp,cpp}` | `ScriptEditor`（`QPlainTextEdit` 子类） | 文本快照、诊断高亮、fix-it 应用 |
| `source_mapping.{hpp,cpp}` | 自由函数 | UTF-8 字节偏移与 `QTextDocument` 位置的换算（§6） |
| `result_model.{hpp,cpp}` | `ResultModel`（`QAbstractTableModel`） | 按需把 `Value` 转成显示文本，不预生成整表字符串 |
| `result_panel.{hpp,cpp}` | 结果区控件 | 结果表格、命令结果行、语句状态行、脚本级横幅 |
| `diagnostic_list.{hpp,cpp}` | 诊断列表 | 按 `statement_index` 展示阶段、范围、message、suggestion 与「应用修复」 |
| `schema_browser.{hpp,cpp}` | 表浏览器 | 通过只读 SELECT 刷新系统表视图（§8） |
| `session_state.{hpp,cpp}` | 会话状态枚举与判定 | 由结构化结果推断连接状态（§3.5） |

类之间只由 `MainWindow` 组装：各面板不持有 `Backend`，也不直接投递请求。

### 4.2 交互细节

- 编辑器内容为空白时「执行」按钮禁用；直接调用执行入口时只在状态栏提示「没有可执行语句」，
  不覆盖上一次结果；若结果中 `statements` 为空且 `script_error` 也为空（例如输入只有注释），
  结果区显示「没有可执行语句」，不当作错误；
- 每次执行替换上一次的结果区内容，不累积历史记录；
- 工具栏开关文字为「出错后继续分析剩余语句（首错后的语句只分析、不执行）」，默认关闭
  （`kStopOnFirstError`），避免被误解成「只分析、不执行」；
- 编辑器使用系统等宽字体（`QFontDatabase::systemFont(QFontDatabase::FixedFont)`），行列编号
  从 1 开始，与 `SourceRange` 一致；
- 窗口最小尺寸以「结果表格至少可见 3 列」为准，具体数值在实现时按截图确认，不作为契约。

## 5. 结果展示映射

GUI 的展示完全是结构化字段到控件的映射，不做文本推断：

| 结构化输入 | GUI 表现 | 说明 |
| --- | --- | --- |
| `ExecuteScriptResult::script_error` 非空 | 状态栏上方红色横幅：kind、范围、message、suggestion；本次按 `statements` 展示已有结果，用户修改文本后可重新执行 | 分句失败、语句数超限或致命中止；分句失败时 `statements` 为空 |
| `kExecuted` + `QueryResult` | 结果表格：表头取 `ColumnHeader::name`，单元格按 `ColumnHeader::type` 与 `Value` 变体渲染；空结果仍显示表头 | 不做列名推断，不重新格式化 SQL |
| `kExecuted` + `CommandResult`（无 error） | 状态行「OK，影响 N 行」 | `CREATE TABLE` 的 `affected_rows` 为 0 |
| `kPlanOnly` | 与 `kExecuted` + QueryResult 一样按结果表格渲染（单列 `plan`） | U3 计划模式的状态；GUI 当前不暴露 `--plan`，但映射必须存在，避免穷举 switch 缺少分支 |
| `kExecutionError` + `CommandResult{affected_rows, error}` | 「已成功影响 N 行后失败」+ 错误详情 | INSERT/DELETE/UPDATE 部分成功路径，必须同时显示已完成的 N |
| `kExecutionError` + `Error` | 错误详情（stage 标签、范围、message） | 普通执行/存储错误 |
| `kCompileError` | 编辑器范围高亮 + 诊断列表一条 + suggestion + 可用的「应用修复」 | 只读 `compile_stage`、`source`、`suggestion`、`fix_it` |
| `kAnalysisError` | 诊断列表一条，标记为「分析」 | analyze 策略下影子状态明确拒绝 |
| `kAnalysisOnly` | 结果区灰色行「已分析，未执行」 | analyze 策略首错之后 |
| `kSkippedExecution` | 结果区灰色行「已跳过」 | stop 策略剩余语句，或致命中止之后 |
| `kCancelled` | 结果区灰色行「已取消」 | U5 运行中取消：当前语句在无副作用检查点结束，或取消后未开始的语句；不携带 outcome，也不表示状态未知 |
| `kExecutionIndeterminate` | 醒目警告「状态未知：storage 可能已产生副作用，请重新打开数据库核对」 | 必须显式提示，不能静默 |

展示规则：

- `statements` 按 `statement_index` 升序渲染，每条一行，可展开查看明细；
- 每条语句显示其 `source` 范围与序号，便于与编辑器对照；
- `ErrorKind` 与 `CompileStage` 到界面文案的映射集中在一处查找表，不散落在控件代码中；
- 大结果集不在 UI 层新设上限，也不放宽 core 的限制：结果按原样接收，转换发生在表格模型的
  `data()` 中（按需转换，见 §4.1 的 `result_model`）；
- 单元格直接渲染 `Value` 的原文，不套用 CLI 的单行转义；含换行的 VARCHAR 按原文本显示，
  完整值同时可通过 tooltip 查看；
- `Value` 的渲染按变体分支实现，不按字符串内容猜测类型，SQL v2 增加类型时只扩这一处。

## 6. SourceRange 与编辑器的坐标映射

契约坐标系（`diagnostic.hpp`）：行列从 1 开始，`column` 与 `offset` 都按 UTF-8 字节计算，
范围是半开区间 `[begin, end)`，`offset` 从 0 开始。

`QTextDocument` / `QTextCursor` 使用 UTF-16 码元索引，两者必须显式换算：

- 维护一个 `QString` 索引 ↔ UTF-8 字节偏移的映射，用二分查找把 `offset` 换算为编辑器位置；
- 换算按字节边界归位：偏移落在多字节字符中间时取该字符起点（正常契约下不会出现，属防御）；
- `offset == 文本总长度` 是合法的文档末尾插入点；
- CRLF 与 `\r\n` 按两个字节计，`\n` 之前的插入点仍然有效；Tab 计 1 个字节、1 列；
- `QPlainTextEdit` 会把行分隔符统一为 `\n`，因此 GUI 送入 core 的文本不含 CRLF；换算函数仍按
  契约支持 CRLF，供以后直接加载 `.sql` 文件使用；
- 空范围 `[x, x)`（插入点）不渲染为选区：向后扩一个字符作为可见标记，位于文档末尾时向左扩
  一个字符；
- 偏移越界或范围无效时不闪烁、不高亮，只在诊断列表显示 message，并附加「范围无效」标记。

fix-it 应用的前置条件与行为：

1. 编辑器当前文本必须与执行时快照完全一致，否则「应用修复」禁用并提示「文本已修改，请重新
   执行后再应用」；
2. 用同一个换算函数定位 `FixIt::range`，以 `QTextCursor` 替换为 `FixIt::replacement`
   （按 UTF-8 解码）；
3. 应用后清空本次执行的诊断高亮（文本已改变，旧范围失效），不自动重跑；
4. 任何手工编辑同样清空高亮并禁用「应用修复」，诊断列表文字保留供参考；
5. 一次执行只保留最新一次诊断的高亮，不做诊断累积。

上述换算与 fix-it 逻辑放在独立源文件中，以便在无窗口环境下单独测试。

## 7. 数据目录与连接流程

- 默认数据目录为 `./tinydbms-data`，与 CLI 的 `kDefaultDataDir` 保持一致；GUI 使用自己的私有
  常量，并在测试中交叉断言两者相等，避免与 app 侧漂移；
- 数据目录不存在的目录由 storage 在 `open` 时创建，GUI 不预创建、不写入；
- GUI 只传非空目录字符串，`OpenDatabaseRequest::data_dir` 为空的情况不出现；
- `open` 失败：显示 message，保持未打开状态，编辑器禁用但允许换目录重试；
- 第一版不持久化「上次使用的目录」（不写用户配置文件）。

## 8. 表浏览器

- 通过只读 SELECT 读取系统表：`SELECT * FROM tdb_sys_tables`，
  `SELECT * FROM tdb_sys_columns`（按需追加 `WHERE table_id = <N>`）；
- 每次刷新是一次独立的 `execute_script` 调用，遵循同一打开状态，不直接访问 storage；
- 刷新时机：手动刷新按钮、`open` 成功之后、以及某次执行中出现成功 DDL 语句之后；
- 刷新失败与普通语句错误一样展示，不额外吞掉错误；
- 不缓存 schema 作为权威数据，系统表是唯一来源。

## 9. 构建与检查开关

### 9.1 开关语义

新增单一开关 `TINYDBMS_BUILD_GUI`，默认 `OFF`：

- `OFF`：不查找 Qt、不生成 GUI 目标、不注册 GUI 测试。其他成员的 `cmake --preset debug` 与
  `ctest --preset debug` 的目标列表、测试数量与今天完全一致；
- `ON`：生成 `tinydbms-gui` 与 GUI 测试（推荐配合独立构建树的 `gui` 预设）；测试与构建共用
  该开关，不需要第二个变量。

### 9.2 根构建改动（唯一的共享构建文件改动）

```cmake
# CMakeLists.txt（在 option(TINYDBMS_BUILD_TESTS ...) 附近追加）
option(TINYDBMS_BUILD_GUI "Build the optional Qt6 GUI frontend" OFF)

# 在 add_subdirectory(src/app) 之后追加
if(TINYDBMS_BUILD_GUI)
    add_subdirectory(src/gui)
endif()
```

纯追加，不修改任何现有行，也不影响现有目标的编译选项与链接关系。

### 9.3 GUI 自身的构建文件

```cmake
# src/gui/CMakeLists.txt（新增文件）
set(CMAKE_AUTOMOC ON)                     # 只作用于本目录，避免根级 moc 全量扫描
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_library(tinydbms_gui_objects OBJECT
    main_window.cpp
    worker.cpp
    script_editor.cpp
    source_mapping.cpp
    result_model.cpp
    result_panel.cpp
    diagnostic_list.cpp
    schema_browser.cpp
    session_state.cpp
)
target_include_directories(tinydbms_gui_objects PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(tinydbms_gui_objects PUBLIC Qt6::Widgets)

add_executable(tinydbms-gui
    main.cpp
    $<TARGET_OBJECTS:tinydbms_gui_objects>
)
target_link_libraries(tinydbms-gui PRIVATE Qt6::Widgets)

if(TINYDBMS_ENABLE_REAL_MODULES)          # 后端选择与 app 的两套 Session 同模式
    target_sources(tinydbms-gui PRIVATE core_backend.cpp)
    target_link_libraries(tinydbms-gui PRIVATE tinydbms_core)
else()
    target_sources(tinydbms-gui PRIVATE unavailable_backend.cpp)
endif()
```

要点：

- `find_package(Qt6)` 只在开关打开时执行，非 Qt 机器不会被影响；
- `CMAKE_AUTOMOC` 只在本目录作用域生效，不在根 `CMakeLists.txt` 设置；
- 两个后端各自单独成文件，且只有一个参与编译，使 UI 与测试目标在不链接 core 的情况下也能构建；
- `tinydbms-gui` 需要显式链接 `Qt6::Widgets`：它只消费 `$<TARGET_OBJECTS:...>`，
  不会自动继承对象库的 usage requirements。

### 9.4 测试注册

```cmake
# tests/CMakeLists.txt（末尾追加一个受开关保护的块）
if(TINYDBMS_BUILD_GUI)
    add_executable(tinydbms_gui_ui_test
        gui_ui_test.cpp
        fakes/gui_backend_fake.cpp
        $<TARGET_OBJECTS:tinydbms_gui_objects>
    )
    target_include_directories(tinydbms_gui_ui_test PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/gui
        ${PROJECT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/src/app      # 仅用于默认数据目录常量的交叉断言
    )
    target_link_libraries(tinydbms_gui_ui_test PRIVATE Qt6::Widgets)
    add_test(NAME tinydbms.gui_ui COMMAND $<TARGET_FILE:tinydbms_gui_ui_test>)
    set_tests_properties(tinydbms.gui_ui PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        LABELS gui
    )
endif()
```

该测试目标**不链接 core**：它只使用 `FakeBackend` 与 UI 源文件，因此在 compiler 完成适配之前
也能构建和运行，模型与 core 测试使用 `$<TARGET_OBJECTS:...>` 加 fake 的现有做法一致。
`QT_QPA_PLATFORM=offscreen` 让测试在无显示环境（含 SSH 会话）可运行，`LABELS gui` 支持
`ctest -L gui` 单独执行。

守卫块内需要再次 `find_package(Qt6 ...)`：Qt 的 imported target 只在创建它的目录及其子目录
可见，`tests/` 与 `src/gui/` 是并列目录；第二次查找命中缓存，代价可忽略。

`src/app` 只作为头文件搜索路径用于默认数据目录常量的交叉断言：`kDefaultDataDir` 是头文件内
常量，不产生链接依赖，UI 测试仍然不链接 app 的逻辑库。

### 9.5 链接现状

`core` 静态库在链接期依赖 `compiler` 与 `storage` 目标。Compiler 适配（PR #4/#5）与 Storage V2
已于 2026-09-14 合入本分支，[联调准备与验收清单](../联调准备与验收清单.md) §3.1 记录的
公共头迁移阻断已关闭。当前：

- 默认配置（`TINYDBMS_ENABLE_REAL_MODULES=OFF`）下 `tinydbms-gui` 编译 `unavailable_backend.cpp`，
  可以正常构建、启动并调试界面；打开数据库会得到明确的「实现不可用」错误，不伪造数据；
- 真实链路（`TINYDBMS_ENABLE_REAL_MODULES=ON`）下 `tinydbms-gui` 链接 `tinydbms_core`，与同一
  开关下的 CLI 一致；`TINYDBMS_ENABLE_REAL_MODULES=ON` + `TINYDBMS_BUILD_GUI=ON` 的构建目录
  已完成全量构建并通过 CTest；
- `tinydbms_gui_ui_test` 不链接 core，可独立构建和运行，覆盖界面与结果模型行为；
- 真实数据目录下的人工交互验收（打开库、执行脚本、修复建议展示）仍需在本机 Qt 环境手动执行，
  不能以离屏 UI 测试代替。

### 9.6 预设

在 `CMakePresets.json` 追加（不修改现有 `debug` 预设）：

```json
{
  "name": "gui",
  "displayName": "Debug (Ninja, Qt6 GUI)",
  "generator": "Ninja",
  "binaryDir": "${sourceDir}/build/gui-debug",
  "cacheVariables": {
    "CMAKE_BUILD_TYPE": "Debug",
    "TINYDBMS_BUILD_TESTS": "ON",
    "TINYDBMS_BUILD_GUI": "ON"
  }
}
```

并追加对应的 build / test preset。独立 `binaryDir` 使 GUI 构建产物与主构建树互不干扰。

### 9.7 编译选项

根级 `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion` 会同样作用于 GUI 源码，这是代码
质量要求，不额外放宽。Qt 的 imported target 头文件通常按系统头处理，预期不产生第三方警告；
若实际出现，再在 GUI 目标内局部处理，不动根级选项。

### 9.8 Qt 版本与依赖边界

- `find_package(Qt6 6.3 REQUIRED COMPONENTS Widgets)`：只依赖 Widgets 一个组件，本机为 6.11.2；
- 不使用 `.ui` 文件、`.qrc` 资源与 `AUTOUIC`/`AUTORCC`，第一版全部手写布局；
- 不使用 `Qt6::Test`：单元测试沿用 `tests/` 现有的手写 `CHECK` 与 `main` 返回码风格，
  offscreen 下只额外需要 `QApplication`；
- 不使用 `QtConcurrent`、`QtNetwork`、`QtSql`：线程模型只有 §3.1 的单一 `QThread`。

## 10. 测试策略

GUI 测试全部使用 `FakeBackend`，不依赖真实 compiler/storage，也不写真实数据目录。用例覆盖：

- 九态 `StatementStatus`（含 U3 的 `kPlanOnly` 与 U5 的 `kCancelled`）各自的渲染映射，
  以及 `script_error` 与 `statements` 并存的情况；
- `CommandResult` 部分成功：同时显示 affected_rows 与错误；
- 空结果集仍渲染表头；INT 与 VARCHAR（含 UTF-8）单元格文本；
- 大结果集只转换可见行（表格模型按需转换，不预生成整表字符串）；
- `SourceRange` 换算：多字节字符位于错误位置之前、CRLF、Tab、文档末尾插入点、越界范围；
- fix-it 应用成功路径，以及「文本已修改」导致按钮禁用的路径；
- 执行按钮在任务进行中禁用，多个请求按投递顺序串行处理；
- `open` 失败后编辑器禁用，换目录重试时先 `close()` 再 `open()`（cleanup-pending 探测）；
- 重试时的清理也失败：不继续 `open`，进入「需要重新打开」；清理恢复后可正常打开；
- `close_succeeded()` 判定：`open` 从未成功时的 `kExecute` 视为清理完成，`kStorage` /
  `kInternal` 一律视为失败（纯函数用例，不依赖 UI）；
- `close` 失败提示与关闭窗口往返；
- 状态栏分工：连接状态写在常驻标签，`执行完成，用时 N ms` 这类临时消息不被状态刷新覆盖；
- 表浏览器按 `table_id` 关联父子节点：表名以 `"(数字)"` 结尾（如 `beta (2)`）时列仍归属正确；
- FakeBackend 调用顺序：`open` 与 `close` 的调用次数与先后关系逐用例断言（切换数据库先
  `close` 后 `open`；重试路径允许 `close` 出现多次）；
- 默认数据目录常量与 app 侧 `kDefaultDataDir` 一致（交叉断言，防止漂移）。

执行环境约定：

- 测试 `main` 先构造 `QApplication`，`QT_QPA_PLATFORM=offscreen` 由 CTest 环境变量提供；
- 目录选择对话框只属于手工路径：测试直接调用接收路径的入口（例如
  `MainWindow::open_directory(path)`），不模拟模态对话框；
- 断言沿用 `CHECK` 宏与 `main` 返回非零，不引入 QtTest 或 `QSignalSpy`；
- 测试只使用注入的 fake 结果与临时窗口对象，不启动真实数据库、不写真实数据目录。

## 11. 侵入点清单

| 位置 | 改动 | 性质 |
| --- | --- | --- |
| `CMakeLists.txt` | 两个纯追加块（开关 + `add_subdirectory`） | 唯一必须改动的共享构建文件 |
| `tests/CMakeLists.txt` | 末尾一个受开关保护的测试块 | 纯追加 |
| `CMakePresets.json` | 追加 `gui` 预设 | 纯追加，可选 |
| `README.md` | 追加 GUI 构建与运行说明 | 纯追加 |
| `tests/gui_ui_test.cpp`、`tests/fakes/gui_backend_fake.{hpp,cpp}` | 新增测试与测试用假后端 | 不侵入现有测试 |
| `src/gui/**` | 全新目录 | 不侵入现有模块 |
| `include/`、`src/app/`、`src/core/`、`src/compiler/`、`src/storage/` | 0 行 | 不侵入 |
| CI 与脚本目录 | 仓库当前没有 `.github/` 与 `scripts/`，无需改动 | 不侵入 |

## 12. 需要通知其他成员的事项

- 公共契约无变化：`include/` 不动，其他模块无需适配 GUI；
- 新增可选开关 `TINYDBMS_BUILD_GUI`，默认 `OFF`；不开启时不会查找 Qt、不新增目标、不新增测试；
- GUI 不需要其他成员安装 Qt，也不参与编译器/存储的交付范围；
- 若演示需要「应用修复」，需要 compiler 在真实诊断中填充 `suggestion` 与 `fix_it`（契约已
  允许，字段为空时 GUI 自动隐藏对应控件）；
- 若后续有人引入 CI，保持默认 `OFF` 即可让 CI 与 Qt 解耦。

## 13. 实施顺序与验收标准

实施顺序：

1. 本文评审通过；
2. 根开关 + `src/gui/CMakeLists.txt` + 空窗口骨架 + `Backend`/`FakeBackend`；
3. `tinydbms_gui_ui_test` 与九态渲染、范围换算、fix-it 测试（此时不依赖 core）；
4. 编辑器、结果表格模型、诊断列表、表浏览器；
5. compiler 适配完成后补 `CoreBackend` 与真实链路手工验收；
6. 补 README 与预设说明，执行 `debug`、`gui-debug` 两棵构建树的完整构建与 CTest。

第 1–4、6 步已于 2026-09-14 完成：`src/gui/` 全部源文件、`tinydbms_gui_ui_test` 的 16 个用例、
offline 运行与 README 说明均已落地；第 5 步（真实链路）等待 compiler 合入公共契约适配。当前
验证结果：

- `cmake --preset gui` + `cmake --build build/gui-debug --target tinydbms-gui tinydbms_gui_ui_test`
  通过，无警告；
- `ctest --test-dir build/gui-debug -L gui` 通过（16 个用例）；
- `build/sanitize-gui`（ASan + UBSan）同一组用例通过，无 sanitizer 报告；
- offscreen 下直接启动 `tinydbms-gui` 不崩溃，进入事件循环；
- 默认构建树 `build/debug` 重新配置后仍为 53 个测试、无 GUI 目标、缓存中没有 Qt 依赖。

一轮审查后的修复（close 语义与状态展示）已补测试，并用反向注入验证过用例有效性：去掉
`MainWindow::open_directory()` 中的 cleanup-pending 分支后，
`open_failure_retries_close_before_reopen` 与 `open_failure_cleanup_failure_blocks_reopen`
两个用例失败。

验收标准：

- 默认 `OFF` 时，目标列表、CTest 用例数量与非 GUI 构建完全一致；
- 开启开关后可完成 GUI 构建，GUI 测试在 offscreen 下通过；
- GUI 不 include 任何模块私有头，不解析文本推断错误种类，不自己分句 SQL；
- 空白输入与空结果集不产生伪错误；会话失效后进入「需要重新打开」状态而不是继续执行；
- 真实链路可演示：打开目录 → CREATE/INSERT/SELECT → 编译错误高亮 → 应用 fix-it → 重新执行
  → 关闭重开确认持久化；
- 无 Qt 的机器上 `cmake --preset debug` 与 `ctest --preset debug` 行为不变。

## 14. 第一版不做的事

SQL 语法高亮与补全、多标签或多连接、结果导出、取消按钮、结果分页、查询历史、
用户偏好持久化、国际化、皮肤主题、Windows 打包，以及编辑器内的「光标语句提示」
（需要可靠的分句信息与光标位置联动，第一版不做）。

这些能力都依赖公共契约目前没有的接口（例如流式结果）或额外的产品决策，等出现真实需求
再单独设计，不在第一版预留抽象。

## 15. 已确认决策

1. 演示内容按 §4 的最小演示路径执行：单库、多语句执行、结果表格、诊断高亮与 fix-it；
   若课程另有展示脚本，再单独并回 §4。
2. GUI 与 compiler 适配并行开工：GUI 的界面、接缝与 offscreen 测试已完成，真实链路等
   compiler PR 合入后再做一次集成联调（见 §9.5）。
