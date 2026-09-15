#ifndef TINYDBMS_GUI_RESULT_EXPORT_HPP
#define TINYDBMS_GUI_RESULT_EXPORT_HPP

#include <QByteArray>
#include <QString>

#include "tinydbms/core.hpp"

namespace tinydbms::gui {

// 结果导出：取值口径与表格一致（复用 value_text），不重新解释 SQL 值。
// CSV 按 RFC 4180 的口径转义——字段包含分隔符、双引号或 CR/LF 时整体加引号，
// 内部的双引号写成两个；行尾用 CRLF。tsv_text() 供剪贴板使用，规则相同但分隔符
// 是制表符、行尾是 LF，便于直接粘进表格软件。
QString csv_text(const tinydbms::core::QueryResult& query);
QString tsv_text(const tinydbms::core::QueryResult& query);

// 写文件用的字节：UTF-8，并在最前面加 BOM，Excel 双击打开时中文不会乱码。
QByteArray csv_bytes(const tinydbms::core::QueryResult& query);

}  // namespace tinydbms::gui

#endif  // TINYDBMS_GUI_RESULT_EXPORT_HPP
