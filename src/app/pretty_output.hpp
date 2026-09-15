#ifndef TINYDBMS_APP_PRETTY_OUTPUT_HPP
#define TINYDBMS_APP_PRETTY_OUTPUT_HPP

#include <iosfwd>

#include "output.hpp"
#include "tinydbms/core.hpp"

namespace tinydbms::app {

// 查询结果的 pretty 渲染选项。
//
// 计划文本（kPlanOnly）是缩进文本而不是表格数据：列映射那一行经常超过 48 列，
// 截断会直接丢掉信息，行数行也只反映"计划文本有几行"而非结果集大小，
// 因此计划模式关闭这两项；普通查询结果保持截断与行数行。
struct PrettyQueryOptions {
    bool truncate_cells = true;
    bool show_row_count = true;
};

// 人读格式的结果渲染：等宽边框表格 + 行数行。诊断与状态不在这里处理，
// 由 output.cpp 的既有实现统一写 stderr，保证两种文本格式的诊断字节一致。
//
// 结果不变式破坏（行宽与列数不一致）时写 stderr 诊断并把 had_error 置位，
// 与 table 模式同语义：stdout 不留下半截表格。
RenderResult write_pretty_query_result(
    const tinydbms::core::QueryResult& query,
    std::ostream& output,
    std::ostream& error_output,
    PrettyQueryOptions options = PrettyQueryOptions{});

// 命令结果：OK, N rows affected。
bool write_pretty_command_result(
    const tinydbms::core::CommandResult& command,
    std::ostream& output);

}  // namespace tinydbms::app

#endif  // TINYDBMS_APP_PRETTY_OUTPUT_HPP
