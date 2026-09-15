#ifndef TINYDBMS_GUI_SOURCE_MAPPING_HPP
#define TINYDBMS_GUI_SOURCE_MAPPING_HPP

#include <cstddef>
#include <optional>
#include <vector>

#include <QString>

#include "tinydbms/diagnostic.hpp"

namespace tinydbms::gui {

// QTextDocument 位置用 UTF-16 码元下标；end == begin 表示插入点。
struct EditorRange {
    int begin = 0;
    int end = 0;
};

// 契约坐标（diagnostic.hpp）的行列与偏移都按 UTF-8 字节计算，编辑器用 UTF-16 码元，
// 因此必须显式换算。偏移落在多字节字符内部时归位到该字符起点；越界返回 nullopt。
std::optional<int> utf8_offset_to_index(const QString& text, std::size_t byte_offset);

// 半开区间 [begin.byte_offset, end.byte_offset) 的换算；end 小于 begin 或越界返回 nullopt。
std::optional<EditorRange> editor_range(
    const QString& text,
    const tinydbms::SourceRange& range);

// 批量换算：一次遍历文本解析所有范围，顺序与输入一致，结果与逐个调用 editor_range 相同。
// 单个范围的换算需要从头扫描文本，诊断数多时会退化成 O(范围数 × 文本长度)；
// 这里的复杂度是 O(文本长度 + 范围数 log 范围数)。
std::vector<std::optional<EditorRange>> editor_ranges(
    const QString& text,
    const std::vector<tinydbms::SourceRange>& ranges);

}  // namespace tinydbms::gui

#endif  // TINYDBMS_GUI_SOURCE_MAPPING_HPP
