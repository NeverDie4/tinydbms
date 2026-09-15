#include "pretty_output.hpp"

#include "value_text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tinydbms::app {
namespace {

using tinydbms::core::CommandResult;
using tinydbms::core::QueryResult;

// 单元格显示宽度上限：超长 VARCHAR 在这里截断并追加省略号，避免撑爆终端。
// 只有 pretty 这么做：table 与 json 永远输出完整值。
constexpr std::size_t kMaxCellWidth = 48;

// U+2500 区块的制表符，按 UTF-8 字节写出，避免依赖源文件编码。
constexpr std::string_view kHorizontal{"\xE2\x94\x80"};  // ─
constexpr std::string_view kVertical{"\xE2\x94\x82"};    // │
constexpr std::string_view kTopLeft{"\xE2\x94\x8C"};     // ┌
constexpr std::string_view kTopMiddle{"\xE2\x94\xAC"};   // ┬
constexpr std::string_view kTopRight{"\xE2\x94\x90"};    // ┐
constexpr std::string_view kMidLeft{"\xE2\x94\x9C"};     // ├
constexpr std::string_view kMidMiddle{"\xE2\x94\xBC"};   // ┼
constexpr std::string_view kMidRight{"\xE2\x94\xA4"};    // ┤
constexpr std::string_view kBottomLeft{"\xE2\x94\x94"};  // └
constexpr std::string_view kBottomMiddle{"\xE2\x94\xB4"};// ┴
constexpr std::string_view kBottomRight{"\xE2\x94\x98"}; // ┘
constexpr std::string_view kEllipsis{"\xE2\x80\xA6"};    // …

struct DecodedCodePoint {
    std::uint32_t code = 0;
    std::size_t length = 1;
};

// 单字节越界与非法序列都返回 {首字节, 1}：宽度按 1 列算，绝不越界、绝不抛异常。
DecodedCodePoint decode_code_point(std::string_view text, std::size_t index) {
    const auto byte = [&text](std::size_t position) {
        return static_cast<std::uint8_t>(text[position]);
    };
    const std::uint8_t first = byte(index);
    if (first < 0x80U) {
        return DecodedCodePoint{first, 1};
    }
    if ((first & 0xE0U) == 0xC0U && index + 1U < text.size() &&
        (byte(index + 1U) & 0xC0U) == 0x80U) {
        const std::uint32_t code =
            (static_cast<std::uint32_t>(first & 0x1FU) << 6U) |
            static_cast<std::uint32_t>(byte(index + 1U) & 0x3FU);
        if (code >= 0x80U) {
            return DecodedCodePoint{code, 2};
        }
    } else if ((first & 0xF0U) == 0xE0U && index + 2U < text.size() &&
               (byte(index + 1U) & 0xC0U) == 0x80U &&
               (byte(index + 2U) & 0xC0U) == 0x80U) {
        const std::uint32_t code =
            (static_cast<std::uint32_t>(first & 0x0FU) << 12U) |
            (static_cast<std::uint32_t>(byte(index + 1U) & 0x3FU) << 6U) |
            static_cast<std::uint32_t>(byte(index + 2U) & 0x3FU);
        if (code >= 0x800U && !(code >= 0xD800U && code <= 0xDFFFU)) {
            return DecodedCodePoint{code, 3};
        }
    } else if ((first & 0xF8U) == 0xF0U && index + 3U < text.size() &&
               (byte(index + 1U) & 0xC0U) == 0x80U &&
               (byte(index + 2U) & 0xC0U) == 0x80U &&
               (byte(index + 3U) & 0xC0U) == 0x80U) {
        const std::uint32_t code =
            (static_cast<std::uint32_t>(first & 0x07U) << 18U) |
            (static_cast<std::uint32_t>(byte(index + 1U) & 0x3FU) << 12U) |
            (static_cast<std::uint32_t>(byte(index + 2U) & 0x3FU) << 6U) |
            static_cast<std::uint32_t>(byte(index + 3U) & 0x3FU);
        if (code >= 0x10000U && code <= 0x10FFFFU) {
            return DecodedCodePoint{code, 4};
        }
    }
    return DecodedCodePoint{first, 1};
}

// 终端显示宽度：东亚宽字符 2 列，组合附标/零宽字符 0 列，控制字符 0 列，其余 1 列。
std::size_t code_point_width(std::uint32_t code) {
    if (code < 0x20U || (code >= 0x7FU && code < 0xA0U)) {
        return 0;
    }
    if ((code >= 0x0300U && code <= 0x036FU) ||
        (code >= 0x1AB0U && code <= 0x1AFFU) ||
        (code >= 0x1DC0U && code <= 0x1DFFU) ||
        (code >= 0x200BU && code <= 0x200FU) ||
        (code >= 0x20D0U && code <= 0x20FFU) ||
        (code >= 0xFE00U && code <= 0xFE0FU) ||
        (code >= 0xFE20U && code <= 0xFE2FU) ||
        code == 0x2060U || code == 0xFEFFU) {
        return 0;
    }
    if ((code >= 0x1100U && code <= 0x115FU) ||
        (code >= 0x2E80U && code <= 0xA4CFU) ||
        (code >= 0xA960U && code <= 0xA97FU) ||
        (code >= 0xAC00U && code <= 0xD7A3U) ||
        (code >= 0xF900U && code <= 0xFAFFU) ||
        (code >= 0xFE10U && code <= 0xFE19U) ||
        (code >= 0xFE30U && code <= 0xFE6FU) ||
        (code >= 0xFF00U && code <= 0xFF60U) ||
        (code >= 0xFFE0U && code <= 0xFFE6U) ||
        (code >= 0x1F300U && code <= 0x1F64FU) ||
        (code >= 0x1F900U && code <= 0x1F9FFU) ||
        (code >= 0x20000U && code <= 0x3FFFDU)) {
        return 2;
    }
    return 1;
}

std::size_t display_width(std::string_view text) {
    std::size_t width = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        const DecodedCodePoint decoded = decode_code_point(text, index);
        width += code_point_width(decoded.code);
        index += decoded.length;
    }
    return width;
}

