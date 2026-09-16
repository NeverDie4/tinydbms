# CLI 升级设计：展示格式与性能观测

状态：**P1/P2/P3 已实现**（2026-09-16，P3 位于 `feat/p3-buffer-stats` 分支）。
实现位置：`src/app/arguments.{hpp,cpp}`（`--format pretty`、`--time`、`--stats`）、
`src/app/output.{hpp,cpp}`（格式分派与计时行）、`src/app/pretty_output.{hpp,cpp}`
（pretty 渲染器）、`src/app/value_text.{hpp,cpp}`（值文本化）、`src/app/runner.cpp`
（默认格式选择、计时与统计输出）、`src/app/terminal.{hpp,cpp}`（stdout 终端判定）、
`include/tinydbms/storage.hpp` + `src/storage/storage.cpp`（`storage_stats()`）、
`include/tinydbms/core.hpp` + `src/core/database.cpp`（`Database::storage_stats()`）、
`src/app/session*.cpp`（`Session::storage_stats()` 转发）。

## 1. 背景与范围

目标：解决两个入口体验问题，并为"这条语句到底走了多少次磁盘"提供可观测手段。

- 现有默认输出是制表符分隔文本（TSV），适合脚本，但在终端里读起来很吃力：列不对齐、
  没有边框、没有行数提示；
- 用户无法从 CLI 看到任何执行代价信息：既没有墙钟耗时，也没有页缓存命中情况。

包含：

- P1 `--format pretty`：人读的等宽表格；
- P2 `--time`：每次 `execute_script` 调用的墙钟耗时；
- P3 `--stats`：每次执行的 buffer pool 命中/缺失汇总（**跨模块**：storage + core + CLI）。

不包含：

- 改动 `table`（TSV）与 `json`（NDJSON）的既有字节级输出——两者已是稳定契约，
  被脚本、golden 测试与 GUI 复用；
- ANSI 颜色、进度条、分页交互；
- 逐语句计时（core 只暴露调用级时间，见 §4.3）；
- 真正意义上的"缺页率"（操作系统缺页）——这里指 **buffer pool 的页缺失率**，见 §5.1。

## 2. 现状基线

| 位置 | 现状 |
| --- | --- |
| `src/app/output.cpp` | `write_header`/`write_row` 固定输出制表符文本；`escape_text` 转义 `\\`、`\t`、`\n`、`\r` |
| `src/app/json_output.cpp` | NDJSON，stdout 只写结果对象、stderr 只写诊断对象 |
| `src/app/arguments.cpp` | `--format table\|json`，最多一次，默认 `table` |
| `src/app/runner.cpp` | `ExecutionOptions` 只有 format/mode/policy/max_query_rows；没有计时 |
| `src/app/terminal.cpp` | 只有 `stdin_is_terminal()`，没有 stdout 判定 |
| `src/storage/buffer_pool.h` | 有 `BufferPoolStats`（fetch/hit/miss/eviction/dirty_flush 等）与 `hit_rate()`，但 `stats()` 是 `src/` 内部头，公共契约没有出口 |
| `include/tinydbms/storage.hpp` | 只有 open/close/表 CRUD/游标；没有任何统计请求 |

## 3. P1：`--format pretty`

### 3.1 默认格式选择

`--format` 未显式给出时按输出目标决定：

- stdout 是终端 → `pretty`；
- stdout 被重定向或接管道 → `table`（现状不变）。

显式给出 `--format` 时永远以显式值为准，终端判定不参与。

判定发生在 `runner` 层，依据 `CliEnvironment::output_is_terminal`（由 `main` 用
`isatty(STDOUT_FILENO)` 填入）。测试与 GUI 不填该字段时默认为 false，
所以"注入输出流"的既有测试仍然走 `table`，行为零变化。

理由：TSV 是脚本契约，不能因为"跑在终端上"就悄悄换掉；反过来，人坐在终端前时
默认给出好看的表格才是合理的默认。

