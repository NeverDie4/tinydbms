# tinydbms

一个用于学习与课程项目的轻量级 DBMS。参考 MiniOB 的分层思路实现，不复用其代码。

## 当前状态

当前仓库已经完成 Typed Heap Storage Phase 0–4；SQL 前端与执行器仍是待完善骨架：

- 可执行入口支持 `--help`、`--version` 和 `--data-dir DIR`
- `core` 已经拥有 `open_database`、`execute_script`、`close_database` 生命周期
- `storage` 已经能够创建、恢复和保存 `storage.meta` Catalog，并通过私有 FileManager 管理独立的 4096-byte PageFile 表文件
- `compiler` 已经提供带源位置的多语句切分接口
- 完整 SQL、执行器、索引和事务仍未实现；RecordCodec、SlottedPage、RecordPage/RID codec 和公共 Storage CRUD 已完成，集成测试验证记录、删除/复用与 RID 的 close/reopen 持久化
- [BufferPool](docs/buffer-pool.md)：FIFO/LRU、failure-safe eviction、PageGuard/pin、dirty/flush、统计与可关闭日志；默认 64 Frames/FIFO
- [HeapTable INSERT](docs/heap-table.md)：allocation-state、多页 First-Fit、新页 bootstrap/失败补偿与批量前缀
- [HeapTable SCAN](docs/heap-scan.md)：live-slot 枚举、固定页终点、EOF/Failed 和进程级单调 CursorId
- [HeapTable DELETE](docs/heap-delete.md)：RID 校验、批量前缀与 generation 持久化
- [Phase 4E / Phase 4 DONE](docs/storage-crud.md) 已接通公共 INSERT、逐行 SCAN、RID DELETE，CursorRegistry 纳入 Storage 生命周期，包含同表游标写入限制与多页 CRUD close/reopen 回归；上述 4B–4D 的阶段性接线待办已完成，未开始 SQL executor

初始化开发链路见 [docs/初始化开发链路.md](docs/初始化开发链路.md)。

Storage V1 架构、格式、失败语义、压力测试与课程差距的最终审计见 [docs/storage-v1-audit.md](docs/storage-v1-audit.md)。

后续开发必须参考 MiniOB 的源码分析文档和项目开发守则，见 [docs/开发守则.md](docs/开发守则.md) 与 [docs/miniob-study/](docs/miniob-study/)。

## 已确认的技术基线

1. C++20（保守子集，不碰 ranges / coroutines / modules）
2. Linux 使用 g++，Windows 使用 MSVC
3. CMake + Ninja
4. 单进程，三层静态库：`tinydbms_compiler`、`tinydbms_storage`、`tinydbms_core`
5. 测试先使用 CTest + 自写断言；GoogleTest 以后需要时再引入
6. REPL 以 EOF 退出（Unix 通常 Ctrl-D；Windows 通常 Ctrl-Z 后回车），初版不提供额外元命令；stdin 非交互时按整段批处理执行
7. 数据目录缺省为当前目录下的 `tinydbms-data/`，可用 `--data-dir` 覆盖

技术决策见 [docs/技术决策.md](docs/技术决策.md)，模块交互契约见 [docs/模块交互契约.md](docs/模块交互契约.md)，字段级消息契约见 [docs/消息契约详细设计.md](docs/消息契约详细设计.md)，Storage 冻结契约见 [docs/storage-contract.md](docs/storage-contract.md)。物理格式与集成设计见 [docs/page-file-format.md](docs/page-file-format.md)、[docs/file-manager.md](docs/file-manager.md) 和 [docs/record-page-format.md](docs/record-page-format.md)。

## 构建与测试

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
./build/debug/src/app/tinydbms --help
```

## 目录结构

```text
src/include/    跨模块公共消息与类型契约
src/app/        可执行入口、参数解析和 REPL
src/compiler/   SQL 语句切分与后续编译入口
src/core/       Database Core、Catalog 所有权和生命周期
src/storage/    Typed Heap Storage、BufferPool、PageFile 与元数据持久化
tests/          CTest 测试
docs/           设计文档、开发守则和 MiniOB 学习资料
```
