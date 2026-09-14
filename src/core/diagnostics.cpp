#include "diagnostics.hpp"

#include <limits>

namespace tinydbms::core::internal {
namespace {

bool is_valid_location(SourceLocation location) noexcept {
    return location.line >= 1 && location.column >= 1;
}

// 端点不能把 \r\n 拆开：offset 指向 '\n' 而前一字节是 '\r' 属于非法端点。
bool is_valid_endpoint(std::size_t offset, std::string_view text) noexcept {
    if (offset > text.size()) {
        return false;
    }
    return !(offset > 0 && offset < text.size() &&
             text[offset - 1] == '\r' && text[offset] == '\n');
}

// 行按 \n 与 \r\n 分割；单独的 \r 和 Tab 都按一个字节占一列。
std::optional<SourceLocation> location_at_offset(
    std::string_view text,
    std::size_t offset) noexcept {
    if (!is_valid_endpoint(offset, text)) {
        return std::nullopt;
    }

    std::int64_t line = 1;
    std::int64_t column = 1;
    std::size_t index = 0;
    while (index < offset) {
        if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n') {
            ++line;
            column = 1;
            index += 2;
            continue;
        }
        if (text[index] == '\n') {
            ++line;
            column = 1;
            ++index;
            continue;
        }
        ++column;
        ++index;
    }

    if (line > std::numeric_limits<int>::max() ||
        column > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return SourceLocation{static_cast<int>(line), static_cast<int>(column), offset};
}

}  // namespace

SourceRange empty_source_range() noexcept {
    return SourceRange{SourceLocation{1, 1, 0}, SourceLocation{1, 1, 0}};
}

bool is_valid_source_range(const SourceRange& range, std::string_view text) noexcept {
    if (!is_valid_location(range.begin) || !is_valid_location(range.end)) {
        return false;
    }
    if (range.begin.byte_offset > range.end.byte_offset ||
        range.end.byte_offset > text.size()) {
        return false;
    }
    if (!is_valid_endpoint(range.begin.byte_offset, text) ||
        !is_valid_endpoint(range.end.byte_offset, text)) {
        return false;
    }

    const std::optional<SourceLocation> begin =
        location_at_offset(text, range.begin.byte_offset);
    const std::optional<SourceLocation> end = location_at_offset(text, range.end.byte_offset);
    if (!begin.has_value() || !end.has_value()) {
        return false;
    }
    return begin->line == range.begin.line && begin->column == range.begin.column &&
        end->line == range.end.line && end->column == range.end.column;
}

std::optional<SourceRange> absolutize_range(
    const compiler::SplitStatement& statement,
    const SourceRange& relative,
    std::string_view script_text) noexcept {
    if (!is_valid_source_range(statement.source, script_text)) {
        return std::nullopt;
    }
    const std::size_t statement_begin = statement.source.begin.byte_offset;
    const std::size_t statement_end = statement.source.end.byte_offset;
    if (statement_end < statement_begin ||
        statement_end - statement_begin != statement.sql.size()) {
        return std::nullopt;
    }
    if (!is_valid_source_range(relative, statement.sql)) {
        return std::nullopt;
    }

    const std::size_t begin_offset = statement_begin + relative.begin.byte_offset;
    const std::size_t end_offset = statement_begin + relative.end.byte_offset;
    if (begin_offset > script_text.size() || end_offset > script_text.size()) {
        return std::nullopt;
    }

    const std::optional<SourceLocation> begin = location_at_offset(script_text, begin_offset);
    const std::optional<SourceLocation> end = location_at_offset(script_text, end_offset);
    if (!begin.has_value() || !end.has_value()) {
        return std::nullopt;
    }
    return SourceRange{*begin, *end};
}

}  // namespace tinydbms::core::internal