// 超过 limit 列时按显示宽度截断并补一个省略号；不超过时原样返回。
std::string truncate_to_width(std::string_view text, std::size_t limit) {
    if (display_width(text) <= limit) {
        return std::string{text};
    }
    const std::size_t ellipsis_width = display_width(kEllipsis);
    if (limit <= ellipsis_width) {
        return std::string{kEllipsis};
    }

    const std::size_t budget = limit - ellipsis_width;
    std::string result;
    std::size_t width = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        const DecodedCodePoint decoded = decode_code_point(text, index);
        const std::size_t next = width + code_point_width(decoded.code);
        if (next > budget) {
            break;
        }
        result.append(text.substr(index, decoded.length));
        width = next;
        index += decoded.length;
    }
    result.append(kEllipsis);
    return result;
}

bool is_numeric_value(const tinydbms::Value& value) {
    return std::holds_alternative<std::int32_t>(value.data) ||
        std::holds_alternative<std::int64_t>(value.data) ||
        std::holds_alternative<double>(value.data);
}

void write_padding(std::ostream& output, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        output.put(' ');
    }
}

void write_border(
    const std::vector<std::size_t>& widths,
    std::string_view left,
    std::string_view middle,
    std::string_view right,
    std::ostream& output) {
    output << left;
    for (std::size_t index = 0; index < widths.size(); ++index) {
        if (index != 0) {
            output << middle;
        }
        // 每个单元格左右各留一格内边距。
        for (std::size_t column = 0; column < widths[index] + 2U; ++column) {
            output << kHorizontal;
        }
    }
    output << right << '\n';
}

