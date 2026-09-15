# CLI 升级设计：展示格式与性能观测

状态：**P1/P2 已实现，P3 待 storage 提供接口**（2026-09-16）。
实现位置：`src/app/arguments.{hpp,cpp}`（`--format pretty`、`--time`）、
`src/app/output.{hpp,cpp}`（格式分派与计时行）、`src/app/pretty_output.{hpp,cpp}`
（pretty 渲染器）、`src/app/value_text.{hpp,cpp}`（值文本化）、`src/app/runner.cpp`
（默认格式选择与计时）、`src/app/terminal.{hpp,cpp}`（stdout 终端判定）。

## 1. 背景与范围

目标：解决两个入口体验问题，并为"这条语句到底走了多少次磁盘"提供可观测手段。

- 现有默认输出是制表符分隔文本（TSV），适合脚本，但在终端里读起来很吃力：列不对齐、
  没有边框、没有行数提示；
- 用户无法从 CLI 看到任何执行代价信息：既没有墙钟耗时，也没有页缓存命中情况。

包含：

- P1 `--format pretty`：人读的等宽表格；
- P2 `--time`：每次 `execute_script` 调用的墙钟耗时；
- P3 缓存命中/缺页统计（**跨模块**，本轮只出提案与通知，不实现）。

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

## 5. P3：buffer pool 统计（缺页率）——待 storage 落地

### 5.1 术语

这里的"缺页率"指 **buffer pool 页缺失率**：一次 `fetch_page` 未在内存帧中命中、
需要读盘的比率，等价于 `miss_count / fetch_count`。它不是操作系统的 minor/major page fault，
CLI 无法从 `/proc` 侧拿到对教学有意义的口径。

### 5.2 现状与阻塞

- 数据已经存在：`src/storage/buffer_pool.h` 的 `BufferPoolStats` 有
  `fetch_count`/`hit_count`/`miss_count`/`dirty_flush_count`/`eviction_count`/`free_frame_miss_count`
  与 `hit_rate()`；
- 但 `BufferPool::stats()` 位于 `src/` 内部头，`include/tinydbms/storage.hpp` 没有任何统计出口；
- core 因此无法取到统计值，CLI/GUI 也不可能显示；
- CLI 只依赖 `app::Session`（core），不能绕过 core 直接调 storage：占位构建（不可用 Session）、
  fake 测试与 GUI 后端都建立在这条边界上。

**结论：缺页率当前不可实现，阻塞点是 storage 公共 API。**

### 5.3 提案（提交给 storage 成员）

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

语义要求（需要 storage 明确确认）：

1. 计数器是**进程级累计值**，从最近一次成功的 `open_storage` 起算，`close_storage` 后归零；
2. 未打开时返回 `StorageErrorKind::kInvalidRequest`，不是空 stats；
3. 只有 buffer pool 的命中口径，不承诺包含预取 worker 的内部读；
4. 该调用不得抛异常，也不得改变任何存储状态（只读快照）。

落地后 core 侧再加 `Database` 的查询接口（`core.hpp` 变更），CLI/GUI 才能显示。
入口的展示形态预留为：

```text
BUFFER fetch=128 hit=120 miss=8 miss_rate=6.25% evictions=3 flushes=2
```

由 `--stats` 触发，通道与 `--time` 一致（stderr），两者可同时使用。

### 5.4 逐页事件日志（storage 侧已有实现，未并入 main）

`origin/codex/log` 分支的 `73cbf5a`（PR #7）把 buffer pool 的事件日志改成结构化枚举
（`kHit/kMiss/kLoad/kEvict/kFlush` + `FlushReason`），并在 `storage.cpp` 里用
`TINYDBMS_BUFFER_EVENT_LOG=1` 装配 `LogSink`，输出 `[BUFFER][HIT] table=… page=… frame=… policy=…`
到 `std::clog`（stderr），默认关闭。

现状：main 里已有 `LogSink` 与 `log_event` 机制，但 `storage.cpp` 的
`BufferPool::create` 调用**没有传 sink**，所以默认构建下不会输出任何日志；PR #7 尚未合并，
`docs/storage/storage-contract.md` 也没有登记这个开关。

