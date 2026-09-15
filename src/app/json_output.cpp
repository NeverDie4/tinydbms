#include "json_output.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace tinydbms::app {
namespace {

using tinydbms::core::ColumnHeader;
using tinydbms::core::CommandResult;
using tinydbms::core::Error;
using tinydbms::core::ErrorKind;
using tinydbms::core::ExecuteResult;
using tinydbms::core::QueryResult;
using tinydbms::core::StatementResult;
using tinydbms::core::StatementStatus;

constexpr std::string_view kReplacementCharacter{"\xEF\xBF\xBD"};  // U+FFFD

[[nodiscard]] std::string_view type_name(Type type) noexcept {
    switch (type) {
    case Type::kInt:
        return "INT";
    case Type::kBigInt:
        return "BIGINT";
    case Type::kVarchar:
        return "VARCHAR";
    case Type::kDouble:
        return "DOUBLE";
    case Type::kBoolean:
        return "BOOLEAN";
    }
    return "UNKNOWN";
}

[[nodiscard]] std::string_view error_kind_name(ErrorKind kind) noexcept {
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

[[nodiscard]] std::string_view compile_stage_name(CompileStage stage) noexcept {
    switch (stage) {
    case CompileStage::kLex:
        return "lex";
    case CompileStage::kSyntax:
        return "syntax";
    case CompileStage::kSemantic:
        return "semantic";
    }
    return "compile";
}

[[nodiscard]] std::string number_text(std::size_t value) {
    return std::to_string(value);
}

[[nodiscard]] std::string double_text(double value) {
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc{}) {
        // 非有限值在 append_value 已替换为 null；这里只兜底，保证行仍是合法 JSON。
        return "null";
    }
    return std::string(buffer.data(), end);
}

void append_hex_escape(std::string& output, const unsigned char value) {
    constexpr std::string_view kDigits{"0123456789abcdef"};
    output += "\\u00";
    output.push_back(kDigits[(value >> 4U) & 0x0FU]);
    output.push_back(kDigits[value & 0x0FU]);
}

// 返回 text[index] 起始的合法 UTF-8 序列长度（1..4）；非法或不完整返回 0。
[[nodiscard]] std::size_t utf8_sequence_length(
    const std::string_view text,
    const std::size_t index) {
    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80U) {
        return 1U;
    }

    std::size_t length = 0U;
    if (lead >= 0xC2U && lead <= 0xDFU) {
        length = 2U;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
        length = 3U;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
        length = 4U;
    } else {
        return 0U;
    }
    if (index + length > text.size()) {
        return 0U;
    }
    for (std::size_t offset = 1U; offset < length; ++offset) {
        const auto continuation = static_cast<unsigned char>(text[index + offset]);
        if (continuation < 0x80U || continuation > 0xBFU) {
            return 0U;
        }
    }

    // 拒绝过长编码与 UTF-16 代理区。
    const auto second = static_cast<unsigned char>(text[index + 1U]);
    if (length == 3U) {
        if (lead == 0xE0U && second < 0xA0U) {
            return 0U;
        }
        if (lead == 0xEDU && second > 0x9FU) {
            return 0U;
        }
    }
    if (length == 4U) {
        if (lead == 0xF0U && second < 0x90U) {
            return 0U;
        }
        if (lead == 0xF4U && second > 0x8FU) {
            return 0U;
        }
    }
    return length;
}

void append_value(std::string& output, const Value& value) {
    std::visit(
        [&output](const auto& item) {
            using Item = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Item, std::monostate>) {
                output += "null";
            } else if constexpr (std::is_same_v<Item, std::int32_t>) {
                output += std::to_string(item);
            } else if constexpr (std::is_same_v<Item, std::int64_t>) {
                output += std::to_string(item);
            } else if constexpr (std::is_same_v<Item, double>) {
                // JSON 没有 NaN/Inf 字面量，非有限值与 NULL 一样输出 null。
                output += std::isfinite(item) ? double_text(item) : std::string{"null"};
            } else if constexpr (std::is_same_v<Item, bool>) {
                output += item ? "true" : "false";
            } else {
                output.push_back('"');
                output += json_escape(item);
                output.push_back('"');
            }
        },
        value.data);
}

// type 恒为第一个字段。query/command 是 type, statement_index, range 开头，
// error/status 的字段顺序见各自的 builder（契约要求与设计稿逐字段一致）。
[[nodiscard]] std::string statement_prefix(
    const std::string_view type,
    const std::size_t statement_index,
    const SourceRange& range) {
    std::string line{"{\"type\":\""};
    line += type;
    line += "\",\"statement_index\":";
    line += number_text(statement_index);
    line += ",\"range\":\"";
    line += format_source_range(range);
    line.push_back('"');
    return line;
}

