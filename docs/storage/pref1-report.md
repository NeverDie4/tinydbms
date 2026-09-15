# PREF-1: Controlled Sequential Background Prefetch

本轮实现与验收日期：2026-09-15。正确性检查通过；本机两个 CPU profile 均出现 elapsed 退化，默认保持关闭，停止在 Storage microbenchmark 阶段。

## A. Files Changed

本轮修改：

- `src/storage/buffer_pool.h`：Storage-private 开关、origin、usefulness metadata、worker 和 metrics。
- `src/storage/buffer_pool.cpp`：单 worker、advisory submission、共享 load protocol、demand retry、shutdown/restart。
- `src/storage/heap_table.cpp`：当前页开始扫描时提交 distance-1 候选。
- `src/storage/CMakeLists.txt`：新增 benchmark target。
- `tests/CMakeLists.txt`：新增 prefetch CTest target，TIMEOUT=60。

本轮新增：

- `src/storage/prefetch_benchmark.cpp`。
- `tests/buffer_pool_prefetch_test.cpp`。
- 本报告、`pref1-benchmark-raw.csv`、`pref1-benchmark-summary.txt`。

已有 CONC 系列未提交改动保持在原工作区。上述列表只描述本轮增量；`git status` 中已有的 FileManager、PageFile、其他测试改动不是本轮新增。`include/`、Compiler、Core、App 没有改动；没有 commit/push。

## B. Worker Architecture

BufferPool 拥有一个 `std::thread`，状态为 RUNNING → STOPPING → STOPPED。默认 construction 参数 `prefetch_enabled=false`；benchmark/test 通过内部 `BufferPool::create(..., true)` 启用。实验 benchmark 还必须显式设置 CMake `-DTINYDBMS_BUILD_STORAGE_BENCHMARKS=ON`，默认构建不生成它们。没有新增 FrameState，仍为 FREE/LOADING/READY；`LoadCompletion::origin` 区分 Demand/Prefetch。

沿现有 HeapTable → RecordPage → BufferPool → PageFile 调用链实现。架构参考仓库 `docs/miniob-study/storage-engine.md` 的 record/page/buffer 分层；采用 tinydbms 现有 CONC 协议，不引入 MiniOB 的日志、事务或调度器。磁盘格式没有变化。

## C. Queue / Submission Semantics

队列固定容量 2，构造时 reserve，提交不分配队列存储。`prefetch_page(PageKey) noexcept` 使用 `try_lock`；mutex 竞争、禁用、停止、无效 page id 或队列满直接 drop。提交去重覆盖 queued 和 active key；worker 在 metadata mutex 下再次检查 READY、LOADING、`loading_table_`。不会等待队列空间或 I/O。

提交失败不会向 HeapTable 返回错误。`requested` 统计尝试调用，因此 OFF 的调用也计入 requested/dropped；ON/OFF 都使用相同扫描触发点。

## D. HeapTable Trigger

`HeapTable::next_record` 成功取得当前 allocated 页的 PageGuard 后，在 `candidate.next_slot == 0` 时提交 `current_page + 1`，随后进入页内 RecordPage scan。candidate 必须小于 cursor 固化的 `page_end_exclusive`；加法使用原有宽类型，并在验证后转换 PageId。

## E. Allocation-State Handling

worker 先取得 PageFileLease，锁外调用 `page_allocation_state(candidate)`。FREE 候选直接 drop，并增加 `prefetch_free_page_drop`，不创建 reservation，不算 requested-page read failure。worker 不向 N+2/N+3 探测，也不因 page_count 后续增长扩展边界。

## F. FREE-Frame Prefetch

复用 FREE → LOADING reservation 和 completion ticket，锁外读取，锁内验证后发布 READY。LOADING 阶段保留原协议的内部 reservation pin；成功 publication 时 prefetch 的 pin_count 为 0，无 PageGuard；demand 的 pin_count 为 1，并返回 Guard。失败恢复 FREE 并唤醒等待者。

## G. Clean-Victim Prefetch