### 3.2 渲染规则

查询结果：

```text
┌────┬────────┬───────┐
│ id │ name   │ score │
├────┼────────┼───────┤
│  1 │ 一甲   │  98.5 │
│  2 │ 乙     │  NULL │
└────┴────────┴───────┘
2 rows
```

固定规则：

1. 边框使用 `┌ ┬ ┐ ├ ┼ ┤ └ ┴ ┘ ─ │`（U+2500 区块），每个单元格左右各留一个空格；
2. 列宽 = 该列"表头 + 全部单元格"的最大**显示宽度**，至少为 1；
3. **显示宽度**：CJK/全角等东亚宽字符记 2 列，组合附标与零宽字符记 0 列，其余记 1 列；
   非法 UTF-8 字节记 1 列（不越界、不抛异常）；
4. 数值列（该列全部非 NULL 值都是 INT/BIGINT/DOUBLE）右对齐，其余列左对齐；表头与列同向；
5. 单元格文本与 `table` 模式同源：`escape_text(value_text(v))`，NULL 渲染为 `NULL`；
6. 单元格**显示宽度**超过 48 列时按显示宽度截断并追加 `…`（U+2026）。截断只发生在
   pretty，`table`/`json` 永远输出完整值；需要完整数据时用那两种格式；
7. 表尾固定一行行数：`0 rows` / `1 row` / `N rows`；
8. 空结果仍然打印表头与边框，只省略数据行。
9. **计划模式例外**：`--plan` 的语句结果（`StatementStatus::kPlanOnly`）是缩进文本而不是
   表格数据——`columns=[…]` 这类行经常超过 48 列，截断会直接丢列映射；`N rows` 也只反映
   “计划文本有几行”而非结果集大小。因此计划模式关闭单元格截断与行数行，普通查询结果不变。

命令结果：单行 `OK, N rows affected`（N 为 1 时为 `1 row affected`）。

诊断与状态（`ERROR`/`SUGGESTION`/`FIX`/`ANALYZED`/`SKIPPED`/`CANCELLED`/
`INDETERMINATE`）与 `table` 模式字节级一致，继续写 stderr，不加颜色、不加边框。
这样"pretty 只改变成功结果的呈现"，诊断输出的测试与脚本不受影响。

### 3.3 契约边界

- pretty 输出是**展示文本，不是解析契约**：不承诺机器可解析，字段顺序、边框字符属于实现细节，
  可以随版本调整；需要稳定解析请用 `table` 或 `json`；
- pretty 与 `--plan` 正交：`--plan` 在 pretty 下渲染成单列（列名 `plan`）的同一套表格，
  按 §3.2 第 9 条关闭单元格截断与行数行；
- pretty 与 `--max-rows`、`--error-policy`、取消、批处理/REPL 的关系与 `table` 完全一致。

## 4. P2：`--time`

### 4.1 语义

`--time` 打开后，**每次 `session.execute_script` 调用**在 stderr 追加一行：

```text
TIME script 12.345 ms
TIME line 3 0.412 ms
```

- 稳定前缀 `TIME `，稳定后缀 `<值> ms`，值固定三位小数毫秒；
- 批处理一次调用覆盖整段脚本，scope 为 `script`；
- REPL 每一行一次调用，scope 为 `line <序号>`，序号从 1 开始、按读取到的行计数；
- `--time` 最多出现一次；
- 该行写在本次调用的结果渲染之后（stderr 顺序：先诊断、后 `TIME`）；
- 只写 stderr，绝不写 stdout：`--format table|json` 的字节流不变，重定向 stdout 的脚本不受影响。

### 4.2 计时口径

用 `std::chrono::steady_clock` 只包住 `execute_script` 调用本身，不包含：

- `open`/`close`（生命周期错误与耗时是两件事；`--time` 不改变它们的输出）；
- 输入读取、结果渲染、流刷新。

因此该值可直接与"core 执行耗时"对齐，是跨入口（CLI/GUI/测试）可比较的口径。

