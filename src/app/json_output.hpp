#ifndef TINYDBMS_APP_JSON_OUTPUT_HPP
#define TINYDBMS_APP_JSON_OUTPUT_HPP

#include <iosfwd>
#include <string>
#include <string_view>

#include "output.hpp"

namespace tinydbms::app {

// JSON 字符串转义：处理 "、\ 与控制字符；非法 UTF-8 字节序列替换为 U+FFFD，
// 保证输出始终是合法 JSON。返回值不含首尾引号。
std::string json_escape(std::string_view text);

// 脚本级错误的 error 对象（分句失败、语句数超限、致命中止、open/close 失败）：
// scope 为 script，没有 statement_index；只有 error.source 存在时才输出 range。
bool write_json_script_error(const tinydbms::core::Error& error, std::ostream& output);

// 单条语句的 JSON 行对象：结果对象（query/command）写 output，
// 诊断对象（error/status）写 error_output。
RenderResult render_json_statement_result(
    const tinydbms::core::StatementResult& result,
    bool aborted,
    std::ostream& output,
    std::ostream& error_output);

}  // namespace tinydbms::app

#endif  // TINYDBMS_APP_JSON_OUTPUT_HPP
