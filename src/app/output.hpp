#ifndef TINYDBMS_APP_OUTPUT_HPP
#define TINYDBMS_APP_OUTPUT_HPP

#include <iosfwd>
#include <string>
#include <string_view>

#include "tinydbms/core.hpp"

namespace tinydbms::app {

struct RenderResult {
    bool output_ok = true;
    bool had_error = false;
};

std::string escape_text(std::string_view text);

// 固定格式 begin_line:begin_column-end_line:end_column，单点范围也保留完整格式。
std::string format_source_range(const tinydbms::SourceRange& range);

// 语句级错误：编译错误使用诊断范围，其余错误由调用方给出语句范围兜底。
bool write_statement_error(
    const tinydbms::core::Error& error,
    const tinydbms::SourceRange& fallback,
    std::ostream& output);

// 非语句级错误（脚本级错误、open/close 等生命周期错误）：
// 分句失败与语句数超限使用 compile 标签，致命中止使用 internal。
bool write_error(const tinydbms::core::Error& error, std::ostream& output);

RenderResult render_statement_result(
    const tinydbms::core::StatementResult& result,
    bool aborted,
    std::ostream& output,
    std::ostream& error_output);

}  // namespace tinydbms::app

#endif  // TINYDBMS_APP_OUTPUT_HPP