void append_error_kind_fields(std::string& line, const Error& error) {
    line += ",\"kind\":\"";
    line += error_kind_name(error.kind);
    line.push_back('"');
    if (error.kind == ErrorKind::kCompile && error.compile_stage.has_value()) {
        line += ",\"stage\":\"";
        line += compile_stage_name(*error.compile_stage);
        line.push_back('"');
    }
}

void append_error_detail_fields(std::string& line, const Error& error) {
    line += ",\"message\":\"";
    line += json_escape(error.message);
    line.push_back('"');
    if (error.suggestion.has_value()) {
        line += ",\"suggestion\":\"";
        line += json_escape(*error.suggestion);
        line.push_back('"');
    }
    if (error.fix_it.has_value()) {
        line += ",\"fix_it\":{\"range\":\"";
        line += format_source_range(error.fix_it->range);
        line += "\",\"replacement\":\"";
        line += json_escape(error.fix_it->replacement);
        line += "\"}";
    }
}

[[nodiscard]] Error internal_error(std::string message) {
    return Error{ErrorKind::kInternal, std::nullopt, std::nullopt, std::move(message), std::nullopt, std::nullopt};
}

bool write_statement_error(
    const Error& error,
    const SourceRange& fallback,
    const std::size_t statement_index,
    std::ostream& output) {
    const SourceRange& range = error.source.has_value() ? *error.source : fallback;
    std::string line{"{\"type\":\"error\",\"scope\":\"statement\",\"statement_index\":"};
    line += number_text(statement_index);
    append_error_kind_fields(line, error);
    line += ",\"range\":\"";
    line += format_source_range(range);
    line.push_back('"');
    append_error_detail_fields(line, error);
    line += '}';
    output << line << '\n';
    return static_cast<bool>(output);
}

bool write_query_object(
    const QueryResult& query,
    const std::size_t statement_index,
    const SourceRange& range,
    std::ostream& output) {
    std::string line = statement_prefix("query", statement_index, range);
    line += ",\"columns\":[";
    for (std::size_t index = 0; index < query.columns.size(); ++index) {
        if (index != 0) {
            line.push_back(',');
        }
        const ColumnHeader& column = query.columns[index];
        line += "{\"name\":\"";
        line += json_escape(column.name);
        line += "\",\"type\":\"";
        line += type_name(column.type);
        line += "\"}";
    }
    line += "],\"row_count\":";
    line += number_text(query.rows.size());
    line += ",\"rows\":[";
    for (std::size_t row_index = 0; row_index < query.rows.size(); ++row_index) {
        if (row_index != 0) {
            line.push_back(',');
        }
        line.push_back('[');
        const std::vector<Value>& row = query.rows[row_index];
        for (std::size_t column_index = 0; column_index < row.size(); ++column_index) {
            if (column_index != 0) {
                line.push_back(',');
            }
            append_value(line, row[column_index]);
        }
        line.push_back(']');
    }
    line += "]}";
    output << line << '\n';
    return static_cast<bool>(output);
}

bool write_command_object(
    const CommandResult& command,
    const std::size_t statement_index,
    const SourceRange& range,
    std::ostream& output) {
    std::string line = statement_prefix("command", statement_index, range);
    line += ",\"affected_rows\":";
    line += std::to_string(command.affected_rows);
    line.push_back('}');
    output << line << '\n';
    return static_cast<bool>(output);
}

bool write_status_object(
    const std::string_view status,
    const std::size_t statement_index,
    const SourceRange& range,
    const std::optional<std::string_view> reason,
    std::ostream& output) {
    std::string line{"{\"type\":\"status\",\"statement_index\":"};
    line += number_text(statement_index);
    line += ",\"status\":\"";
    line += status;
    line.push_back('"');
    line += ",\"range\":\"";
    line += format_source_range(range);
    line.push_back('"');
    if (reason.has_value()) {
        line += ",\"reason\":\"";
        line += *reason;
        line.push_back('"');
    }
    line.push_back('}');
    output << line << '\n';
    return static_cast<bool>(output);
}

[[nodiscard]] bool rows_match_columns(const QueryResult& query) {
    for (const std::vector<Value>& row : query.rows) {
        if (row.size() != query.columns.size()) {
            return false;
        }
    }
    return true;
}

