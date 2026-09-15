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
- **取消按钮（已实现，见 §17）**：core 自 U5 起提供 `CancelToken`（见
  [运行中取消设计](../core-cli/运行中取消设计.md)）。每次执行生成独立令牌，经队列信号传入
  worker 线程；UI 线程点击「取消」只调用 `request_cancel()` 置位无锁标志，core 在下一个
  检查点结束当前语句。置位后按钮禁用、状态栏显示「正在取消」，结果按 `kCancelled` 正常渲染。
  取消是请求而不是强制中断：storage 的单次调用不可中断，响应延迟上界与运行中取消设计一致。
  「分析模式」开关只决定 `ScriptErrorPolicy`，不是取消；
- 运行期间状态栏显示「执行中」或「正在取消」，长脚本由 core 的上限（SQL 字节数、语句数、
  表达式深度）约束。

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
工具栏        [打开/切换数据库] [执行] [取消] [分析模式] [刷新表结构]   当前数据目录
左侧上半区    SQL 编辑器（等宽字体、诊断范围高亮）
左侧下半区    诊断列表（语句序号 / 阶段 / 范围 / message / suggestion / [应用修复]）
右侧主区      结果区（脚本级横幅 + 表格/图表标签页 + 语句状态行）
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
| `script_editor.{hpp,cpp}` | `ScriptEditor`（`QPlainTextEdit` 子类） | 文本快照、诊断高亮、fix-it 应用、可执行文本缓存（§18） |
| `source_mapping.{hpp,cpp}` | 自由函数 | UTF-8 字节偏移与 `QTextDocument` 位置的换算，单个与成批两种入口（§6、§18） |
| `result_model.{hpp,cpp}` | `ResultModel`（`QAbstractTableModel`） | 按需把 `Value` 转成显示文本，不预生成整表字符串 |
| `result_panel.{hpp,cpp}` | 结果区控件 | 结果表格、命令结果行、语句状态行、脚本级横幅、导出与复制按钮（§19） |
| `result_export.{hpp,cpp}` | 自由函数 | 把当前查询结果渲染成 CSV / TSV 文本与带 BOM 的 UTF-8 字节（§19） |
| `diagnostic_list.{hpp,cpp}` | 诊断列表 | 按 `statement_index` 展示阶段、范围、message、suggestion 与「应用修复」 |
| `schema_browser.{hpp,cpp}` | 表浏览器 | 通过只读 SELECT 刷新系统表视图（§8） |
| `schema_query.hpp` | 系统表只读脚本常量（不依赖 Qt） | 界面与真实 compiler 联调测试引用同一份文本，避免两处各写一遍（§19） |
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
| `kExecuted` + `QueryResult`（含数值列） | 「图表」标签页：数值列柱状图；没有数值列时显示空状态文案 | 展示层派生，不改变结果集；规则见 §16 |
| `kExecuted` + `CommandResult`（无 error） | 状态行「OK，影响 N 行」 | `CREATE TABLE` 的 `affected_rows` 为 0 |
| `kPlanOnly` | 结果表格渲染计划文本（单列 `plan`），语句摘要写「计划 N 行文本（未执行）」 | U3 计划模式的状态；GUI 由「只编译，不执行（查看计划）」开关触发（§19.1），永远不进入执行器 |
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

- 通过只读 SELECT 读取系统表，文本固定为
  `SELECT * FROM tdb_sys_tables; SELECT * FROM tdb_sys_columns;`（按需追加
  `WHERE table_id = <N>`）。分句契约要求每条语句（含末条）以 `;` 结束，末条省略会被
  compiler 报成 syntax error，面板只会显示「无法读取系统表」。该文本放在不依赖 Qt 的
  `schema_query.hpp`，由实现与真实 compiler 联调测试共同引用（§19.4）；
- 每次刷新是一次独立的 `execute_script` 调用，遵循同一打开状态，不直接访问 storage；
  刷新请求固定使用 `kExecute` 模式与默认上限，不受「计划模式」「结果上限」开关影响，
  否则面板会拿到一份计划文本；
