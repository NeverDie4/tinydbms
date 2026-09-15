#include "output.hpp"

#include "json_output.hpp"
#include "pretty_output.hpp"
#include "value_text.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tinydbms::app {
namespace {

std::string_view compile_stage_name(tinydbms::CompileStage stage) noexcept {
    switch (stage) {
    case tinydbms::CompileStage::kLex:
        return "lex";
    case tinydbms::CompileStage::kSyntax:
        return "syntax";
    case tinydbms::CompileStage::kSemantic:
        return "semantic";
    }
    return "compile";
}

std::string_view statement_error_kind_name(tinydbms::core::ErrorKind kind) noexcept {
    using tinydbms::core::ErrorKind;
    switch (kind) {
    case ErrorKind::kExecute:
        return "execute";
    case ErrorKind::kStorage:
        return "storage";
    case ErrorKind::kAnalysis:
        return "analysis";
    case ErrorKind::kInternal:
        return "internal";
    case ErrorKind::kCompile:
        break;
    }
    return "internal";
}

std::string_view script_error_kind_name(tinydbms::core::ErrorKind kind) noexcept {
    using tinydbms::core::ErrorKind;
    switch (kind) {
    case ErrorKind::kCompile:
        return "compile";
    case ErrorKind::kExecute:
        return "execute";
    case ErrorKind::kStorage:
        return "storage";
    case ErrorKind::kAnalysis:
        return "analysis";
    case ErrorKind::kInternal:
        return "internal";
    }
    return "internal";
}

bool write_row(
    const std::vector<tinydbms::Value>& row,
    std::ostream& output) {
    for (std::size_t index = 0; index < row.size(); ++index) {
        if (index != 0) {
            output.put('\t');
        }
        output << escape_text(value_text(row[index]));
    }
    output.put('\n');
    return static_cast<bool>(output);
}

bool write_header(
    const std::vector<tinydbms::core::ColumnHeader>& columns,
    std::ostream& output) {
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (index != 0) {
            output.put('\t');
        }
        output << escape_text(columns[index].name);
    }
    output.put('\n');
    return static_cast<bool>(output);
}

bool write_error_line(
    const tinydbms::core::Error& error,
    std::string_view label,
    const std::optional<tinydbms::SourceRange>& source,
    std::ostream& output) {
    output << "ERROR " << label;
    if (source.has_value()) {
        output << ' ' << format_source_range(*source);
    }
    output << ' ' << escape_text(error.message) << '\n';
    if (error.suggestion.has_value()) {
        output << "SUGGESTION " << escape_text(*error.suggestion) << '\n';
    }
    if (error.fix_it.has_value()) {
        output << "FIX " << format_source_range(error.fix_it->range) << ' '
               << escape_text(error.fix_it->replacement) << '\n';
    }
    return static_cast<bool>(output);
}

RenderResult write_query_result(
    const tinydbms::core::QueryResult& query,
    std::ostream& output,
    std::ostream& error_output) {
    if (!write_header(query.columns, output)) {
        return RenderResult{false, false};
    }
    for (const auto& row : query.rows) {
        if (row.size() != query.columns.size()) {
            const tinydbms::core::Error malformed =
                malformed_result_error("query result row width does not match column count");
            // 结果不变式被破坏属于内部错误：写入 stderr 并计入失败，退出码为 1。
            return RenderResult{
                write_error_line(malformed, "internal", std::nullopt, error_output),
                true};
        }
        if (!write_row(row, output)) {
            return RenderResult{false, false};
        }
    }
    return RenderResult{true, false};
}

bool write_command_result(
    const tinydbms::core::CommandResult& command,
    std::ostream& output) {
    output << "OK " << command.affected_rows << '\n';
    return static_cast<bool>(output);
}

}  // namespace

std::string escape_text(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '\\':
            escaped += "\\\\";
            break;
        case '\t':
            escaped += "\\t";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        default:
            escaped.push_back(character);
            break;
        }
    }
    return escaped;
}

tinydbms::core::Error malformed_result_error(std::string message) {
    return tinydbms::core::Error{
        tinydbms::core::ErrorKind::kInternal,
        std::nullopt,
        std::nullopt,
        std::move(message),
        std::nullopt,
        std::nullopt};
}

std::string format_source_range(const tinydbms::SourceRange& range) {
    return std::to_string(range.begin.line) + ':' + std::to_string(range.begin.column) +
        '-' + std::to_string(range.end.line) + ':' + std::to_string(range.end.column);
}

std::string format_milliseconds(std::chrono::nanoseconds elapsed) {
    if (elapsed.count() < 0) {
        elapsed = std::chrono::nanoseconds{0};
    }
    // steady_clock 的时长换算成毫秒后固定三位小数；四舍五入交给 iostream 的 fixed/setprecision。
    const std::chrono::duration<double, std::milli> milliseconds{elapsed};
    std::ostringstream text;
    text << std::fixed << std::setprecision(3) << milliseconds.count();
    return text.str();
}

bool write_time_line(
    std::string_view scope,
    std::chrono::nanoseconds elapsed,
    std::ostream& output) {
    output << "TIME " << scope << ' ' << format_milliseconds(elapsed) << " ms\n";
    return static_cast<bool>(output);
}

bool write_statement_error(
    const tinydbms::core::Error& error,
    const tinydbms::SourceRange& fallback,
    std::ostream& output) {
    std::string_view label;
    if (error.kind == tinydbms::core::ErrorKind::kCompile) {
        label = error.compile_stage.has_value()
            ? compile_stage_name(*error.compile_stage)
            : std::string_view{"compile"};
    } else {
        label = statement_error_kind_name(error.kind);
    }

    const std::optional<tinydbms::SourceRange> source =
        error.source.has_value()
        ? error.source
        : std::optional<tinydbms::SourceRange>{fallback};
    return write_error_line(error, label, source, output);
}

