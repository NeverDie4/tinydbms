#ifndef TINYDBMS_COMPILER_OPTIMIZER_HPP
#define TINYDBMS_COMPILER_OPTIMIZER_HPP

#include <span>
#include <string_view>
#include <vector>

#include "bound_ast.hpp"

namespace tinydbms::compiler::internal {

enum class OptimizationRule {
    kConstantComparison,
    kBooleanSimplification,
    kDoubleNotElimination,
    kRedundantTruePredicateElimination
};

enum class OptimizationRuleScope {
    kComparisonExpression,
    kLogicalExpression,
    kUnaryExpression,
    kStatementPredicate
};

struct OptimizationRuleDescriptor {
    OptimizationRule rule;
    std::string_view name;
    OptimizationRuleScope scope;
};

struct OptimizationEvent {
    OptimizationRule rule;
};

using OptimizationTrace = std::vector<OptimizationEvent>;

struct OptimizationResult {
    BoundStatement statement;
    OptimizationTrace trace;
};

[[nodiscard]] std::span<const OptimizationRuleDescriptor> optimization_rule_registry();
[[nodiscard]] const OptimizationRuleDescriptor& optimization_rule_descriptor(
    OptimizationRule rule);
[[nodiscard]] std::string_view optimization_rule_name(OptimizationRule rule);
[[nodiscard]] std::string_view optimization_rule_scope_name(OptimizationRuleScope scope);
[[nodiscard]] OptimizationResult optimize_with_trace(BoundStatement statement);
[[nodiscard]] BoundStatement optimize(BoundStatement statement);

}  // namespace tinydbms::compiler::internal

#endif  // TINYDBMS_COMPILER_OPTIMIZER_HPP
