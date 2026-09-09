#include "optimizer.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace tinydbms::compiler::internal {
namespace {

struct OptimizedExpr {
    BoundExpr expression;
    std::optional<bool> constant_bool;
};

constexpr std::array<OptimizationRuleDescriptor, 4> kOptimizationRuleRegistry{
    OptimizationRuleDescriptor{
        OptimizationRule::kConstantComparison,
        "ConstantComparison",
        OptimizationRuleScope::kComparisonExpression
    },
    OptimizationRuleDescriptor{
        OptimizationRule::kBooleanSimplification,
        "BooleanSimplification",
        OptimizationRuleScope::kLogicalExpression
    },
    OptimizationRuleDescriptor{
        OptimizationRule::kDoubleNotElimination,
        "DoubleNotElimination",
        OptimizationRuleScope::kUnaryExpression
    },
    OptimizationRuleDescriptor{
        OptimizationRule::kRedundantTruePredicateElimination,
        "RedundantTruePredicateElimination",
        OptimizationRuleScope::kStatementPredicate
    },
};

[[nodiscard]] constexpr const OptimizationRuleDescriptor* find_rule_descriptor(
    OptimizationRule rule) {
    for (const OptimizationRuleDescriptor& descriptor : kOptimizationRuleRegistry) {
        if (descriptor.rule == rule) {
            return &descriptor;
        }
    }
    return nullptr;
}

[[nodiscard]] bool compare_int(CmpOp op, std::int32_t lhs, std::int32_t rhs) {
    switch (op) {
        case CmpOp::kEq:
            return lhs == rhs;
        case CmpOp::kNe:
            return lhs != rhs;
        case CmpOp::kLt:
            return lhs < rhs;
        case CmpOp::kLe:
            return lhs <= rhs;
        case CmpOp::kGt:
            return lhs > rhs;
        case CmpOp::kGe:
            return lhs >= rhs;
    }
    return false;
}

[[nodiscard]] bool compare_string(CmpOp op, const std::string& lhs, const std::string& rhs) {
    return op == CmpOp::kEq ? lhs == rhs : lhs != rhs;
}

[[nodiscard]] std::optional<bool> evaluate_literals(
    CmpOp op,
    const BoundExpr& lhs,
    const BoundExpr& rhs) {
    const auto* lhs_literal = std::get_if<BoundLiteral>(&lhs.kind);
    const auto* rhs_literal = std::get_if<BoundLiteral>(&rhs.kind);
    if (lhs_literal == nullptr || rhs_literal == nullptr) {
        return std::nullopt;
    }

    if (const auto* lhs_int = std::get_if<std::int32_t>(&lhs_literal->value.data)) {
        return compare_int(op, *lhs_int, std::get<std::int32_t>(rhs_literal->value.data));
    }
    return compare_string(
        op,
        std::get<std::string>(lhs_literal->value.data),
        std::get<std::string>(rhs_literal->value.data));
}

[[nodiscard]] OptimizedExpr apply_constant_comparison_rule(
    CmpOp op,
    OptimizedExpr lhs,
    OptimizedExpr rhs,
    OptimizationTrace& trace) {
    const std::optional<bool> constant =
        evaluate_literals(op, lhs.expression, rhs.expression);
    if (constant.has_value()) {
        trace.push_back(OptimizationEvent{
            OptimizationRule::kConstantComparison
        });
    }
    return OptimizedExpr{
        BoundExpr{BoundBinaryExpr{
            op,
            std::make_unique<BoundExpr>(std::move(lhs.expression)),
            std::make_unique<BoundExpr>(std::move(rhs.expression))
        }},
        constant
    };
}

[[nodiscard]] OptimizedExpr apply_boolean_simplification_rule(
    LogicOp op,
    OptimizedExpr lhs,
    OptimizedExpr rhs,
    OptimizationTrace& trace) {
    if (op == LogicOp::kAnd) {
        if (lhs.constant_bool.has_value()) {
            trace.push_back(OptimizationEvent{
                OptimizationRule::kBooleanSimplification
            });
            return *lhs.constant_bool ? std::move(rhs) : std::move(lhs);
        }
        if (rhs.constant_bool.has_value()) {
            trace.push_back(OptimizationEvent{
                OptimizationRule::kBooleanSimplification
            });
            return *rhs.constant_bool ? std::move(lhs) : std::move(rhs);
        }
    } else {
        if (lhs.constant_bool.has_value()) {
            trace.push_back(OptimizationEvent{
                OptimizationRule::kBooleanSimplification
            });
            return *lhs.constant_bool ? std::move(lhs) : std::move(rhs);
        }
        if (rhs.constant_bool.has_value()) {
            trace.push_back(OptimizationEvent{
                OptimizationRule::kBooleanSimplification
            });
            return *rhs.constant_bool ? std::move(rhs) : std::move(lhs);
        }
    }

    return OptimizedExpr{
        BoundExpr{BoundBinaryExpr{
            op,
            std::make_unique<BoundExpr>(std::move(lhs.expression)),
            std::make_unique<BoundExpr>(std::move(rhs.expression))
        }},
        std::nullopt
    };
}

[[nodiscard]] OptimizedExpr apply_double_not_rule(
    OptimizedExpr operand,
    OptimizationTrace& trace) {
    std::optional<bool> constant;
    if (operand.constant_bool.has_value()) {
        constant = !*operand.constant_bool;
    }
    if (auto* inner = std::get_if<BoundUnaryExpr>(&operand.expression.kind);
        inner != nullptr && inner->op == UnaryOp::kNot) {
        trace.push_back(OptimizationEvent{
            OptimizationRule::kDoubleNotElimination
        });
        BoundExpr simplified = std::move(*inner->operand);
        return OptimizedExpr{std::move(simplified), constant};
    }
    return OptimizedExpr{
        BoundExpr{BoundUnaryExpr{
            std::make_unique<BoundExpr>(std::move(operand.expression))
        }},
        constant
    };
}

void apply_redundant_true_predicate_rule(
    BoundExprPtr& predicate,
    OptimizedExpr optimized,
    OptimizationTrace& trace) {
    if (optimized.constant_bool == true) {
        trace.push_back(OptimizationEvent{
            OptimizationRule::kRedundantTruePredicateElimination
        });
        predicate.reset();
        return;
    }
    predicate = std::make_unique<BoundExpr>(std::move(optimized.expression));
}

[[nodiscard]] OptimizedExpr optimize_expression(
    BoundExpr expression,
    OptimizationTrace& trace) {
    if (std::holds_alternative<BoundColumnRef>(expression.kind) ||
        std::holds_alternative<BoundLiteral>(expression.kind)) {
        return OptimizedExpr{std::move(expression), std::nullopt};
    }

    if (auto* binary = std::get_if<BoundBinaryExpr>(&expression.kind)) {
        OptimizedExpr lhs = optimize_expression(std::move(*binary->lhs), trace);
        OptimizedExpr rhs = optimize_expression(std::move(*binary->rhs), trace);

        if (const auto* comparison = std::get_if<CmpOp>(&binary->op)) {
            return apply_constant_comparison_rule(
                *comparison,
                std::move(lhs),
                std::move(rhs),
                trace);
        }

        return apply_boolean_simplification_rule(
            std::get<LogicOp>(binary->op),
            std::move(lhs),
            std::move(rhs),
            trace);
    }

    auto& unary = std::get<BoundUnaryExpr>(expression.kind);
    OptimizedExpr operand = optimize_expression(std::move(*unary.operand), trace);
    return apply_double_not_rule(std::move(operand), trace);
}

void optimize_predicate(BoundExprPtr& predicate, OptimizationTrace& trace) {
    if (predicate == nullptr) {
        return;
    }

    OptimizedExpr optimized = optimize_expression(std::move(*predicate), trace);
    apply_redundant_true_predicate_rule(predicate, std::move(optimized), trace);
}

}  // namespace

