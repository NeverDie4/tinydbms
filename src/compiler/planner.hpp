#ifndef TINYDBMS_COMPILER_PLANNER_HPP
#define TINYDBMS_COMPILER_PLANNER_HPP

#include "bound_ast.hpp"
#include "tinydbms/compiler.hpp"

namespace tinydbms::compiler::internal {

[[nodiscard]] Plan generate_plan(BoundCreateTable statement);
[[nodiscard]] Plan generate_plan(BoundInsert statement);
[[nodiscard]] Plan generate_plan(BoundSelect statement);
[[nodiscard]] Plan generate_plan(BoundDelete statement);

}  // namespace tinydbms::compiler::internal

#endif  // TINYDBMS_COMPILER_PLANNER_HPP
