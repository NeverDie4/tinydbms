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

enum class ConstantTruth {
    kFalse,
    kTrue,
    kUnknown
};

struct OptimizedExpr {
    BoundExpr expression;
    std::optional<ConstantTruth> constant_truth;
};

[[nodiscard]] ConstantTruth truth_value(bool value) {
    return value ? ConstantTruth::kTrue : ConstantTruth::kFalse;
}

[[nodiscard]] BoundExpr truth_literal(ConstantTruth value) {
    if (value == ConstantTruth::kUnknown) {
        return BoundExpr{BoundLiteral{Value{std::monostate{}}}};
    }
    return BoundExpr{BoundLiteral{Value{value == ConstantTruth::kTrue}}};
}

[[nodiscard]] ConstantTruth truth_not(ConstantTruth value) {
    if (value == ConstantTruth::kUnknown) {
        return value;
    }
    return value == ConstantTruth::kTrue
        ? ConstantTruth::kFalse
        : ConstantTruth::kTrue;
}

[[nodiscard]] ConstantTruth truth_and(ConstantTruth lhs, ConstantTruth rhs) {
    if (lhs == ConstantTruth::kFalse || rhs == ConstantTruth::kFalse) {
        return ConstantTruth::kFalse;
    }
    if (lhs == ConstantTruth::kUnknown || rhs == ConstantTruth::kUnknown) {
        return ConstantTruth::kUnknown;
    }
    return ConstantTruth::kTrue;
}

[[nodiscard]] ConstantTruth truth_or(ConstantTruth lhs, ConstantTruth rhs) {
    if (lhs == ConstantTruth::kTrue || rhs == ConstantTruth::kTrue) {
        return ConstantTruth::kTrue;
    }
    if (lhs == ConstantTruth::kUnknown || rhs == ConstantTruth::kUnknown) {
        return ConstantTruth::kUnknown;
    }
    return ConstantTruth::kFalse;
}

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

