#ifndef TINYDBMS_COMPILER_PLANNER_HPP
#define TINYDBMS_COMPILER_PLANNER_HPP

#include "bound_ast.hpp"
#include "tinydbms/compiler.hpp"

namespace tinydbms::compiler::internal {

struct PlannerResult {
    std::variant<Plan, CompileError> outcome;
};

[[nodiscard]] Plan generate_plan(BoundCreateTable statement);
[[nodiscard]] Plan generate_plan(BoundInsert statement);
[[nodiscard]] PlannerResult generate_plan(BoundSelect statement, CatalogView catalog);
[[nodiscard]] PlannerResult generate_plan(BoundDelete statement, CatalogView catalog);
[[nodiscard]] PlannerResult generate_plan(BoundUpdate statement, CatalogView catalog);

}  // namespace tinydbms::compiler::internal

#endif  // TINYDBMS_COMPILER_PLANNER_HPP