### 4.3 不做的部分

- 不提供逐语句计时：`ExecuteScriptResult` 没有每语句的时间戳，加字段属于 `core.hpp` 变更；
  当前粒度是调用级，REPL 下等价于语句级。
- 不提供累计统计（总耗时、平均耗时）：入口层不值得为此维护会话级状态。

## 5. P3：buffer pool 统计（缺页率）——已落地

### 5.1 术语

这里的"缺页率"指 **buffer pool 页缺失率**：一次 `fetch_page` 未在内存帧中命中、
需要读盘的比率，等价于 `miss_count / fetch_count`。它不是操作系统的 minor/major page fault，
CLI 无法从 `/proc` 侧拿到对教学有意义的口径。

### 5.2 原有阻塞

- 数据一直存在：`src/storage/buffer_pool.h` 的 `BufferPoolStats` 有
  `fetch_count`/`hit_count`/`miss_count`/`dirty_flush_count`/`eviction_count`/`free_frame_miss_count`
  与 `hit_rate()`；
- 阻塞点是它只位于 `src/` 内部头，`include/tinydbms/storage.hpp` 没有统计出口，
  core 取不到值，CLI/GUI 也无法显示；
- CLI 只依赖 `app::Session`（core），不能绕过 core 直接调 storage：占位构建（不可用 Session）、
  fake 测试与 GUI 后端都建立在这条边界上。

阻塞已通过下述公共契约解除。

### 5.3 落地形态（storage → core → CLI）

在 `include/tinydbms/storage.hpp` 增加一组请求/结果类型，风格与现有 API 一致：

```cpp
struct StorageStatsRequest {};

struct StorageStats {
    std::uint64_t fetch_count = 0;
    std::uint64_t hit_count = 0;
    std::uint64_t miss_count = 0;
    std::uint64_t eviction_count = 0;
    std::uint64_t dirty_flush_count = 0;
    double hit_rate() const noexcept;   // fetch_count == 0 时为 0
};

struct StorageStatsResult {
    std::optional<StorageStats> stats;
    std::optional<StorageError> error;
};

StorageStatsResult storage_stats(const StorageStatsRequest& request);
```

语义：

1. 计数器是**进程级累计值**，从最近一次成功的 `open_storage` 起算，`close_storage` 后归零；
2. 未打开时返回 `StorageErrorKind::kInvalidRequest`，不是空 stats；
3. 只有 buffer pool 的命中口径，不承诺包含预取 worker 的内部读；
4. 该调用不得抛异常，也不得改变任何存储状态（只读快照）。

core 侧对应 `core::StorageStats` / `core::StorageStatsResult` 与
`Database::storage_stats()`：同一把 `Impl::mutex` 下取快照，未打开（含 cleanup-pending）
返回 `kExecute`，storage 错误映射为 `kStorage`，异常收敛为 `kInternal`；
统计失败只影响观测，不影响执行结果。入口层经
`Session::storage_stats()`（`CoreSession` 转发、占位构建返回不可用错误）取数。

展示形态：

```text
BUFFER fetch=128 hit=120 miss=8 miss_rate=6.25% evictions=3 flushes=2
```

由 `--stats` 触发，通道与 `--time` 一致（stderr），两者可同时使用（`--time` 行在前）。
批处理每次调用一条 `BUFFER script` 快照、REPL 每行一条，取值都在 close 之前完成；
`miss_rate` 固定两位小数，`fetch_count == 0` 时是 `0.00%`。统计失败写成一行
`ERROR …` 诊断，不改变 stdout 与退出码。

### 5.4 逐页事件日志（storage 侧已落地）

`origin/codex/log` 分支的 `73cbf5a`（PR #7）把 buffer pool 的事件日志改成结构化枚举
（`kHit/kMiss/kLoad/kEvict/kFlush` + `FlushReason`），并在 `storage.cpp` 里用
`TINYDBMS_BUFFER_EVENT_LOG=1` 装配 `LogSink`，输出 `[BUFFER][HIT] table=… page=… frame=… policy=…`
到 `std::clog`（stderr），默认关闭。

