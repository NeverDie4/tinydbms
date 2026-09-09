#include "expression.hpp"

#include <cstdint>
#include <type_traits>
#include <utility>

namespace tinydbms::core::internal::expression {
namespace {

Error make_error(std::string message) {
    return Error{std::move(message)};
}

ExprType type_of_value(const tinydbms::Value& value) {
    if (std::holds_alternative<std::int32_t>(value.data)) {
        return ExprType::kInt;
    }
    return ExprType::kVarchar;
}

ExprType type_of_column(const ColumnMeta& column) {
    return column.type == Type::kInt ? ExprType::kInt : ExprType::kVarchar;
}

bool value_matches_column(const tinydbms::Value& value, const ColumnMeta& column) {
    const bool is_int = std::holds_alternative<std::int32_t>(value.data);
    return (column.type == Type::kInt) == is_int;
}

bool is_ordered_comparison(compiler::CmpOp op) {
    return op == compiler::CmpOp::kLt || op == compiler::CmpOp::kLe ||
        op == compiler::CmpOp::kGt || op == compiler::CmpOp::kGe;
}

bool is_supported_comparison(compiler::CmpOp op) {
    switch (op) {
        case compiler::CmpOp::kEq:
        case compiler::CmpOp::kNe:
        case compiler::CmpOp::kLt:
        case compiler::CmpOp::kLe:
        case compiler::CmpOp::kGt:
        case compiler::CmpOp::kGe:
            return true;
    }
    return false;
}

bool is_supported_logic(compiler::LogicOp op) {
    return op == compiler::LogicOp::kAnd || op == compiler::LogicOp::kOr;
}

bool compare_ints(std::int32_t lhs, std::int32_t rhs, compiler::CmpOp op) {
    switch (op) {
        case compiler::CmpOp::kEq:
            return lhs == rhs;
        case compiler::CmpOp::kNe:
            return lhs != rhs;
        case compiler::CmpOp::kLt:
            return lhs < rhs;
        case compiler::CmpOp::kLe:
            return lhs <= rhs;
        case compiler::CmpOp::kGt:
            return lhs > rhs;
        case compiler::CmpOp::kGe:
            return lhs >= rhs;
    }
    return false;
}

bool compare_strings(
    const std::string& lhs,
    const std::string& rhs,
    compiler::CmpOp op) {
    switch (op) {
        case compiler::CmpOp::kEq:
            return lhs == rhs;
        case compiler::CmpOp::kNe:
            return lhs != rhs;
        case compiler::CmpOp::kLt:
        case compiler::CmpOp::kLe:
        case compiler::CmpOp::kGt:
        case compiler::CmpOp::kGe:
            return false;
    }
    return false;
}

ValidationResult validate_impl(
    const compiler::Expr& expr,
    const TableMeta& table,
    std::size_t depth) {
    if (depth >= kMaxExpressionDepth) {
        return make_error("expression depth exceeds core limit");
    }
    return std::visit(
        [&table, depth](const auto& node) -> ValidationResult {
            using NodeType = std::decay_t<decltype(node)>;

            if constexpr (std::is_same_v<NodeType, compiler::ColumnRef>) {
                if (node.column_id >= table.columns.size()) {
                    return make_error("expression column id is out of range");
                }
                return type_of_column(table.columns[node.column_id]);
            } else if constexpr (std::is_same_v<NodeType, compiler::Literal>) {
                return type_of_value(node.value);
            } else if constexpr (std::is_same_v<NodeType, compiler::Binary>) {
                if (!node.lhs || !node.rhs) {
                    return make_error("binary expression has a null child");
                }

                ValidationResult lhs = validate_impl(*node.lhs, table, depth + 1U);
                if (const Error* error = std::get_if<Error>(&lhs)) {
                    return *error;
                }
                ValidationResult rhs = validate_impl(*node.rhs, table, depth + 1U);
                if (const Error* error = std::get_if<Error>(&rhs)) {
                    return *error;
                }

                const ExprType lhs_type = std::get<ExprType>(lhs);
                const ExprType rhs_type = std::get<ExprType>(rhs);
                return std::visit(
                    [lhs_type, rhs_type](const auto& op) -> ValidationResult {
                        using OpType = std::decay_t<decltype(op)>;
                        if constexpr (std::is_same_v<OpType, compiler::CmpOp>) {
                            if (!is_supported_comparison(op)) {
                                return make_error("unknown comparison operator");
                            }
                            if ((lhs_type != ExprType::kInt && lhs_type != ExprType::kVarchar) ||
                                lhs_type != rhs_type) {
                                return make_error("comparison operands must have the same value type");
                            }
                            if (lhs_type == ExprType::kVarchar && is_ordered_comparison(op)) {
                                return make_error("VARCHAR only supports equality comparisons");
                            }
                            return ExprType::kBool;
                        } else {
                            if (!is_supported_logic(op)) {
                                return make_error("unknown logical operator");
                            }
                            if (lhs_type != ExprType::kBool || rhs_type != ExprType::kBool) {
                                return make_error("logical operands must be BOOL");
                            }
                            return ExprType::kBool;
                        }
                    },
                    node.op);
            } else {
                if (node.op != compiler::UnaryOp::kNot) {
                    return make_error("unknown unary operator");
                }
                if (!node.operand) {
                    return make_error("unary expression has a null operand");
                }
                ValidationResult operand = validate_impl(*node.operand, table, depth + 1U);
                if (const Error* error = std::get_if<Error>(&operand)) {
                    return *error;
                }
                if (std::get<ExprType>(operand) != ExprType::kBool) {
                    return make_error("NOT operand must be BOOL");
                }
                return ExprType::kBool;
            }
        },
        expr.kind);
}

EvaluationResult evaluate_impl(
    const compiler::Expr& expr,
    const TableMeta& table,
    const Row& row,
    std::size_t depth) {
    if (depth >= kMaxExpressionDepth) {
        return make_error("expression depth exceeds core limit");
    }
    return std::visit(
        [&table, &row, depth](const auto& node) -> EvaluationResult {
            using NodeType = std::decay_t<decltype(node)>;

            if constexpr (std::is_same_v<NodeType, compiler::ColumnRef>) {
                if (node.column_id >= table.columns.size() || node.column_id >= row.size()) {
                    return make_error("expression column id is out of range at runtime");
                }
                return row[node.column_id];
            } else if constexpr (std::is_same_v<NodeType, compiler::Literal>) {
                return node.value;
            } else if constexpr (std::is_same_v<NodeType, compiler::Binary>) {
                if (!node.lhs || !node.rhs) {
                    return make_error("binary expression has a null child");
                }

                // Both operands are evaluated deliberately. Expressions have no side effects;
                // eager evaluation gives malformed runtime data a deterministic failure path.
                EvaluationResult lhs = evaluate_impl(*node.lhs, table, row, depth + 1U);
                if (const Error* error = std::get_if<Error>(&lhs)) {
                    return *error;
                }
                EvaluationResult rhs = evaluate_impl(*node.rhs, table, row, depth + 1U);
                if (const Error* error = std::get_if<Error>(&rhs)) {
                    return *error;
                }

                return std::visit(
                    [&lhs, &rhs](const auto& op) -> EvaluationResult {
                        using OpType = std::decay_t<decltype(op)>;
                        if constexpr (std::is_same_v<OpType, compiler::CmpOp>) {
                            if (!is_supported_comparison(op)) {
                                return make_error("unknown comparison operator");
                            }
                            const EvaluatedValue* lhs_evaluated =
                                std::get_if<EvaluatedValue>(&lhs);
                            const EvaluatedValue* rhs_evaluated =
                                std::get_if<EvaluatedValue>(&rhs);
                            const tinydbms::Value* lhs_value = lhs_evaluated == nullptr
                                ? nullptr
                                : std::get_if<tinydbms::Value>(lhs_evaluated);
                            const tinydbms::Value* rhs_value = rhs_evaluated == nullptr
                                ? nullptr
                                : std::get_if<tinydbms::Value>(rhs_evaluated);
                            if (lhs_value == nullptr || rhs_value == nullptr) {
                                return make_error("comparison operands must be values");
                            }
                            if (lhs_value->data.index() != rhs_value->data.index()) {
                                return make_error("comparison operands have different value types");
                            }
                            if (const auto* lhs_int = std::get_if<std::int32_t>(&lhs_value->data)) {
                                return compare_ints(
                                    *lhs_int,
                                    std::get<std::int32_t>(rhs_value->data),
                                    op);
                            }
                            return compare_strings(
                                std::get<std::string>(lhs_value->data),
                                std::get<std::string>(rhs_value->data),
                                op);
                        } else {
                            const EvaluatedValue* lhs_evaluated =
                                std::get_if<EvaluatedValue>(&lhs);
                            const EvaluatedValue* rhs_evaluated =
                                std::get_if<EvaluatedValue>(&rhs);
                            const bool* lhs_bool = lhs_evaluated == nullptr
                                ? nullptr
                                : std::get_if<bool>(lhs_evaluated);
                            const bool* rhs_bool = rhs_evaluated == nullptr
                                ? nullptr
                                : std::get_if<bool>(rhs_evaluated);
                            if (lhs_bool == nullptr || rhs_bool == nullptr) {
                                return make_error("logical operands must be BOOL");
                            }
                            if (!is_supported_logic(op)) {
                                return make_error("unknown logical operator");
                            }
                            return op == compiler::LogicOp::kAnd
                                ? (*lhs_bool && *rhs_bool)
                                : (*lhs_bool || *rhs_bool);
                        }
                    },
                    node.op);
            } else {
                if (node.op != compiler::UnaryOp::kNot) {
                    return make_error("unknown unary operator");
                }
                if (!node.operand) {
                    return make_error("unary expression has a null operand");
                }
                EvaluationResult operand = evaluate_impl(*node.operand, table, row, depth + 1U);
                if (const Error* error = std::get_if<Error>(&operand)) {
                    return *error;
                }
                const EvaluatedValue* evaluated = std::get_if<EvaluatedValue>(&operand);
                const bool* value = evaluated == nullptr
                    ? nullptr
                    : std::get_if<bool>(evaluated);
                if (value == nullptr) {
                    return make_error("NOT operand must be BOOL");
                }
                return !*value;
            }
        },
        expr.kind);
}

}  // namespace

std::optional<Error> validate_row(const TableMeta& table, const Row& row) {
    if (row.size() != table.columns.size()) {
        return Error{"record row width does not match table schema"};
    }
    for (std::size_t index = 0; index < row.size(); ++index) {
        if (!value_matches_column(row[index], table.columns[index])) {
            return Error{"record value type does not match table schema"};
        }
    }
    return std::nullopt;
}

ValidationResult validate(const compiler::Expr& expr, const TableMeta& table) {
    return validate_impl(expr, table, 0);
}

std::optional<Error> validate_predicate(
    const compiler::Expr& expr,
    const TableMeta& table) {
    ValidationResult result = validate_impl(expr, table, 0);
    if (const Error* error = std::get_if<Error>(&result)) {
        return *error;
    }
    if (std::get<ExprType>(result) != ExprType::kBool) {
        return Error{"predicate expression must evaluate to BOOL"};
    }
    return std::nullopt;
}

EvaluationResult evaluate(
    const compiler::Expr& expr,
    const TableMeta& table,
    const Row& row) {
    return evaluate_impl(expr, table, row, 0);
}

std::variant<bool, Error> evaluate_predicate(
    const compiler::Expr& expr,
    const TableMeta& table,
    const Row& row) {
    EvaluationResult result = evaluate_impl(expr, table, row, 0);
    if (const Error* error = std::get_if<Error>(&result)) {
        return *error;
    }
    const EvaluatedValue* evaluated = std::get_if<EvaluatedValue>(&result);
    const bool* predicate = evaluated == nullptr
        ? nullptr
        : std::get_if<bool>(evaluated);
    if (predicate == nullptr) {
        return Error{"predicate expression did not evaluate to BOOL"};
    }
    return *predicate;
}

}  // namespace tinydbms::core::internal::expression