bool write_error(const tinydbms::core::Error& error, std::ostream& output) {
    return write_error_line(
        error,
        script_error_kind_name(error.kind),
        error.source,
        output);
}

namespace {

RenderResult render_text_statement_result(
    const tinydbms::core::StatementResult& result,
    bool aborted,
    bool pretty,
    std::ostream& output,
    std::ostream& error_output) {
    using tinydbms::core::StatementStatus;

    switch (result.status()) {
    case StatementStatus::kExecuted: {
        const tinydbms::core::ExecuteResult& outcome = *result.outcome();
        if (const auto* query = std::get_if<tinydbms::core::QueryResult>(&outcome.outcome);
            query != nullptr) {
            return pretty
                ? write_pretty_query_result(*query, output, error_output)
                : write_query_result(*query, output, error_output);
        }
        const auto* command = std::get_if<tinydbms::core::CommandResult>(&outcome.outcome);
        if (command == nullptr) {
            // 工厂保证 kExecuted 只携带无错误结果；走到这里说明结果不变式被破坏，
            // 必须留下诊断文本，不能只把退出码置为失败。
            return RenderResult{
                write_statement_error(
                    malformed_result_error("kExecuted statement carries an unsupported outcome"),
                    result.source(),
                    error_output),
                true};
        }
        return RenderResult{
            pretty
                ? write_pretty_command_result(*command, output)
                : write_command_result(*command, output),
            false};
    }
    case StatementStatus::kPlanOnly: {
        const tinydbms::core::ExecuteResult& outcome = *result.outcome();
        const auto* query = std::get_if<tinydbms::core::QueryResult>(&outcome.outcome);
        if (query == nullptr) {
            // 工厂保证 kPlanOnly 只携带 QueryResult；走到这里说明结果不变式被破坏。
            return RenderResult{
                write_statement_error(
                    malformed_result_error(
                        "kPlanOnly statement carries a non-query outcome"),
                    result.source(),
                    error_output),
                true};
        }
        return pretty
            // 计划文本是缩进文本：不截断列映射，也不打印"计划文本有几行"。
            ? write_pretty_query_result(
                  *query,
                  output,
                  error_output,
                  PrettyQueryOptions{false, false})
            : write_query_result(*query, output, error_output);
    }
    case StatementStatus::kCompileError:
    case StatementStatus::kExecutionError:
    case StatementStatus::kAnalysisError: {
        const tinydbms::core::ExecuteResult& outcome = *result.outcome();
        if (const auto* error = std::get_if<tinydbms::core::Error>(&outcome.outcome);
            error != nullptr) {
            return RenderResult{
                write_statement_error(*error, result.source(), error_output),
                true};
        }
        // 部分成功的 CommandResult：先报告已完成数量，再在 stderr 报告错误。
        const auto* command = std::get_if<tinydbms::core::CommandResult>(&outcome.outcome);
        if (command == nullptr || !command->error.has_value()) {
            return RenderResult{
                write_statement_error(
                    malformed_result_error(
                        "statement error status carries neither an error nor a partial result"),
                    result.source(),
                    error_output),
                true};
        }
        const bool command_written = pretty
            ? write_pretty_command_result(*command, output)
            : write_command_result(*command, output);
        if (!command_written) {
            return RenderResult{false, true};
        }
        return RenderResult{
            write_statement_error(*command->error, result.source(), error_output),
            true};
    }
    case StatementStatus::kAnalysisOnly:
        error_output << "ANALYZED " << format_source_range(result.source()) << '\n';
        return RenderResult{static_cast<bool>(error_output), false};
    case StatementStatus::kSkippedExecution:
        error_output << "SKIPPED " << format_source_range(result.source())
                     << (aborted ? " aborted" : " policy") << '\n';
        return RenderResult{static_cast<bool>(error_output), false};
    case StatementStatus::kCancelled:
        // 取消是调用方的主动行为：写出状态即可，但本次调用按失败处理（退出码 1）。
        error_output << "CANCELLED " << format_source_range(result.source()) << '\n';
        return RenderResult{static_cast<bool>(error_output), true};
    case StatementStatus::kExecutionIndeterminate:
        error_output << "INDETERMINATE " << format_source_range(result.source()) << '\n';
        return RenderResult{static_cast<bool>(error_output), false};
    }
    return RenderResult{
        write_statement_error(
            malformed_result_error("statement carries an unknown execution status"),
            result.source(),
            error_output),
        true};
}

}  // namespace

bool write_error(
    const tinydbms::core::Error& error,
    OutputFormat format,
    std::ostream& output) {
    if (format == OutputFormat::kJson) {
        return write_json_script_error(error, output);
    }
    return write_error(error, output);
}

RenderResult render_statement_result(
    const tinydbms::core::StatementResult& result,
    bool aborted,
    OutputFormat format,
    std::ostream& output,
    std::ostream& error_output) {
    if (format == OutputFormat::kJson) {
        return render_json_statement_result(result, aborted, output, error_output);
    }
    // pretty 只替换成功结果的呈现；诊断、状态与错误路径与 table 完全共用一套实现。
    return render_text_statement_result(
        result,
        aborted,
        format == OutputFormat::kPretty,
        output,
        error_output);
}

}  // namespace tinydbms::app
