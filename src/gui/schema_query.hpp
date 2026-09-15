#ifndef TINYDBMS_GUI_SCHEMA_QUERY_HPP
#define TINYDBMS_GUI_SCHEMA_QUERY_HPP

#include <string_view>

namespace tinydbms::gui {

// 表结构面板读取系统表的只读脚本。
//
// 分句契约（docs/消息契约详细设计.md）要求每条语句都以 ';' 结束，没有“末条可省略”
// 的例外：缺少结尾分号时 compiler 会把整条语句报成 syntax error，面板会退化成
// “无法读取系统表”。这里放在独立的 Qt-free 头文件里，便于真实 compiler 的联调测试
// 直接引用同一份文本，避免测试与实现各写一遍导致漂移。
inline constexpr std::string_view kSchemaQueryText =
    "SELECT * FROM tdb_sys_tables; SELECT * FROM tdb_sys_columns;";

}  // namespace tinydbms::gui

#endif  // TINYDBMS_GUI_SCHEMA_QUERY_HPP