- 刷新时机：手动刷新按钮、`open` 成功之后、会话失效后的重新打开，以及**某次执行里出现
  成功写语句之后**（`kExecuted` 且 outcome 为不带 error 的 `CommandResult`）；
- 写语句后的自动刷新只能按「有无成功写语句」判断：`CommandResult` 只有 `affected_rows`
  与可选 error，不携带语句种类，无法把 `CREATE TABLE` 与「影响 0 行的 DELETE/UPDATE」
  区分开。因此这条规则是 DDL 的超集——DML 之后也会多一次只读查询，代价是两次系统表
  SELECT，换来面板不会停留在旧结构。要精确区分需要在 `StatementResult` 上补语句种类，
  属于跨模块契约改动，等有真实需求再提；
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
- `tinydbms.gui_core_backend`（`TINYDBMS_ENABLE_REAL_MODULES=ON` 时构建，不依赖 Qt）与
  `tinydbms-gui` 编译同一个 `core_backend.cpp`，自动覆盖真实后端的数据路径：未 open 时执行、
  打开库、建表/写入/查询、语义错误的结构化字段（`kind`/`compile_stage`/`source`）、
  suggestion 与 fix-it、`open` 失败后的 cleanup 判定、close 与重开；
  该用例只受真实模块开关约束、不受 `TINYDBMS_BUILD_GUI` 约束：`core_backend.cpp` 本身不依赖
  Qt，把它放进默认的真实模块构建可以在没有 Qt 的机器上也守住这条数据路径，因此真实模块构建的
  用例数是 GUI=OFF 65 项、GUI=ON 66 项（后者多出的是离屏界面用例 `tinydbms.gui_ui`）；
- 真实数据目录下的**人工交互验收**（点击打开库、在编辑器里执行脚本、界面上展示修复建议）
  仍需在本机 Qt 环境手动执行，不能以离屏 UI 测试代替；上面的后端用例只保证这条数据路径本身
  已被自动化覆盖。

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

界面测试（`tinydbms.gui_ui`）全部使用 `FakeBackend`，不依赖真实 compiler/storage，也不写真实
数据目录；真实后端的数据路径由另一支用例 `tinydbms.gui_core_backend`
（`TINYDBMS_ENABLE_REAL_MODULES=ON` 时构建，见 §9.5）在临时目录中覆盖。界面测试用例覆盖：

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
- 结果图表：数值系列提取（INT/BIGINT/DOUBLE 入选，BOOLEAN/VARCHAR 排除）、SQL NULL 保留为
  缺口、单数值列的行号 x 轴回退、无数值列的空状态、超过 `kMaxChartRows` 的显式截断，
  以及有数据与空状态两种离屏绘制 smoke；
- 取消按钮：执行中按钮可用 → 点击后按钮禁用、状态栏进入「正在取消」、工作线程内的令牌确实
  被置位，并以 `kCancelled` 结果回到结果区；
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

SQL 语法高亮与补全、多标签或多连接、结果分页、查询历史、
用户偏好持久化、国际化、皮肤主题、Windows 打包，以及编辑器内的「光标语句提示」
（需要可靠的分句信息与光标位置联动，第一版不做）。

这些能力都依赖公共契约目前没有的接口（例如流式结果）或额外的产品决策，等出现真实需求
再单独设计，不在第一版预留抽象。

## 15. 已确认决策

1. 演示内容按 §4 的最小演示路径执行：单库、多语句执行、结果表格、诊断高亮与 fix-it；
   若课程另有展示脚本，再单独并回 §4。
2. GUI 与 compiler 适配并行开工：GUI 的界面、接缝与 offscreen 测试已完成，真实链路等
   compiler PR 合入后再做一次集成联调（见 §9.5）。

## 16. 第二版收尾：结果图表（2026-09-15）

状态：**已实现并验收**。第一版只有结果表格；收尾阶段在不改动公共契约、不引入新依赖的
前提下，为同一份 `QueryResult` 增加一个派生的图表视图。

