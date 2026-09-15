#ifndef TINYDBMS_APP_PRETTY_OUTPUT_HPP
#define TINYDBMS_APP_PRETTY_OUTPUT_HPP

#include <iosfwd>

#include "output.hpp"
#include "tinydbms/core.hpp"

namespace tinydbms::app {

// 人读格式的结果渲染：等宽边框表格 + 行数行。诊断与状态不在这里处理，
// 由 output.cpp 的既有实现统一写 stderr，保证两种文本格式的诊断字节一致。
//
// 结果不变式破坏（行宽与列数不一致）时写 stderr 诊断并把 had_error 置位，
// 与 table 模式同语义：stdout 不留下半截表格。
RenderResult write_pretty_query_result(
    const tinydbms::core::QueryResult& query,
    std::ostream& output,
    std::ostream& error_output);

// 命令结果：OK, N rows affected。
bool write_pretty_command_result(
    const tinydbms::core::CommandResult& command,
    std::ostream& output);

}  // namespace tinydbms::app

#endif  // TINYDBMS_APP_PRETTY_OUTPUT_HPP
