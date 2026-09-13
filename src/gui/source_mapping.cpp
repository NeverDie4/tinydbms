#include "source_mapping.hpp"

#include <algorithm>

namespace tinydbms::gui {

std::optional<int> utf8_offset_to_index(const QString& text, std::size_t byte_offset) {
    std::size_t consumed = 0;
    int index = 0;

    while (index < text.size()) {
        const QChar unit = text.at(index);
        int units = 1;
        std::size_t bytes = 1;

        if (unit.isHighSurrogate()) {
            if (index + 1 < text.size() && text.at(index + 1).isLowSurrogate()) {
                units = 2;
                bytes = 4;
            } else {
                // 孤立代理项不是合法 UTF-8；按替换字符的 3 字节防御处理。
                bytes = 3;
            }
        } else if (unit.isLowSurrogate()) {
            bytes = 3;
        } else {
            const char16_t code = unit.unicode();
            if (code < 0x80U) {
                bytes = 1;
            } else if (code < 0x800U) {
                bytes = 2;
            } else {
                bytes = 3;
            }
        }

        if (consumed + bytes > byte_offset) {
            return index;
        }

        consumed += bytes;
        index += units;

        if (consumed == byte_offset) {
            return index;
        }
    }

    if (consumed == byte_offset) {
        return index;
    }
    return std::nullopt;
}

std::optional<EditorRange> editor_range(
    const QString& text,
    const tinydbms::SourceRange& range) {
    const std::optional<int> begin = utf8_offset_to_index(text, range.begin_offset);
    const std::optional<int> end = utf8_offset_to_index(text, range.end_offset);
    if (!begin.has_value() || !end.has_value() || *end < *begin) {
        return std::nullopt;
    }
    return EditorRange{*begin, *end};
}

}  // namespace tinydbms::gui
