#ifndef TINYDBMS_CORE_PLAN_TEXT_HPP
#define TINYDBMS_CORE_PLAN_TEXT_HPP

#include "tinydbms/compiler.hpp"
#include "tinydbms/core.hpp"

#include <span>
#include <variant>

namespace tinydbms::core::internal {

// 把 compiler 产出的 Plan 渲染成稳定的多行文本，包装成单列 VARCHAR（列名 plan）的
// QueryResult：每条语句一行文本，未执行的语句不会走到这里。
//
// 渲染只读 Catalog，用于把 TableId/ColumnId 解析成人名；解析不到时退化成 table#N /
// slot#N / column#N。Plan 违反 compiler 契约（空 root、空子节点、空操作数）时返回
// kInternal Error，由 script.cpp 按致命中止处理，避免渲染器在空指针上崩溃。
std::variant<QueryResult, Error> render_plan_result(
    const compiler::Plan& plan,
    std::span<const TableMeta> catalog);

}  // namespace tinydbms::core::internal

#endif  // TINYDBMS_CORE_PLAN_TEXT_HPP