只选择 READY、clean、unpinned、未 reserved 的 victim。复用旧页保持 READY + `loading_table_[new_key]` 的 speculative read；读到 local RawPage 后验证 old mapping、replacement ticket、access generation、pin/dirty/lifecycle，再 commit READY(pin=0)。旧页被 foreground touch 时取消 commit。没有可用 clean/FREE frame 就 drop，不进入 dirty writeback。

## H. Same-Key Foreground Interaction

foreground 遇 prefetch-origin LOADING 或 clean reservation 时等待同一 completion，不发第二次 read。每个 API fetch 最多计一次 `foreground_wait_for_prefetch`。成功后验证 READY/key/publication ticket，再 pin 并返回 Guard，仍是 miss。

由于 publication pin=0，等待者醒来前可能发生合法 eviction；此时内部重新 lookup/load，而不是返回一个错误身份的 Guard。foreground 取得正在被 speculative replacement 的旧页时更新 access generation，保留取消语义。

## I. Advisory Failure Retry

prefetch 失败先清理 reservation，再完成通知。foreground 观察到 Prefetch-origin error 后进入 `load_page` 内部循环，继续 demand lookup/load；Demand-origin failure 仍向 creator/followers 返回同一 error。

`fetch_count` 只在 API 入口增加；hit/miss 分类只发生一次。测试精确验证单次 demand API：fetch delta=1、miss delta=1，真实 PageFile read attempts delta=2（一次 short read，一次成功 retry），prefetch read failure=1。FREE 与 clean-victim 两条路径均覆盖。

## J. Replacement / Pollution Semantics

Policy C：prefetch publication 调用原 `record_load`，与普通 READY 页共享 FIFO/LRU，无优先级。首次 demand 消费清除 `prefetched_ready`；若 demand 在 LOADING 期间已到达，后续不会误记 `prefetch_hit`。`useful_prefetch` 统计首次实际被 demand 消费的预取，包括 waiter；hit 只统计 demand 到达前已 READY 的预取。

没有任何 foreground 到达的预取页在正常 replacement 时被移除，计 `unused_prefetch_evicted`。close/release_table 的缓存清理不归为 replacement eviction。测试验证 FIFO/LRU 不同 victim 选择、unused eviction、clean reservation 被 touch 后取消、dirty/pinned victim 禁止。

## K. Worker Shutdown / Close Retry

`stop_prefetch_worker()` 停止接收、计数并清空 queued 请求，允许 in-flight 完成/cleanup，然后锁外 join。join 不持 metadata mutex。`close()` 在 worker 完全退出后执行原 OPEN/CLOSING/CLOSED 协议；BUSY 或 flush failure 恢复 OPEN 的返回路径会重新启动单 worker。

`release_table` 同样先停止后台任务，再执行原释放协议，最后在仍 OPEN 时恢复 worker，避免 queued/in-flight 工作在表释放过程中重新发布缓存。析构先 stop/join，再检查现有 guard lifetime 约束。

## L. Metrics

提交 counters 使用 atomic；加载/替换 counters 由 metadata mutex 保护；`stats()` 合并 snapshot。

| Counter | 定义 |
|---|---|
| prefetch_requested | API/scan 尝试提交 |
| prefetch_deduplicated | queued/active/READY/LOADING/loading_table 去重 |
| prefetch_dropped | 队列满、try_lock 失败、禁用/停止、FREE、无安全 frame、失败或取消等 advisory inability |
| prefetch_started | worker 实际建立 FREE/clean reservation 并开始 load attempt |
| prefetch_read_success/failure | requested-page read 的返回结果；allocation traversal 不计入 |
| prefetch_ready | 成功发布 READY |
| prefetch_hit | 首次 demand 到达前已经由 prefetch 发布 READY，并被消费 |
| useful_prefetch | 首次 demand 实际消费预取页，包含 waiter，不重复计 |
| foreground_wait_for_prefetch | API fetch 遇到 in-flight prefetch completion |
| unused_prefetch_evicted | 未有 foreground 到达即被 replacement 淘汰的预取页 |
| prefetch_free_page_drop | allocation-state 为 FREE 的候选 |

