#ifndef TINYDBMS_APP_OUTPUT_HPP
#define TINYDBMS_APP_OUTPUT_HPP

#include <chrono>
#include <iosfwd>
#include <string>
#include <string_view>

#include "tinydbms/core.hpp"

namespace tinydbms::app {

// 展示格式：table 是制表符文本（脚本契约）；json 是 NDJSON，每行一个 JSON 对象；
// pretty 是终端里给人看的等宽表格（不承诺机器可解析）。
enum class OutputFormat {
    kTable,
    kPretty,
    kJson
};

struct RenderResult {
    bool output_ok = true;
    bool had_error = false;
};

std::string escape_text(std::string_view text);

// CLI 侧发现结果不变式破坏时的统一兜底错误：core 的工厂理论上不允许出现，
// 这里保证任何异常组合都会留下 kInternal 诊断而不是静默失败。
tinydbms::core::Error malformed_result_error(std::string message);

// 固定格式 begin_line:begin_column-end_line:end_column，单点范围也保留完整格式。
std::string format_source_range(const tinydbms::SourceRange& range);

// 毫秒文本，固定三位小数（--time 的取值部分）。
std::string format_milliseconds(std::chrono::nanoseconds elapsed);

// --time 的一行：TIME <scope> <毫秒> ms。scope 由调用方给出（script / line N）。
bool write_time_line(
    std::string_view scope,
    std::chrono::nanoseconds elapsed,
    std::ostream& output);

// 缺失率文本，固定两位小数；fetch 为 0 时是 0.00。
std::string format_miss_rate(std::uint64_t miss_count, std::uint64_t fetch_count);

// --stats 的一行：BUFFER fetch=… hit=… miss=… miss_rate=…% evictions=… flushes=…。
bool write_storage_stats_line(
    const tinydbms::core::StorageStats& stats,
    std::ostream& output);

// 语句级错误：编译错误使用诊断范围，其余错误由调用方给出语句范围兜底。
bool write_statement_error(
    const tinydbms::core::Error& error,
    const tinydbms::SourceRange& fallback,
    std::ostream& output);

// 非语句级错误（脚本级错误、open/close 等生命周期错误）：
// 分句失败与语句数超限使用 compile 标签，致命中止使用 internal。
bool write_error(const tinydbms::core::Error& error, std::ostream& output);

// 格式分派入口：table 与 pretty 走本文件既有实现（只有成功结果的渲染不同），
// json 交给 json_output.cpp。三者的流向约束一致：结果写 output，诊断写 error_output。
bool write_error(
    const tinydbms::core::Error& error,
    OutputFormat format,
    std::ostream& output);

RenderResult render_statement_result(
    const tinydbms::core::StatementResult& result,
    bool aborted,
    OutputFormat format,
    std::ostream& output,
    std::ostream& error_output);

}  // namespace tinydbms::app

#endif  // TINYDBMS_APP_OUTPUT_HPP
