# tinydbms

一个用于学习与课程项目的轻量级 DBMS。参考 MiniOB 的分层思路实现，不复用其代码。

## 当前状态

当前仓库只是可编译的开发骨架：

- 可执行入口支持 `--help` 和 `--version`
- 尚未实现 SQL、执行器、Catalog 或页式存储
- 编译器层、存储层、core 层均为占位静态库
- 跨模块消息契约的主体方向与边界细节已记录在 docs；尚未编写公共接口头文件

## 已确认的技术基线

1. C++20（保守子集，不碰 ranges / coroutines / modules）
2. Linux 使用 g++，Windows 使用 MSVC
3. CMake + Ninja
4. 单进程，三层静态库：`tinydbms_compiler`、`tinydbms_storage`、`tinydbms_core`
5. 测试先使用 CTest + 自写断言；GoogleTest 以后需要时再引入
6. REPL 以 EOF 退出（Unix 通常 Ctrl-D；Windows 通常 Ctrl-Z 后回车），初版不提供额外元命令；stdin 非交互时按整段批处理执行
7. 数据目录缺省为当前目录下的 `tinydbms-data/`，可用 `--data-dir` 覆盖

技术决策见 [docs/技术决策.md](docs/技术决策.md)，模块交互契约见 [docs/模块交互契约.md](docs/模块交互契约.md)，字段级消息契约见 [docs/消息契约详细设计.md](docs/消息契约详细设计.md)。

## 构建与测试

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
./build/debug/src/app/tinydbms --help
```

## 目录结构

```text
src/app/        可执行入口（当前仅 --help / --version）
src/compiler/   SQL 编译器层占位
src/core/       Database Core 占位
src/storage/    页式存储层占位
tests/          CTest 测试
docs/           设计文档（技术决策、模块契约、字段级契约）
```
