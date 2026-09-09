# tinydbms

一个用于学习与课程项目的轻量级 DBMS。参考 MiniOB 的分层思路实现，不复用其代码。

## 当前状态

当前仓库处于分层实现阶段：

- 可执行入口已实现 `--help`、`--version`、`--data-dir`、REPL/批处理、结果展示和退出码策略
- core 已实现 `Database` 生命周期、Catalog 恢复、TableId 分配、CREATE TABLE、INSERT、DELETE、SELECT
  执行和脚本主循环
- compiler、storage 仍为占位静态库；默认构建的 CLI 使用不可用模块适配器，完整 SQL 链路尚未接通
- 跨模块契约头文件已落地在 `include/tinydbms/`，真实模块 API 仍待实现

## 已确认的技术基线

1. C++20（保守子集，不碰 ranges / coroutines / modules）
2. Linux 使用 g++，Windows 使用 MSVC
3. CMake + Ninja
4. 单进程，三层静态库：`tinydbms_compiler`、`tinydbms_storage`、`tinydbms_core`
5. 测试先使用 CTest + 自写断言；GoogleTest 以后需要时再引入
6. REPL 以 EOF 退出（Unix 通常 Ctrl-D；Windows 通常 Ctrl-Z 后回车），初版不提供额外元命令；stdin 非交互时按整段批处理执行
7. 数据目录缺省为当前目录下的 `tinydbms-data/`，可用 `--data-dir` 覆盖

技术决策见 [docs/技术决策.md](docs/技术决策.md)，模块交互契约见 [docs/模块交互契约.md](docs/模块交互契约.md)，字段级消息契约见 [docs/消息契约详细设计.md](docs/消息契约详细设计.md)。

当前实现阶段的设计入口：

- [core 与 CLI 实现设计](docs/core-cli/实现设计.md)
- [第二阶段执行器设计](docs/core-cli/第二阶段执行器设计.md)
- [第三阶段 CLI 与入口设计](docs/core-cli/第三阶段CLI与入口设计.md)

## 构建与测试

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
./build/debug/src/app/tinydbms --help
```

compiler/storage 交付后，使用 `-DTINYDBMS_ENABLE_REAL_MODULES=ON` 重新配置构建，才会启用真实
`CoreSession` 和完整 SQL 链路；占位构建仍可独立验证 CLI 参数、输入输出和生命周期测试。

## 目录结构

```text
src/app/        可执行入口与 CLI 逻辑（真实模块交付后接通 SQL）
src/compiler/   SQL 编译器层占位
src/core/       Database Core（按 database / script / executor / expression 拆分）
src/storage/    物理存储层占位
include/        公共契约头文件（common / compiler / storage / core）
tests/          CTest 测试
docs/           设计文档（技术决策、模块契约、字段级契约和阶段实现规格）
```
