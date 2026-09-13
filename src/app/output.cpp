#include "output.hpp"

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
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

std::string value_text(const tinydbms::Value& value) {
    return std::visit(
        [](const auto& item) -> std::string {
            using Item = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Item, std::int32_t>) {
                return std::to_string(item);
            } else {
                return item;
            }
        },
        value.data);
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
            const tinydbms::core::Error malformed{
                tinydbms::core::ErrorKind::kInternal,
                std::nullopt,
                std::nullopt,
                "query result row width does not match column count",
                std::nullopt,
                std::nullopt};
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

std::string format_source_range(const tinydbms::SourceRange& range) {
    return std::to_string(range.begin.line) + ':' + std::to_string(range.begin.column) +
        '-' + std::to_string(range.end.line) + ':' + std::to_string(range.end.column);
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

RenderResult render_statement_result(
    const tinydbms::core::StatementResult& result,
    bool aborted,
    std::ostream& output,
    std::ostream& error_output) {
    using tinydbms::core::StatementStatus;

    switch (result.status()) {
    case StatementStatus::kExecuted: {
        const tinydbms::core::ExecuteResult& outcome = *result.outcome();
        if (const auto* query = std::get_if<tinydbms::core::QueryResult>(&outcome.outcome);
            query != nullptr) {
            return write_query_result(*query, output, error_output);
        }
        const auto* command = std::get_if<tinydbms::core::CommandResult>(&outcome.outcome);
        if (command == nullptr) {
            return RenderResult{false, false};
        }
        return RenderResult{write_command_result(*command, output), false};
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
            return RenderResult{false, true};
        }
        if (!write_command_result(*command, output)) {
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
    case StatementStatus::kExecutionIndeterminate:
        error_output << "INDETERMINATE " << format_source_range(result.source()) << '\n';
        return RenderResult{static_cast<bool>(error_output), false};
    }
    return RenderResult{false, true};
}

}  // namespace tinydbms::app