RenderResult malformed_result(
    const StatementResult& result,
    std::string message,
    std::ostream& error_output) {
    // 结果不变式被破坏属于内部错误：写入 stderr 诊断并计入失败，退出码为 1。
    return RenderResult{
        write_statement_error(
            internal_error(std::move(message)),
            result.source(),
            result.statement_index(),
            error_output),
        true};
}

}  // namespace

std::string json_escape(const std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        const char character = text[index];
        if (character == '"') {
            escaped += "\\\"";
            ++index;
            continue;
        }
        if (character == '\\') {
            escaped += "\\\\";
            ++index;
            continue;
        }
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20U) {
            switch (character) {
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                append_hex_escape(escaped, byte);
                break;
            }
            ++index;
            continue;
        }

        const std::size_t length = utf8_sequence_length(text, index);
        if (length == 0U) {
            escaped += kReplacementCharacter;
            ++index;
            continue;
        }
        escaped.append(text.data() + index, length);
        index += length;
    }
    return escaped;
}

bool write_json_script_error(const Error& error, std::ostream& output) {
    std::string line{"{\"type\":\"error\",\"scope\":\"script\""};
    append_error_kind_fields(line, error);
    if (error.source.has_value()) {
        line += ",\"range\":\"";
        line += format_source_range(*error.source);
        line.push_back('"');
    }
    append_error_detail_fields(line, error);
    line += '}';
    output << line << '\n';
    return static_cast<bool>(output);
}

RenderResult render_json_statement_result(
    const StatementResult& result,
    const bool aborted,
    std::ostream& output,
    std::ostream& error_output) {
    switch (result.status()) {
    case StatementStatus::kExecuted:
    case StatementStatus::kPlanOnly: {
        const ExecuteResult& outcome = *result.outcome();
        if (const auto* query = std::get_if<QueryResult>(&outcome.outcome); query != nullptr) {
            if (!rows_match_columns(*query)) {
                return malformed_result(
                    result,
                    "query result row width does not match column count",
                    error_output);
            }
            return RenderResult{
                write_query_object(
                    *query, result.statement_index(), result.source(), output),
                false};
        }
        if (result.status() == StatementStatus::kPlanOnly) {
            return malformed_result(
                result,
                "kPlanOnly statement carries a non-query outcome",
                error_output);
        }
        const auto* command = std::get_if<CommandResult>(&outcome.outcome);
        if (command == nullptr) {
            return malformed_result(
                result,
                "kExecuted statement carries an unsupported outcome",
                error_output);
        }
        return RenderResult{
            write_command_object(
                *command, result.statement_index(), result.source(), output),
            false};
    }
    case StatementStatus::kCompileError:
    case StatementStatus::kExecutionError:
    case StatementStatus::kAnalysisError: {
        const ExecuteResult& outcome = *result.outcome();
        if (const auto* error = std::get_if<Error>(&outcome.outcome); error != nullptr) {
            return RenderResult{
                write_statement_error(
                    *error, result.source(), result.statement_index(), error_output),
                true};
        }
        // 部分成功的 CommandResult：先报告已完成数量，再在 stderr 报告错误。
        const auto* command = std::get_if<CommandResult>(&outcome.outcome);
        if (command == nullptr || !command->error.has_value()) {
            return malformed_result(
                result,
                "statement error status carries neither an error nor a partial result",
                error_output);
        }
        if (!write_command_object(
                *command, result.statement_index(), result.source(), output)) {
            return RenderResult{false, true};
        }
        return RenderResult{
            write_statement_error(
                *command->error, result.source(), result.statement_index(), error_output),
            true};
    }
    case StatementStatus::kAnalysisOnly:
        return RenderResult{
            write_status_object(
                "analyzed", result.statement_index(), result.source(), std::nullopt, error_output),
            false};
    case StatementStatus::kSkippedExecution:
        return RenderResult{
            write_status_object(
                "skipped",
                result.statement_index(),
                result.source(),
                aborted ? "aborted" : "policy",
                error_output),
            false};
    case StatementStatus::kCancelled:
        // 与 SKIPPED/INDETERMINATE 同样写 stderr；取消按失败计入退出码。
        return RenderResult{
            write_status_object(
                "cancelled",
                result.statement_index(),
                result.source(),
                std::nullopt,
                error_output),
            true};
    case StatementStatus::kExecutionIndeterminate:
        return RenderResult{
            write_status_object(
                "indeterminate",
                result.statement_index(),
                result.source(),
                std::nullopt,
                error_output),
            false};
    }
    return malformed_result(
        result,
        "statement carries an unknown execution status",
        error_output);
}

}  // namespace tinydbms::app
