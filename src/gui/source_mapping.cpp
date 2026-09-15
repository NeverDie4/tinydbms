#include "source_mapping.hpp"

#include <algorithm>

namespace tinydbms::gui {
namespace {

// 一个 UTF-16 码元（高代理项时连同低代理项）对应的字节宽度与码元宽度。
// 换算规则与注释见 utf8_offset_to_index：孤立代理项不是合法 UTF-8，
// 按替换字符的 3 字节防御处理。
struct UnitWidth {
    int units = 1;
    std::size_t bytes = 1;
};

UnitWidth unit_width(const QString& text, int index) {
    const QChar unit = text.at(index);
    if (unit.isHighSurrogate()) {
        if (index + 1 < text.size() && text.at(index + 1).isLowSurrogate()) {
            return UnitWidth{2, 4};
        }
        return UnitWidth{1, 3};
    }
    if (unit.isLowSurrogate()) {
        return UnitWidth{1, 3};
    }
    const char16_t code = unit.unicode();
    if (code < 0x80U) {
        return UnitWidth{1, 1};
    }
    if (code < 0x800U) {
        return UnitWidth{1, 2};
    }
    return UnitWidth{1, 3};
}

}  // namespace

std::optional<int> utf8_offset_to_index(const QString& text, std::size_t byte_offset) {
    std::size_t consumed = 0;
    int index = 0;

    while (index < text.size()) {
        const UnitWidth width = unit_width(text, index);
        if (consumed + width.bytes > byte_offset) {
            return index;
        }

        consumed += width.bytes;
        index += width.units;

        if (consumed == byte_offset) {
            return index;
        }
    }

    if (consumed == byte_offset) {
        return index;
    }
    return std::nullopt;
}

std::vector<std::optional<EditorRange>> editor_ranges(
    const QString& text,
    const std::vector<tinydbms::SourceRange>& ranges) {
    struct Target {
        std::size_t offset = 0;
        std::optional<int>* slot = nullptr;
    };

    std::vector<std::optional<int>> begin_indices(ranges.size());
    std::vector<std::optional<int>> end_indices(ranges.size());
    std::vector<Target> targets;
    targets.reserve(ranges.size() * 2U);
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        targets.push_back(Target{ranges[index].begin.byte_offset, &begin_indices[index]});
        targets.push_back(Target{ranges[index].end.byte_offset, &end_indices[index]});
    }
    std::stable_sort(
        targets.begin(),
        targets.end(),
        [](const Target& left, const Target& right) { return left.offset < right.offset; });

    // 一次遍历文本，按偏移升序解析所有目标；偏移落在字符内部时归位到该字符起点。
    std::size_t consumed = 0;
    int position = 0;
    std::size_t next = 0;
    while (next < targets.size() && targets[next].offset == consumed) {
        *targets[next].slot = position;
        ++next;
    }
    while (next < targets.size() && position < text.size()) {
        const UnitWidth width = unit_width(text, position);
        if (consumed + width.bytes > targets[next].offset) {
            *targets[next].slot = position;
            ++next;
            continue;
        }
        consumed += width.bytes;
        position += width.units;
        while (next < targets.size() && targets[next].offset == consumed) {
            *targets[next].slot = position;
            ++next;
        }
    }
    // 剩余目标都大于文本总字节数，保持 nullopt。

    std::vector<std::optional<EditorRange>> results(ranges.size());
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        if (!begin_indices[index].has_value() || !end_indices[index].has_value() ||
            *end_indices[index] < *begin_indices[index]) {
            continue;
        }
        results[index] = EditorRange{*begin_indices[index], *end_indices[index]};
    }
    return results;
}

std::optional<EditorRange> editor_range(
    const QString& text,
    const tinydbms::SourceRange& range) {
    const std::optional<int> begin = utf8_offset_to_index(text, range.begin.byte_offset);
    const std::optional<int> end = utf8_offset_to_index(text, range.end.byte_offset);
    if (!begin.has_value() || !end.has_value() || *end < *begin) {
        return std::nullopt;
    }
    return EditorRange{*begin, *end};
}

}  // namespace tinydbms::gui