[[nodiscard]] bool compare_integer(CmpOp op, std::int64_t lhs, std::int64_t rhs) {
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

[[nodiscard]] bool compare_double(CmpOp op, double lhs, double rhs) {
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

[[nodiscard]] std::optional<std::int64_t> integer_value(const Value& value) {
    if (const auto* integer = std::get_if<std::int32_t>(&value.data)) {
        return static_cast<std::int64_t>(*integer);
    }
    if (const auto* bigint = std::get_if<std::int64_t>(&value.data)) {
        return *bigint;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<double> numeric_double_value(const Value& value) {
    if (const auto* number = std::get_if<double>(&value.data)) {
        return *number;
    }
    if (const auto* integer = std::get_if<std::int32_t>(&value.data)) {
        return static_cast<double>(*integer);
    }
    if (const auto* bigint = std::get_if<std::int64_t>(&value.data)) {
        return static_cast<double>(*bigint);
    }
    return std::nullopt;
}

[[nodiscard]] bool compare_string(CmpOp op, const std::string& lhs, const std::string& rhs) {
    return op == CmpOp::kEq ? lhs == rhs : lhs != rhs;
}

[[nodiscard]] std::optional<bool> compare_boolean(
    CmpOp op,
    const Value& lhs,
    const Value& rhs) {
    const auto* lhs_boolean = std::get_if<bool>(&lhs.data);
    const auto* rhs_boolean = std::get_if<bool>(&rhs.data);
    if (lhs_boolean == nullptr || rhs_boolean == nullptr) {
        return std::nullopt;
    }
    if (op == CmpOp::kEq) {
        return *lhs_boolean == *rhs_boolean;
    }
    if (op == CmpOp::kNe) {
        return *lhs_boolean != *rhs_boolean;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ConstantTruth> evaluate_literals(
    CmpOp op,
    const BoundExpr& lhs,
    const BoundExpr& rhs) {
    const auto* lhs_literal = std::get_if<BoundLiteral>(&lhs.kind);
    const auto* rhs_literal = std::get_if<BoundLiteral>(&rhs.kind);
    if (lhs_literal == nullptr || rhs_literal == nullptr) {
        return std::nullopt;
    }
    if (std::holds_alternative<std::monostate>(lhs_literal->value.data) ||
        std::holds_alternative<std::monostate>(rhs_literal->value.data)) {
        return ConstantTruth::kUnknown;
    }

    if (std::holds_alternative<bool>(lhs_literal->value.data) ||
        std::holds_alternative<bool>(rhs_literal->value.data)) {
        const std::optional<bool> result = compare_boolean(op, lhs_literal->value, rhs_literal->value);
        return result.has_value()
            ? std::optional<ConstantTruth>{truth_value(*result)}
            : std::nullopt;
    }

    if (std::holds_alternative<double>(lhs_literal->value.data) ||
        std::holds_alternative<double>(rhs_literal->value.data)) {
        const std::optional<double> lhs_number = numeric_double_value(lhs_literal->value);
        const std::optional<double> rhs_number = numeric_double_value(rhs_literal->value);
        if (lhs_number.has_value() && rhs_number.has_value()) {
            return truth_value(compare_double(op, *lhs_number, *rhs_number));
        }
        return std::nullopt;
    }

    const std::optional<std::int64_t> lhs_integer = integer_value(lhs_literal->value);
    const std::optional<std::int64_t> rhs_integer = integer_value(rhs_literal->value);
    if (lhs_integer.has_value() && rhs_integer.has_value()) {
        return truth_value(compare_integer(op, *lhs_integer, *rhs_integer));
    }
    const auto* lhs_string = std::get_if<std::string>(&lhs_literal->value.data);
    const auto* rhs_string = std::get_if<std::string>(&rhs_literal->value.data);
    if (lhs_string == nullptr || rhs_string == nullptr) {
        return std::nullopt;
    }
    return truth_value(compare_string(op, *lhs_string, *rhs_string));
}

[[nodiscard]] OptimizedExpr apply_constant_comparison_rule(
    CmpOp op,
    OptimizedExpr lhs,
    OptimizedExpr rhs,
    OptimizationTrace& trace) {
    const std::optional<ConstantTruth> constant =
        evaluate_literals(op, lhs.expression, rhs.expression);
    if (constant.has_value()) {
        trace.push_back(OptimizationEvent{
            OptimizationRule::kConstantComparison
        });
    }
    if (constant == ConstantTruth::kUnknown) {
        return OptimizedExpr{truth_literal(*constant), constant};
    }
    return OptimizedExpr{
        BoundExpr{BoundBinaryExpr{
            op,
            std::make_unique<BoundExpr>(std::move(lhs.expression)),
            std::make_unique<BoundExpr>(std::move(rhs.expression))}},
        constant};
}

[[nodiscard]] OptimizedExpr apply_boolean_simplification_rule(
    LogicOp op,
    OptimizedExpr lhs,
    OptimizedExpr rhs,
    OptimizationTrace& trace) {
    if (lhs.constant_truth.has_value() && rhs.constant_truth.has_value()) {
        const ConstantTruth result = op == LogicOp::kAnd
            ? truth_and(*lhs.constant_truth, *rhs.constant_truth)
            : truth_or(*lhs.constant_truth, *rhs.constant_truth);
        trace.push_back(OptimizationEvent{OptimizationRule::kBooleanSimplification});
        if (result == ConstantTruth::kUnknown) {
            return OptimizedExpr{truth_literal(result), result};
        }
        if (lhs.constant_truth == result) {
            return lhs;
        }
        if (rhs.constant_truth == result) {
            return rhs;
        }
        return OptimizedExpr{truth_literal(result), result};
    }

    if (op == LogicOp::kAnd) {
        if (lhs.constant_truth == ConstantTruth::kTrue ||
            lhs.constant_truth == ConstantTruth::kFalse) {
            trace.push_back(OptimizationEvent{
                OptimizationRule::kBooleanSimplification
            });
            return lhs.constant_truth == ConstantTruth::kTrue
                ? std::move(rhs)
                : std::move(lhs);
        }
        if (rhs.constant_truth == ConstantTruth::kTrue ||
            rhs.constant_truth == ConstantTruth::kFalse) {
            trace.push_back(OptimizationEvent{
                OptimizationRule::kBooleanSimplification
            });
            return rhs.constant_truth == ConstantTruth::kTrue
                ? std::move(lhs)
                : std::move(rhs);
        }
    } else {
        if (lhs.constant_truth == ConstantTruth::kTrue ||
            lhs.constant_truth == ConstantTruth::kFalse) {
            trace.push_back(OptimizationEvent{
                OptimizationRule::kBooleanSimplification
            });
            return lhs.constant_truth == ConstantTruth::kTrue
                ? std::move(lhs)
                : std::move(rhs);
        }
        if (rhs.constant_truth == ConstantTruth::kTrue ||
            rhs.constant_truth == ConstantTruth::kFalse) {
            trace.push_back(OptimizationEvent{
                OptimizationRule::kBooleanSimplification
            });
            return rhs.constant_truth == ConstantTruth::kTrue
                ? std::move(rhs)
                : std::move(lhs);
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
    std::optional<ConstantTruth> constant;
    if (operand.constant_truth.has_value()) {
        constant = truth_not(*operand.constant_truth);
    }
    if (auto* inner = std::get_if<BoundUnaryExpr>(&operand.expression.kind);
        inner != nullptr && inner->op == UnaryOp::kNot) {
        trace.push_back(OptimizationEvent{
            OptimizationRule::kDoubleNotElimination
        });
        BoundExpr simplified = std::move(*inner->operand);
        return OptimizedExpr{std::move(simplified), constant};
    }
    if (constant == ConstantTruth::kUnknown) {
        return OptimizedExpr{truth_literal(*constant), constant};
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
    if (optimized.constant_truth == ConstantTruth::kTrue) {
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
    if (std::holds_alternative<BoundColumnRef>(expression.kind)) {
        return OptimizedExpr{std::move(expression), std::nullopt};
    }
    if (const auto* literal = std::get_if<BoundLiteral>(&expression.kind)) {
        const auto* boolean = std::get_if<bool>(&literal->value.data);
        const bool is_null = std::holds_alternative<std::monostate>(literal->value.data);
        return OptimizedExpr{
            std::move(expression),
            boolean != nullptr
                ? std::optional<ConstantTruth>{truth_value(*boolean)}
                : (is_null
                    ? std::optional<ConstantTruth>{ConstantTruth::kUnknown}
                    : std::nullopt)};
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

    if (auto* null_test = std::get_if<BoundNullTestExpr>(&expression.kind)) {
        OptimizedExpr operand = optimize_expression(std::move(*null_test->operand), trace);
        if (const auto* literal = std::get_if<BoundLiteral>(&operand.expression.kind)) {
            const bool is_null = std::holds_alternative<std::monostate>(literal->value.data);
            const bool result = null_test->op == NullTestOp::kIsNull ? is_null : !is_null;
            return OptimizedExpr{BoundExpr{BoundLiteral{Value{result}}}, truth_value(result)};
        }
        return OptimizedExpr{
            BoundExpr{BoundNullTestExpr{
                null_test->op,
                std::make_unique<BoundExpr>(std::move(operand.expression))}},
            std::nullopt};
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

void optimize_join_condition(BoundExprPtr& condition, OptimizationTrace& trace) {
    OptimizedExpr optimized = optimize_expression(std::move(*condition), trace);
    condition = std::make_unique<BoundExpr>(std::move(optimized.expression));
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
        for (BoundJoin& join : select->joins) {
            optimize_join_condition(join.condition, trace);
        }
        optimize_predicate(select->predicate, trace);
    } else if (auto* deletion = std::get_if<BoundDelete>(&statement.kind)) {
        optimize_predicate(deletion->predicate, trace);
    } else if (auto* update = std::get_if<BoundUpdate>(&statement.kind)) {
        optimize_predicate(update->predicate, trace);
    }
    return OptimizationResult{std::move(statement), std::move(trace)};
}

BoundStatement optimize(BoundStatement statement) {
    OptimizationResult result = optimize_with_trace(std::move(statement));
    return std::move(result.statement);
}

}  // namespace tinydbms::compiler::internal