### 16.1 范围与依赖

- 结果区改为「表格 / 图表」两个标签页，表格仍是默认视图与完整数据视图；
- `chart_data.{hpp,cpp}` 负责把 `QueryResult` 提取成 `ChartData`，`chart_widget.{hpp,cpp}`
  负责自绘柱状图；不依赖 Qt Charts，`TINYDBMS_BUILD_GUI` 的依赖边界仍是 Qt6 Widgets；
- 数据提取是可单测的纯函数，`ChartWidget` 只做绘制，两者与 `ResultPanel` 的接线解耦；
- 查询计划视图与结果导出在 §19 单独收尾，不属于本节范围。

### 16.2 提取与展示规则

- 多列且首列之后存在数值列（INT/BIGINT/DOUBLE）：首列作 x 轴标签，其余数值列作系列；
- 否则回退为行号作 x 轴，全部数值列作系列；
- 没有任何数值列：图表显示「没有可绘制的数值列」，不伪造图形；结果为空时显示「结果为空」；
- BOOLEAN 与 VARCHAR 不参与数值系列；x 轴标签复用表格的 `value_text`，换行、回车与制表符
  折叠为空格；
- SQL NULL 保留为缺口（`std::optional<double>` 的 `nullopt`），不折算成 0；
- 行数超过 `kMaxChartRows = 200` 时按行截断绘制，并在图表内标注
  「仅绘制前 N 行，共 M 行（表格视图展示完整结果）」——不静默改变数据；
- 全正数据只向上留白、全负数据只向下留白，避免 0 基线被留白推入错误方向。

已知边界：所有系列共用一条 y 轴，量级差异大时小数值系列会被压扁；不做双轴或归一化，
避免制造误导性图形。需要更强表达能力时再单独设计（例如按系列归一化或切换图表类型）。

### 16.3 验证

- `tinydbms.gui_ui` 新增三个用例：数值系列选择与 NULL 缺口、单列回退与截断、结果区接线
  与离屏绘制 smoke；
- 另用离屏渲染核对过实际图像：图例、网格、x 轴标签、NULL 缺口与 0 基线刻度均正确；
- `build/gui-debug` 全量 63/63 通过。

## 17. 第二版收尾：取消按钮（2026-09-15）

状态：**已实现并验收**。U5 交付了 core 侧的 `CancelToken` 与检查点，GUI 侧当时只登记边界；
收尾阶段把按钮与交互补齐，公共契约无变化。

### 17.1 实现要点

- `ExecuteScriptRequest::cancel` 已存在，GUI 不新增 core 接口；
- `MainWindow` 每次执行生成独立令牌（`active_cancel_`），经 `request_execute` 信号的第四个
  参数跨线程传给 `Worker`；`CancelToken` 拷贝共享同一 `std::atomic<bool>`，已注册 metatype；
- UI 线程点击「取消」只调用 `active_cancel_.request_cancel()`：无锁、非阻塞，可以在信号
  处理器里安全执行，不等待工作线程；
- 状态机：执行中按钮可用 → 点击后立即禁用并显示「正在取消」→ 收到结果后恢复初始状态；
  `close` 进行中不提供取消（关闭走 cleanup 语义，不复用执行令牌）；
- 结果按既有 `kCancelled` 映射渲染为「已取消」，不携带 outcome，也不表示状态未知。

### 17.2 验证

- `tinydbms.gui_ui` 新增 `cancel_button_requests_cancellation`：用测试闸门把 fake 后端停在
  执行中，真实点击按钮，断言令牌在 worker 线程被观察到、按钮与状态栏进入取消态、结果区显示
  「已取消」；
- `build/gui-debug` 全量 63/63 通过（`tinydbms.gui_ui` 内部 21 个用例）。

## 18. 第二版收尾：渲染与诊断路径优化（2026-09-16）

状态：**已实现并验收**。公共契约无变化，界面行为不变；本节只消除随语句数或文本长度增长的
重复工作，并给 GUI 内部的复杂度留出与 core 上限匹配的余量。