std::span<const OptimizationRuleDescriptor> optimization_rule_registry() {
    return std::span<const OptimizationRuleDescriptor>{kOptimizationRuleRegistry};
}

const OptimizationRuleDescriptor& optimization_rule_descriptor(OptimizationRule rule) {
    const OptimizationRuleDescriptor* descriptor = find_rule_descriptor(rule);
    if (descriptor == nullptr) {
        std::terminate();
    }
    return *descriptor;
}

std::string_view optimization_rule_name(OptimizationRule rule) {
    const OptimizationRuleDescriptor* descriptor = find_rule_descriptor(rule);
    return descriptor == nullptr ? "Unknown" : descriptor->name;
}

std::string_view optimization_rule_scope_name(OptimizationRuleScope scope) {
    switch (scope) {
        case OptimizationRuleScope::kComparisonExpression:
            return "ComparisonExpression";
        case OptimizationRuleScope::kLogicalExpression:
            return "LogicalExpression";
        case OptimizationRuleScope::kUnaryExpression:
            return "UnaryExpression";
        case OptimizationRuleScope::kStatementPredicate:
            return "StatementPredicate";
    }
    return "Unknown";
}

OptimizationResult optimize_with_trace(BoundStatement statement) {
    OptimizationTrace trace;
    if (auto* select = std::get_if<BoundSelect>(&statement.kind)) {
        optimize_predicate(select->predicate, trace);
    } else if (auto* deletion = std::get_if<BoundDelete>(&statement.kind)) {
        optimize_predicate(deletion->predicate, trace);
    }
    return OptimizationResult{std::move(statement), std::move(trace)};
}

BoundStatement optimize(BoundStatement statement) {
    OptimizationResult result = optimize_with_trace(std::move(statement));
    return std::move(result.statement);
}

}  // namespace tinydbms::compiler::internal