void write_row(
    const std::vector<std::string>& cells,
    const std::vector<std::size_t>& widths,
    const std::vector<bool>& right_aligned,
    std::ostream& output) {
    output << kVertical;
    for (std::size_t index = 0; index < cells.size(); ++index) {
        const std::size_t text_width = display_width(cells[index]);
        const std::size_t padding = widths[index] > text_width
            ? widths[index] - text_width
            : 0;
        output.put(' ');
        if (right_aligned[index]) {
            write_padding(output, padding);
        }
        output << cells[index];
        if (!right_aligned[index]) {
            write_padding(output, padding);
        }
        output.put(' ');
        output << kVertical;
    }
    output.put('\n');
}

}  // namespace

RenderResult write_pretty_query_result(
    const QueryResult& query,
    std::ostream& output,
    std::ostream& error_output,
    PrettyQueryOptions options) {
    for (const auto& row : query.rows) {
        if (row.size() != query.columns.size()) {
            // 与 table 模式同一兜底：写 stderr 诊断并计入失败，退出码为 1。
            return RenderResult{
                write_error(
                    malformed_result_error(
                        "query result row width does not match column count"),
                    error_output),
                true};
        }
    }

    const std::size_t column_count = query.columns.size();
    if (column_count == 0U) {
        output << "0 rows\n";
        return RenderResult{static_cast<bool>(output), false};
    }

    std::vector<std::string> headers;
    headers.reserve(column_count);
    std::vector<std::size_t> widths(column_count, 0);
    std::vector<bool> right_aligned(column_count, false);
    for (std::size_t index = 0; index < column_count; ++index) {
        const std::string header_text = escape_text(query.columns[index].name);
        headers.push_back(
            options.truncate_cells
                ? truncate_to_width(header_text, kMaxCellWidth)
                : header_text);
        widths[index] = display_width(headers.back());
    }

    std::vector<std::vector<std::string>> cells;
    cells.reserve(query.rows.size());
    std::vector<bool> saw_numeric(column_count, false);
    std::vector<bool> saw_non_numeric(column_count, false);
    for (const auto& row : query.rows) {
        std::vector<std::string> rendered;
        rendered.reserve(column_count);
        for (std::size_t index = 0; index < column_count; ++index) {
            const std::string cell_text = escape_text(value_text(row[index]));
            rendered.push_back(
                options.truncate_cells
                    ? truncate_to_width(cell_text, kMaxCellWidth)
                    : cell_text);
            widths[index] = std::max(widths[index], display_width(rendered.back()));
            if (std::holds_alternative<std::monostate>(row[index].data)) {
                continue;
            }
            if (is_numeric_value(row[index])) {
                saw_numeric[index] = true;
            } else {
                saw_non_numeric[index] = true;
            }
        }
        cells.push_back(std::move(rendered));
    }
    for (std::size_t index = 0; index < column_count; ++index) {
        // 全 NULL 列按文本列处理：没有数值证据就不右对齐。
        right_aligned[index] = saw_numeric[index] && !saw_non_numeric[index];
    }

    write_border(widths, kTopLeft, kTopMiddle, kTopRight, output);
    write_row(headers, widths, right_aligned, output);
    write_border(widths, kMidLeft, kMidMiddle, kMidRight, output);
    for (const auto& row : cells) {
        write_row(row, widths, right_aligned, output);
    }
    write_border(widths, kBottomLeft, kBottomMiddle, kBottomRight, output);

    if (options.show_row_count) {
        const std::size_t row_count = query.rows.size();
        output << row_count << (row_count == 1U ? " row\n" : " rows\n");
    }
    return RenderResult{static_cast<bool>(output), false};
}

bool write_pretty_command_result(
    const CommandResult& command,
    std::ostream& output) {
    output << "OK, " << command.affected_rows
           << (command.affected_rows == 1U ? " row affected\n" : " rows affected\n");
    return static_cast<bool>(output);
}

}  // namespace tinydbms::app