PR #7 已合入 main；`TINYDBMS_BUFFER_EVENT_LOG`、事件字段、reason 取值和
demand/prefetch 边界已登记到 [storage 契约](../storage/storage-contract.md)。
默认构建仍然不输出事件日志。

它与计划中的 `--stats` 是两种观察出口：`--stats` 给汇总计数，事件日志给 demand
逐页轨迹。事件日志明确不记录预取 worker 内部事件；后者由 prefetch_* 计数观察。

### 5.5 PR #7（`codex/log`）审查与处理结果

审查时间 2026-09-16；PR #7 已合入，以下 4 项已按最保守口径处理：

1. **日志口径 ≠ 统计口径**。HIT/MISS 日志与 `stats_.hit_count`/`miss_count` 已统一为
   demand 口径；`fetch_count` 仍只统计 demand 调用，预取 worker 的内部事件继续只由
   `prefetch_*` 与 `useful_prefetch` 计数观察。于是日志中的 demand HIT/MISS 可与
   `--stats` 的数值互相印证。
2. **枚举兜底会终止进程**。`event_name` / `flush_reason_name` 已改为对未知取值返回
   `UNKNOWN`，格式化辅助函数不再因新增枚举而直接终止进程。
3. **EVICT 的 `dirty=` 是死值**。淘汰路径已在提交前快照 `old_dirty`，事件现在报告
   victim 被替换时的真实脏页状态。
4. **契约文档未同步**。`TINYDBMS_BUFFER_EVENT_LOG`、事件字段、`FlushReason` 取值与
   demand/prefetch 边界已写入 [storage 契约](../storage/storage-contract.md)，
   `docs/storage/buffer-pool.md` 也改用新的结构化事件名称。

## 6. 需要通知其他模块

- **storage**：
  1. `include/tinydbms/storage.hpp` 与 `src/storage/storage.cpp` 已按 §5.3 落地
     `StorageStatsRequest/StorageStats/StorageStatsResult` 与 `storage_stats()`，
     请评审这组新增契约与四条语义（累计与清零、未打开的错误、统计口径、只读无异常）；
  2. PR #7 的日志口径与契约同步已在 §5.5 记录完成；
  3. 该变更在 `feat/p3-buffer-stats` 分支上，合入 main 前需要 storage 侧确认。
- **compiler**：无。本轮不涉及 SQL 语法、Plan 结构或 `compiler.hpp`。
- **GUI**：无。`--format pretty`、`--time` 与 `--stats` 只作用于 CLI；GUI 有自己的表格控件
  与耗时显示，本轮不接入 buffer pool 统计（需要时可直接调用 `Database::storage_stats()`）。

## 7. 验收

1. `--format table` 与 `--format json` 的字节级输出与改动前一致（既有 golden 测试全绿）；
2. 未显式指定 `--format` 且 stdout 不是终端时，默认仍是 TSV；
3. `--format pretty` 的查询、空结果、NULL、CJK 宽度、超宽截断、命令结果都有 golden 断言；
4. `--time` 打开后 stdout 与未打开时逐字节相同，只有 stderr 多出 `TIME …` 行；
5. `--format` 出现 `yaml` 等未知值、重复出现仍是退出码 2；`--format pretty` 被接受；
6. 真实链路（`TINYDBMS_ENABLE_REAL_MODULES=ON`）下 pretty + `--time` 的 CREATE/INSERT/SELECT
   批处理可运行，退出码 0。
7. `--stats` 打开后 stdout 与未打开时逐字节相同，stderr 每次执行多出一行
   `BUFFER fetch=… hit=… miss=… miss_rate=…% evictions=… flushes=…`；
   真实链路上 REPL 的第二次相同查询只增加命中、不增加缺失，统计失败（storage 报错或抛异常）
   不改变退出码。