实时 snapshot 的多个 atomic 提交 counters 不保证跨字段同一时刻；benchmark 在 close/join 后读取，后台活动已结束。

## M. Tests Added

`tinydbms.buffer_pool_prefetch` 覆盖：

- gated worker read 时访问当前页和 metadata；same-key wait、单次 read、成功 waiter 不计 hit。
- PageFile mutex 内真实 read gate；真实 short-read 注入、FREE/clean 两种 demand retry、精确物理/API counters。
- 提前 READY hit/usefulness 一次性消费、unused eviction。
- active/queued/READY/Demand-origin LOADING/clean-reservation 去重，capacity=2、queue full drop。
- dirty-only/pinned-only pool drop；随后正常 demand dirty writeback。
- FIFO/LRU clean replacement、原 victim 被 touch 后取消、replacement order。
- queued + in-flight close、destructor join、BUSY close 后再次成功预取及 retry close。
- FREE candidate drop、HeapTable ON/OFF 相同 RID/records/order/EOF、free-page gap、scan 固化边界后的 append。

gate/condition/state 负责 ordering；超时仅用于失败 watchdog，不用 wall-clock 证明 overlap。

## N. Repeat Results

| CTest | 执行方式 | 结果 |
|---|---|---|
| tinydbms.buffer_pool_prefetch | --repeat until-fail:50 | 50/50 PASS |
| tinydbms.buffer_pool_loading | --repeat until-fail:20 | 20/20 PASS |
| tinydbms.buffer_pool_clean_victim | --repeat until-fail:20 | 20/20 PASS |
| tinydbms.buffer_pool_dirty_victim | --repeat until-fail:20 | 20/20 PASS |

日志：`build-real/pref1-repeat50.log`、`build-real/pref1-conc-repeat20.log`。以上为最终源码重新构建后结果。

## O. Build & CTest

g++ 15.2.0 / MinGW x86_64-posix-seh，Ninja，`TINYDBMS_ENABLE_REAL_MODULES=ON`。

```text
cmake --build build-real -j 4                 PASS
ctest --test-dir build-real --output-on-failure
                                            61/61 PASS
git diff --check                            PASS
```

full build 与 full CTest 日志：`build-real/pref1-build.log`、`build-real/pref1-ctest-full.log`。原测试中的既有 warning 保留；不将 warning 描述为新增 PREF-1 错误。

TSAN = NOT RUN。使用同一 g++ 对最小 `int main()` 执行 `-fsanitize=thread` 链接探针，报 `cannot find -ltsan: No such file or directory`；日志 `build-real/pref1-tsan-probe.log`。因此没有 TSAN race-free 验证结论。

## P. CPU-Overlap Benchmark

Release `-O3 -DNDEBUG`，完整 Storage library 与 benchmark 使用相同优化配置。独立构建目录 `build-pref1-release`，g++ 15.2.0，真实模块 ON。

```text
cmake -S . -B build-pref1-release -G Ninja
  -DCMAKE_CXX_COMPILER=C:/ProgramData/mingw64/mingw64/bin/g++.exe
  -DCMAKE_BUILD_TYPE=Release -DTINYDBMS_ENABLE_REAL_MODULES=ON
  -DTINYDBMS_BUILD_TESTS=OFF
cmake --build build-pref1-release --target tinydbms_prefetch_benchmark -j 4
build-pref1-release/src/storage/tinydbms_prefetch_benchmark.exe
  docs/storage/pref1-benchmark-raw.csv
```

pages=4096，capacity=64，FIFO，worker=1，distance=1，queue=2。每页 8 条记录，每条 384-byte varchar。真实 HeapTable scan/decode，加 deterministic predicate/projection/checksum：light 固定 1 轮，medium 固定 8 轮；没有 sleep/yield CPU 模拟。每个 profile 每个 mode warmup=2、measurements=20，OFF/ON 成对交替顺序。每次使用新 BufferPool，计时包含 worker drain/close；fixture 创建、线程 construction 不在计时内。最终测量期间没有并行构建或 CTest。

