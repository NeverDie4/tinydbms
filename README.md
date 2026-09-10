# tinydbms

`tinydbms` 是一个单进程、单线程的教学数据库。当前已经具备从 SQL 文本到页式持久化存储的完整模块边界和最小执行路径。

## 当前状态

- `app` 提供 `--help`、`--version`、`--data-dir`、REPL 和 stdin 批处理入口。
- `compiler` 提供分句、词法、语法、语义、优化和 Plan 生成。
- `core` 提供 `Database` 生命周期、Catalog 恢复、TableId 分配和脚本顺序执行，源码按 lifecycle、script、executor、expression 拆分。
- `storage` 提供 typed heap storage、buffer pool、record/page 编解码、cursor 和持久化 metadata。
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

- `CREATE TABLE`
- `INSERT`，包括完整的显式列重排
- `SELECT`、列投影与简单 WHERE，包括比较、`AND`、`OR`、`NOT`
- `DELETE`，采用扫描、收集 RID、关闭 cursor、批量删除流程
- 正常 close/reopen 后的 schema 和记录持久化

初版 public type 只有 `INT` 和 `VARCHAR`。`INT` 是有符号 `int32_t`，物理编码为 4-byte little-endian；`VARCHAR` 是 UTF-8，物理编码为 `uint32_t` little-endian 字节长度加内容。

当前未实现 System Catalog 特殊表、FSM、Index、WAL、Transaction、MVCC、复杂 SQL，以及多个数据库并发打开。一个进程中可以创建多个 `Database` 对象，但 Storage 是 singleton，同一时刻最多一个对象处于 open 或 cleanup-pending 状态。

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

- [core 与 CLI 实现设计](docs/core-cli/实现设计.md)
- [第二阶段执行器设计](docs/core-cli/第二阶段执行器设计.md)
- [第三阶段 CLI 与入口设计](docs/core-cli/第三阶段CLI与入口设计.md)
- [联调准备与验收清单](docs/联调准备与验收清单.md)

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

开发时参考 [docs/开发守则.md](docs/开发守则.md) 和 [docs/miniob-study/](docs/miniob-study/) 的分层与调用链，不复制其事务、日志或多引擎范围。