### 18.1 范围

四项改动都属于「同样的输入，同样的输出，更少的重复计算」：

| 位置 | 原实现 | 现实现 |
| --- | --- | --- |
| `result_panel.cpp` | 每条带查询结果的语句都调用一次 `ResultModel::set_query`，每条都触发一次模型重置 | 先确定「最后一条查询结果」，每次刷新只重置一次模型 |
| `result_panel.cpp` | 表格沿用默认列宽，较长的值整列以省略号呈现 | 小结果（≤200 行、≤16 列）按内容自适应，列宽夹在 60–320 px；大结果保持 100 px 固定宽度 |
| `schema_browser.cpp` | 每个列行线性扫描顶层表节点，复杂度 O(列行数 × 表数) | 表行建立 `table_id → 顶层节点` 的哈希索引，列行 O(1) 定位父节点 |
| `source_mapping.cpp` | 每个诊断范围从头扫描文本，复杂度 O(范围数 × 文本长度) | 新增 `editor_ranges`：所有范围排序后一次遍历，O(文本长度 + 范围数 log 范围数) |
| `script_editor.cpp` / `main_window.cpp` | 每次控件状态更新都 `toPlainText().trimmed()`，复制整篇文档 | `has_executable_text()` 按需计算并缓存，文本变化时才失效 |

`--error-policy analyze` 会为脚本里每条语句各产生一个诊断范围，脚本上限 1 MiB / 语句数上限
由 core 决定，因此前两项的线性扫描在极端输入下会被同时放大；`has_executable_text()` 则出现
在每次编辑与每次控件状态更新上。

### 18.2 正确性约束

- `editor_ranges` 与逐个调用 `editor_range` 必须逐项一致：字符内部偏移归位到字符起点、
  越界返回 `nullopt`、`end < begin` 返回 `nullopt`、空文本只有偏移 0 合法；
- `has_executable_text()` 等价于 `!toPlainText().trimmed().isEmpty()`，缓存必须在
  `textChanged` 时失效，高亮与「执行」按钮状态不得读到过期结果；
- 模型重置次数不随语句数增长，且仍然展示最后一条查询结果；没有查询结果时模型必须清空。

### 18.3 验证

- `tinydbms.gui_ui` 新增 `result_panel_single_model_reset`：三条语句（含两条查询）只触发一次
  `modelReset`，模型指向最后一条查询；纯命令脚本与 `show_notice` 同样各只重置一次；
- `tinydbms.gui_ui` 新增 `editor_executable_text_and_diagnostics`：空文本、纯空白与非空文本的
  缓存切换，越界诊断范围被忽略，清空高亮后选区数为 0；
- `utf8_offset_mapping` 扩展为对 6 种文本逐一遍历所有起止字节偏移，断言批量换算与逐个换算
  结果完全一致；
- `result_panel_column_widths`：400 字符的长值被钳制到最大列宽 320 px，300 行的结果走固定
  100 px 路径，保证 `resizeColumnsToContents` 的测量成本不会随结果规模无界增长；
- 换算路径另用临时 Release 微基准测量（1 MiB 脚本、2000 个诊断范围、Release 构建）：
  逐个 `editor_range` 1504 ms，`editor_ranges` 1.40 ms，两者结果逐项一致。该数字只说明
  纯函数的复杂度差异，不代表界面实际耗时；
- 另用离屏渲染核对了实际界面：编辑器高亮、诊断列表、结果表格、表浏览器与状态栏在
  `QT_QPA_PLATFORM=offscreen` 下的截图与预期一致；
- `build/gui-debug` 全量 63/63 通过（`tinydbms.gui_ui` 内部 25 个用例）。

## 19. 第二版收尾：计划模式、结果上限与结果导出（2026-09-16）

