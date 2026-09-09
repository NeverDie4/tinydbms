#include "output.hpp"

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace tinydbms::app {
namespace {

std::string_view error_kind_name(tinydbms::core::ErrorKind kind) noexcept {
    using tinydbms::core::ErrorKind;
    switch (kind) {
    case ErrorKind::kCompile:
        return "compile";
    case ErrorKind::kExecute:
        return "execute";
    case ErrorKind::kStorage:
        return "storage";
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

bool write_error(const tinydbms::core::Error& error, std::ostream& output) {
    output << "ERROR " << error_kind_name(error.kind);
    if (error.kind == tinydbms::core::ErrorKind::kCompile && error.location.has_value() &&
        error.location->line >= 1 && error.location->column >= 1) {
        output << ' ' << error.location->line << ':' << error.location->column;
    }
    output << ' ' << escape_text(error.message) << '\n';
    return static_cast<bool>(output);
}

RenderResult render_execute_result(
    const tinydbms::core::ExecuteResult& result,
    std::ostream& output,
    std::ostream& error_output) {
    if (const auto* query = std::get_if<tinydbms::core::QueryResult>(&result.outcome);
        query != nullptr) {
        if (!write_header(query->columns, output)) {
            return RenderResult{false, false};
        }
        for (const auto& row : query->rows) {
            if (row.size() != query->columns.size()) {
                const tinydbms::core::Error malformed_result{
                    tinydbms::core::ErrorKind::kInternal,
                    std::nullopt,
                    "query result row width does not match column count"};
                return RenderResult{write_error(malformed_result, error_output), true};
            }
            if (!write_row(row, output)) {
                return RenderResult{false, false};
            }
        }
        return RenderResult{true, false};
    }

    if (const auto* command = std::get_if<tinydbms::core::CommandResult>(&result.outcome);
        command != nullptr) {
        output << "OK " << command->affected_rows << '\n';
        if (!output) {
            return RenderResult{false, false};
        }
        if (!command->error.has_value()) {
            return RenderResult{true, false};
        }
        return RenderResult{write_error(*command->error, error_output), true};
    }

    const auto* error = std::get_if<tinydbms::core::Error>(&result.outcome);
    if (error == nullptr) {
        const tinydbms::core::Error malformed_result{
            tinydbms::core::ErrorKind::kInternal,
            std::nullopt,
            "unknown execute result variant"};
        return RenderResult{write_error(malformed_result, error_output), true};
    }
    return RenderResult{write_error(*error, error_output), true};
}

}  // namespace tinydbms::app