每次校验 32768 条记录及 checksum；每个 profile 的 40 个 measured checksum 全部一致。OS cache 未清空，属于本机 warm OS cache 的同步 stream 路径，不推断冷盘延迟或设备并行读取。

| CPU | Prefetch | min ms | p25 ms | median ms | p75 ms | max ms | pages/sec |
|---|---|---:|---:|---:|---:|---:|---:|
| light | OFF | 43.883 | 44.779 | 45.517 | 46.840 | 48.772 | 89987.466 |
| light | ON | 73.485 | 79.471 | 80.804 | 81.995 | 88.684 | 50690.340 |
| medium | OFF | 143.639 | 146.018 | 147.806 | 149.180 | 152.913 | 27711.991 |
| medium | ON | 174.426 | 175.069 | 175.971 | 178.324 | 180.870 | 23276.530 |

以下 counters 是每次测量均值，物理 reads 包含 PageFile 层全部 stream read attempts：

| CPU/mode | reads | requested | started / ready | hit | useful | wait | dropped | read failure | unused eviction |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| light OFF | 4096 | 4095 | 0 / 0 | 0 | 0 | 0 | 4095 | 0 | 0 |
| light ON | 4096 | 4095 | 3776.55 / 3776.55 | 3774.45 | 3776.55 | 2.10 | 186.90 | 0 | 0 |
| medium OFF | 4096 | 4095 | 0 / 0 | 0 | 0 | 0 | 4095 | 0 | 0 |
| medium ON | 4096 | 4095 | 3936.80 / 3936.80 | 3935.25 | 3936.80 | 1.55 | 45.55 | 0 | 0 |

原始 80 行 measured samples 见 [pref1-benchmark-raw.csv](pref1-benchmark-raw.csv)，完整聚合输出见 [pref1-benchmark-summary.txt](pref1-benchmark-summary.txt)。分位数采用有序样本线性插值。

## Q. Prefetch Effectiveness

各比例使用同组 measured counters 总和，避免对单次比例再次平均。

| CPU | hit / ready | useful / ready | unused / ready | dropped / requested | ON/OFF physical reads | ON median 变化 |
|---|---:|---:|---:|---:|---:|---:|
| light | 99.9444% | 100% | 0% | 4.5641% | 1.000000× | +77.52% |
| medium | 99.9606% | 100% | 0% | 1.1123% | 1.000000× | +19.06% |

两组均 0/20 对 ON 更快。高 hit/useful ratio 没有转化为 elapsed 改善，性能判断为 **NO MATERIAL BENEFIT**，且本机观察到显著退化。

## R. Known Limitations

只保证单 worker、bounded distance-1 advisory sequential prefetch；不提供 dirty-victim prefetch、后台 dirty flush、adaptive distance、priority/worker pool/IOScheduler、跨 query 策略、same-page writer synchronization、transaction/SQL concurrency。

同一 PageFile 的同步 physical reads 仍由 per-instance mutex 串行；目标仅为当前页 CPU 与下一页 read 的重叠机会。当前 HeapTable 每次 `next_record` 仍查询 file/page allocation 并取得 Guard，worker 增加线程唤醒、lease、allocation lookup、metadata/File mutex 竞争。这些是从代码推断的退化可能来源，本轮没有用 profiling 逐项归因。

结果适用于本机、此 fixture 与 warm OS cache；不外推其他设备和冷盘。没有运行 SQL E2E 性能扩展，因为 Storage microbenchmark 未达到稳定 elapsed 收益条件。

## S. Recommendation

保留默认 disabled 与全部验证证据，停止本轮扩展。若继续，应先单独立项定位固定成本和锁竞争，不增加 adaptive prefetch、worker pool 或 SQL concurrency。

**PREFETCH REGRESSION — INVESTIGATE**