状态：**已实现并验收**。三项都复用公共契约里已有的字段（`ExecutionMode::kPlanOnly`、
`ExecuteScriptRequest::max_query_rows`、`QueryResult`），`include/`、根 `CMakeLists.txt`
与 `tests/CMakeLists.txt` 均无改动，不产生需要通知其他成员的事项；依赖边界仍是 Qt6 Widgets。

### 19.1 计划模式开关

- 工具栏新增复选框「只编译，不执行（查看计划）」；勾选后执行请求带
  `ExecutionMode::kPlanOnly`，core 只编译并返回单列 `plan` 文本，不调用 storage、不修改 Catalog；
- 「执行」按钮在该模式下改文案为「查看计划 (Ctrl+Enter)」，快捷键与启用条件不变；
- 结果区按 §5 的 `kPlanOnly` 映射渲染计划文本，语句摘要写「计划 N 行文本（未执行）」，
  状态栏临时消息为「已生成计划，未执行语句，用时 N ms」，与「执行完成」明确区分；
- 表结构刷新固定走 `kExecute` + 默认上限，不受该开关影响，否则面板只会拿到一份计划文本；
- `kPlanOnly` 不产生 `script_error`，也不改变 `next_session_state` 的判定，会话状态机不变。

### 19.2 结果上限档位

- 工具栏新增下拉框，档位为「默认（`kMaxQueryRows` = 262144）」「1000」「10000」「100000」；
  默认档直接取 `core::kMaxQueryRows`，不写死数字，避免与契约漂移；
- 选中值直接进 `ExecuteScriptRequest::max_query_rows`；取不到数据或为 `0` 时回退默认档，
  请求里不出现非法值；
- 语义与 CLI 的 `--max-rows` 完全一致：只约束单条语句在内存中物化的行数，超限是执行错误；
- 未打开数据库时下拉框禁用，忙碌期间与执行按钮一起禁用。

### 19.3 结果导出与复制

- 结果区表格页改为「按钮行 + 表格」，新增「导出 CSV」与「复制」；没有查询结果时两者禁用；
- 取值口径与表格一致（复用 `value_text`），缺失单元格按空字段导出，不中断整份导出；
- CSV 按 RFC 4180：字段含分隔符、双引号或 CR/LF 时整体加引号，内部 `"` 写成 `""`，
  行尾为 CRLF；写盘内容为 UTF-8 且带 BOM，Excel 双击打开中文不乱码；空结果导出为空字节；
- 剪贴板用 TSV（制表符分隔、LF 行尾、不带 BOM），便于直接粘进表格软件；
- 导出只写用户选择的路径，失败弹一次提示；不修改结果区内容，也不触碰数据库文件；
- 写盘用 `QSaveFile`（先写临时文件，`commit` 成功才替换目标），失败不会留下半份结果；
- 渲染是纯函数（`result_export.{hpp,cpp}`），不依赖窗口，可单独单测。

### 19.4 表结构面板的系统表脚本修复

验收过程中发现真实后端下一个此前被 fake 掩盖的缺陷：「表结构」面板始终显示
「无法读取系统表」。根因是刷新脚本 `SELECT * FROM tdb_sys_tables; SELECT * FROM
tdb_sys_columns` 缺少结尾分号——分句契约要求每条语句（含末条）以 `;` 结束，compiler 因此
把第二条语句报成 syntax error，而 fake 后端按预置结果返回，测试看不到该行为。

修复方式：

- 脚本移入不依赖 Qt 的 `schema_query.hpp` 并补上结尾 `;`，界面与联调测试引用同一份文本；
- `tests/gui_core_backend_test.cpp` 把该文本交给真实 compiler，断言两条语句都是 `kExecuted`
  且都返回 `QueryResult`：空库 0 行、建表后 1 张表 2 列；
- `tests/gui_ui_test.cpp` 断言刷新请求文本与常量逐字节一致且以 `;` 结尾。

### 19.5 验证

- `build/gui-debug`：全量 63/63 通过，`tinydbms.gui_ui` 在 §19 收尾时为 27 个用例
  （含计划模式、结果上限、导出与剪贴板、系统表脚本文本），§20 增补后为 32 个用例；