它与 `--stats` 是同一批数据的两种出口：`--stats` 给汇总计数，事件日志给逐页轨迹。
两者都归 storage 契约，需要 storage 成员确认后再改入口。

### 5.5 PR #7（`codex/log`）审查结论

审查时间 2026-09-16，实测记录：

- base 是旧 main（`4e66b97`，落后当前 main 7 个提交），但合入当前 main 无冲突；
- `-DTINYDBMS_ENABLE_REAL_MODULES=ON` 构建成功，`ctest` **65/65 通过**；
- 严格警告（`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion`）零警告；
- 开关在真实链路生效，实测输出包含
  `[BUFFER][MISS] table=1 page=1`、`[BUFFER][LOAD] table=1 page=1 frame=0`、
  `[BUFFER][FLUSH] table=0 page=1 frame=1 reason=explicit status=success`。

以下 4 项需 storage 处理，都不影响当前正确性，属口径与健壮性：

1. **日志口径 ≠ 统计口径**。main 的 HIT/MISS/Evict 是无条件记录的，PR #7 改成
   `if (!prefetch && log_)`；而 `stats_.hit_count` 仍计入预取 worker 的命中
   （`miss_count` 却排除预取 worker 的缺失，`fetch_count` 只统计 demand 调用）。
   于是"数日志行数"与 `--stats` 的 `hit_rate` 会对不上，需要 storage 明确两者口径，
   或让两者一致。
2. **`event_name` / `flush_reason_name` 用 `std::terminate()` 兜底**。给枚举新增取值
   却忘记改映射会直接终止进程；这两处是格式化辅助函数而非不变量断言，建议
   `return "UNKNOWN";`。
3. **EVICT 的 `dirty=` 是死值**。干净 victim 的提交路径在记录事件之前已把
   `target.dirty` 置为 `false`，随后 `log_event(…, target.dirty)` 读到的恒为 `false`。
   当前该路径只接受干净 victim（`can_commit` 要求 `!target.dirty`），结果正确，
   但建议改成显式常量或提前快照，避免路径扩展后误报。
4. **契约文档未同步**。`TINYDBMS_BUFFER_EVENT_LOG`、`[BUFFER][…]` 文本格式与
   `FlushReason` 取值均未写进 `docs/storage/storage-contract.md`；
   `docs/storage/buffer-pool.md` 第 87 行仍写着旧的 `Buffer HIT / Buffer MISS / Evict /
   Flush dirty` 命名，合并前应一并更新。

## 6. 需要通知其他模块

- **storage**：
  1. 请评估 §5.3 的 `StorageStatsRequest/StorageStats/StorageStatsResult` 与 `storage_stats()`；
  2. 请确认 §5.3 的四条语义（累计与清零、未打开的错误、统计口径、只读无异常）；
  3. 请合并 `codex/log`（PR #7），处理 §5.5 的四项，并把 `TINYDBMS_BUFFER_EVENT_LOG`
     与 `[BUFFER][…]` 格式写进 `docs/storage/storage-contract.md`；
  4. 本轮 core/cli 不改 `include/tinydbms/storage.hpp`，等 storage 落地 `storage_stats()`
     之后再动。
- **compiler**：无。本轮不涉及 SQL 语法、Plan 结构或 `compiler.hpp`。
- **GUI**：无。`--format pretty` 与 `--time` 只作用于 CLI；GUI 有自己的表格控件与耗时显示。

## 7. 验收

1. `--format table` 与 `--format json` 的字节级输出与改动前一致（既有 golden 测试全绿）；
2. 未显式指定 `--format` 且 stdout 不是终端时，默认仍是 TSV；
3. `--format pretty` 的查询、空结果、NULL、CJK 宽度、超宽截断、命令结果都有 golden 断言；
4. `--time` 打开后 stdout 与未打开时逐字节相同，只有 stderr 多出 `TIME …` 行；
5. `--format` 出现 `yaml` 等未知值、重复出现仍是退出码 2；`--format pretty` 被接受；
6. 真实链路（`TINYDBMS_ENABLE_REAL_MODULES=ON`）下 pretty + `--time` 的 CREATE/INSERT/SELECT
   批处理可运行，退出码 0。