- `build/mergecheck`（真实 compiler + storage + GUI）：全量 66/66 通过，
  `tinydbms.gui_core_backend` 覆盖 GUI 真实后端，含新增的系统表脚本用例；
- `build/sanitize-gui`：`gui` 标签 1/1 通过，界面测试在 ASan/UBSan 下无报错；
- 全量重编 GUI 相关源文件（`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion`）
  无新增告警；
- 另用接真实 compiler/storage 的离屏 shell 程序跑通并截图核对：常规查询（3 行结果、
  状态栏「执行完成，用时 N ms」）、计划模式（`Project → Sort → Filter → SeqScan` 计划文本、
  状态栏「已生成计划，未执行语句」）、以及表结构面板读到系统表内容。

## 20. 全面验收补强（2026-09-16）

状态：**已实现并验收**。本节记录对 GUI 全部用户路径的离屏验收，以及验收中发现的三个
交互缺陷。公共契约、构建依赖和界面结构不变。

### 20.1 验收范围

- 真实编译器与存储链路：打开空库、建表写入、查询表格、数值图表、无数值列空状态、NULL、
  计划模式、计划无副作用、结果上限超限与恢复、编译诊断、fix-it、重新执行、analyze、
  空编辑器、CSV 导出、重开库后的表结构、900×560 小窗口、关闭；
- fake 后端补齐真实链路难以稳定制造的状态：执行中取消、正在取消、取消完成、内部错误、
  会话重开、打开失败、关闭失败、命令与查询混合脚本；
- 共保存并逐项核对 27 张 1440×900（小窗口为 900×560）离屏截图。截图工装只位于
  `/tmp/gui-drive`，不进入仓库。

### 20.2 验收中修复的缺陷

1. **旧诊断可能重新高亮到新文本**：执行期间用户修改编辑器后，结果仍会按旧快照的字节范围
   调用 `apply_diagnostics`。现在只有 `editor_->toPlainText()` 与执行快照完全一致时才应用
   高亮与 fix-it，否则清空高亮，诊断文字仍保留供参考；
2. **计划成功的文案掩盖语句失败**：脚本同时含成功 plan 与编译错误时，旧状态栏显示
   「已生成计划」，与结果区里的失败诊断矛盾。现在优先级为脚本错误、语句失败、成功计划、
   正常完成，计划成功文案不会覆盖失败；
3. **表结构刷新按钮在不可用状态仍显示可用**：会话未打开、执行期间或 `kNeedsReopen` 时，
   点击刷新会被状态机静默忽略，但按钮仍可点击。现在按钮随 `open && !busy` 精确启停，
   与 §3.5 的「禁用执行与刷新」一致。

### 20.3 回归与验证

- `tinydbms.gui_ui` 新增 `stale_result_does_not_rehighlight_editor`、
  `plan_status_prioritizes_errors`，并扩展打开、内部错误和打开失败用例的刷新按钮状态断言；
- `build/gui-debug`：全量 63/63 通过，`tinydbms.gui_ui` 内部 32 个用例；
- `build/mergecheck`：真实 compiler + storage + GUI 全量 66/66 通过；
- `build/sanitize-gui`：`gui` 标签 1/1 通过，ASan/UBSan 无报错；
- 另用 `/tmp/tinydbms-gui-warnings` 开启 `-Werror` 和
  `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion` 全量构建 GUI 与测试目标，通过；
- 重新运行 27 张截图场景，计划失败状态栏与刷新按钮禁用状态已在截图中确认。

本机 GUI 与 TSan 的组合未能得到有效结论：Qt6 预编译库触发的跨线程事件队列报告与
ThreadSanitizer 的 `sanitizer_thread_registry.cpp` 运行时断言同时出现，该问题在 Qt 事件
队列之外不可复现。并发正确性仍以现有 `core_concurrency`、取消闸门用例和 ASan/UBSan 结果
为准；后续如要补 GUI 竞态检测，需要可用的 Qt TSan 运行时或单独的最小复现工装。
