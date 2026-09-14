#include "expression.hpp"

#include <cstdint>
#include <type_traits>
#include <utility>

namespace tinydbms::core::internal::expression {
namespace {

Error make_error(std::string message) {
    return Error{std::move(message)};
}

ValidationResult type_of_value(const tinydbms::Value& value) {
    if (std::holds_alternative<std::int32_t>(value.data)) {
        return ExprType::kInt;
    }
    if (std::holds_alternative<std::int64_t>(value.data)) {
        return ExprType::kBigInt;
    }
    if (std::holds_alternative<double>(value.data)) {
        return ExprType::kDouble;
    }
    if (std::holds_alternative<std::string>(value.data)) {
        return ExprType::kVarchar;
    }
    if (std::holds_alternative<bool>(value.data)) {
        return ExprType::kBool;
    }
    if (std::holds_alternative<std::monostate>(value.data)) {
        return ExprType::kNull;
    }
    return make_error("expression Value type is not supported by the runtime");
}

std::variant<const tinydbms::Value*, Error> lookup_slot(
    const SlotRow& row,
    SlotId slot_id) {
    const tinydbms::Value* result = nullptr;
    for (const SlotValue& entry : row) {
        if (entry.slot_id != slot_id) {
            continue;
        }
        if (result != nullptr) {
            return make_error(
                "expression slot s" + std::to_string(slot_id) +
                " is bound more than once");
        }
        result = &entry.value;
    }
    if (result == nullptr) {
        return make_error(
            "expression slot s" + std::to_string(slot_id) +
            " is missing or unbound");
    }
    return result;
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

bool is_numeric_type(ExprType type) {
    return type == ExprType::kInt || type == ExprType::kBigInt ||
        type == ExprType::kDouble;
}

std::optional<double> numeric_double_value(const tinydbms::Value& value) {
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

std::optional<std::int64_t> integer_value(const tinydbms::Value& value) {
    if (const auto* integer = std::get_if<std::int32_t>(&value.data)) {
        return static_cast<std::int64_t>(*integer);
    }
    if (const auto* bigint = std::get_if<std::int64_t>(&value.data)) {
        return *bigint;
    }
    return std::nullopt;
}

bool compare_ints(std::int64_t lhs, std::int64_t rhs, compiler::CmpOp op) {
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

bool compare_doubles(double lhs, double rhs, compiler::CmpOp op) {
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

bool compare_booleans(bool lhs, bool rhs, compiler::CmpOp op) {
    if (op == compiler::CmpOp::kEq) {
        return lhs == rhs;
    }
    return lhs != rhs;
}

ValidationResult validate_impl(
    const compiler::Expr& expr,
    const SlotRow& row,
    std::size_t depth) {
    if (depth >= kMaxExpressionDepth) {
        return make_error("expression depth exceeds core limit");
    }
    return std::visit(
        [&row, depth](const auto& node) -> ValidationResult {
            using NodeType = std::decay_t<decltype(node)>;

            if constexpr (std::is_same_v<NodeType, compiler::ColumnRef>) {
                const auto lookup = lookup_slot(row, node.slot_id);
                if (const Error* error = std::get_if<Error>(&lookup)) {
                    return *error;
                }
                return type_of_value(**std::get_if<const tinydbms::Value*>(&lookup));
            } else if constexpr (std::is_same_v<NodeType, compiler::Literal>) {
                return type_of_value(node.value);
            } else if constexpr (std::is_same_v<NodeType, compiler::Binary>) {
                if (!node.lhs || !node.rhs) {
                    return make_error("binary expression has a null child");
                }

                ValidationResult lhs = validate_impl(*node.lhs, row, depth + 1U);
                if (const Error* error = std::get_if<Error>(&lhs)) {
                    return *error;
                }
                ValidationResult rhs = validate_impl(*node.rhs, row, depth + 1U);
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
                            if (lhs_type == ExprType::kNull || rhs_type == ExprType::kNull) {
                                const ExprType concrete_type = lhs_type == ExprType::kNull
                                    ? rhs_type : lhs_type;
                                if (is_ordered_comparison(op) &&
                                    (concrete_type == ExprType::kVarchar ||
                                     concrete_type == ExprType::kBool)) {
                                    return make_error(
                                        concrete_type == ExprType::kVarchar
                                            ? "VARCHAR only supports equality comparisons"
                                            : "BOOLEAN only supports equality comparisons");
                                }
                                return ExprType::kBool;
                            }
                            const bool numeric_comparison =
                                is_numeric_type(lhs_type) && is_numeric_type(rhs_type);
                            const bool same_equality_type =
                                lhs_type == rhs_type &&
                                (lhs_type == ExprType::kVarchar ||
                                 lhs_type == ExprType::kBool);
                            if (!numeric_comparison && !same_equality_type) {
                                return make_error("comparison operands must have the same value type");
                            }
                            if (lhs_type == ExprType::kVarchar && is_ordered_comparison(op)) {
                                return make_error("VARCHAR only supports equality comparisons");
                            }
                            if (lhs_type == ExprType::kBool && is_ordered_comparison(op)) {
                                return make_error("BOOLEAN only supports equality comparisons");
                            }
                            return ExprType::kBool;
                        } else {
                            if (!is_supported_logic(op)) {
                                return make_error("unknown logical operator");
                            }
                            const bool lhs_truth = lhs_type == ExprType::kBool ||
                                lhs_type == ExprType::kNull;
                            const bool rhs_truth = rhs_type == ExprType::kBool ||
                                rhs_type == ExprType::kNull;
                            if (!lhs_truth || !rhs_truth) {
                                return make_error("logical operands must be BOOL");
                            }
                            return ExprType::kBool;
                        }
                    },
                    node.op);
            } else if constexpr (std::is_same_v<NodeType, compiler::NullTest>) {
                if (!node.operand) {
                    return make_error("NULL test expression has a null operand");
                }
                ValidationResult operand = validate_impl(*node.operand, row, depth + 1U);
                if (const Error* error = std::get_if<Error>(&operand)) {
                    return *error;
                }
                return ExprType::kBool;
            } else {
                if (node.op != compiler::UnaryOp::kNot) {
                    return make_error("unknown unary operator");
                }
                if (!node.operand) {
                    return make_error("unary expression has a null operand");
                }
                ValidationResult operand = validate_impl(*node.operand, row, depth + 1U);
                if (const Error* error = std::get_if<Error>(&operand)) {
                    return *error;
                }
                const ExprType operand_type = std::get<ExprType>(operand);
                if (operand_type != ExprType::kBool && operand_type != ExprType::kNull) {
                    return make_error("NOT operand must be BOOL");
                }
                return ExprType::kBool;
            }
        },
        expr.kind);
}

EvaluationResult evaluate_impl(
    const compiler::Expr& expr,
    const SlotRow& row,
    std::size_t depth) {
    if (depth >= kMaxExpressionDepth) {
        return make_error("expression depth exceeds core limit");
    }
    return std::visit(
        [&row, depth](const auto& node) -> EvaluationResult {
            using NodeType = std::decay_t<decltype(node)>;

            if constexpr (std::is_same_v<NodeType, compiler::ColumnRef>) {
                const auto lookup = lookup_slot(row, node.slot_id);
                if (const Error* error = std::get_if<Error>(&lookup)) {
                    return *error;
                }
                return **std::get_if<const tinydbms::Value*>(&lookup);
            } else if constexpr (std::is_same_v<NodeType, compiler::Literal>) {
                return node.value;
            } else if constexpr (std::is_same_v<NodeType, compiler::Binary>) {
                if (!node.lhs || !node.rhs) {
                    return make_error("binary expression has a null child");
                }

                // Both operands are evaluated deliberately. Expressions have no side effects;
                // eager evaluation gives malformed runtime data a deterministic failure path.
                EvaluationResult lhs = evaluate_impl(*node.lhs, row, depth + 1U);
                if (const Error* error = std::get_if<Error>(&lhs)) {
                    return *error;
                }
                EvaluationResult rhs = evaluate_impl(*node.rhs, row, depth + 1U);
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
                            const tinydbms::Value* lhs_value =
                                std::get_if<tinydbms::Value>(&lhs);
                            const tinydbms::Value* rhs_value =
                                std::get_if<tinydbms::Value>(&rhs);
                            if (lhs_value == nullptr || rhs_value == nullptr) {
                                return make_error("comparison operands must be values");
                            }
                            const bool lhs_null =
                                std::holds_alternative<std::monostate>(lhs_value->data);
                            const bool rhs_null =
                                std::holds_alternative<std::monostate>(rhs_value->data);
                            if (lhs_null || rhs_null) {
                                const tinydbms::Value& concrete = lhs_null ? *rhs_value : *lhs_value;
                                if (is_ordered_comparison(op) &&
                                    (std::holds_alternative<std::string>(concrete.data) ||
                                     std::holds_alternative<bool>(concrete.data))) {
                                    return make_error(
                                        std::holds_alternative<std::string>(concrete.data)
                                            ? "VARCHAR only supports equality comparisons"
                                            : "BOOLEAN only supports equality comparisons");
                                }
                                return tinydbms::Value{std::monostate{}};
                            }
                            if (const auto* lhs_boolean =
                                    std::get_if<bool>(&lhs_value->data)) {
                                const auto* rhs_boolean =
                                    std::get_if<bool>(&rhs_value->data);
                                if (rhs_boolean == nullptr) {
                                    return make_error(
                                        "comparison operands have different value types");
                                }
                                if (is_ordered_comparison(op)) {
                                    return make_error(
                                        "BOOLEAN only supports equality comparisons");
                                }
                                return tinydbms::Value{compare_booleans(
                                    *lhs_boolean, *rhs_boolean, op)};
                            }
                            if (std::holds_alternative<bool>(rhs_value->data)) {
                                return make_error(
                                    "comparison operands have different value types");
                            }
                            if (std::holds_alternative<double>(lhs_value->data) ||
                                std::holds_alternative<double>(rhs_value->data)) {
                                const std::optional<double> lhs_number =
                                    numeric_double_value(*lhs_value);
                                const std::optional<double> rhs_number =
                                    numeric_double_value(*rhs_value);
                                if (lhs_number.has_value() && rhs_number.has_value()) {
                                    return tinydbms::Value{compare_doubles(
                                        *lhs_number, *rhs_number, op)};
                                }
                                return make_error(
                                    "comparison operands have different value types");
                            }
                            const std::optional<std::int64_t> lhs_integer =
                                integer_value(*lhs_value);
                            const std::optional<std::int64_t> rhs_integer =
                                integer_value(*rhs_value);
                            if (lhs_integer.has_value() && rhs_integer.has_value()) {
                                return tinydbms::Value{compare_ints(
                                    *lhs_integer,*rhs_integer,op)};
                            }
                            if (lhs_integer.has_value() != rhs_integer.has_value()) {
                                return make_error("comparison operands have different value types");
                            }
                            if (const auto* lhs_string =
                                    std::get_if<std::string>(&lhs_value->data)) {
                                const auto* rhs_string =
                                    std::get_if<std::string>(&rhs_value->data);
                                if (rhs_string == nullptr) {
                                    return make_error(
                                        "comparison operands have different value types");
                                }
                                return tinydbms::Value{compare_strings(
                                    *lhs_string,*rhs_string,op)};
                            }
                            return make_error(
                                "comparison Value type is not supported by the runtime");
                        } else {
                            if (!is_supported_logic(op)) {
                                return make_error("unknown logical operator");
                            }
                            const tinydbms::Value* lhs_value =
                                std::get_if<tinydbms::Value>(&lhs);
                            const tinydbms::Value* rhs_value =
                                std::get_if<tinydbms::Value>(&rhs);
                            if (lhs_value == nullptr || rhs_value == nullptr) {
                                return make_error("logical operands must be values");
                            }
                            const TruthValueResult lhs_truth = to_truth_value(*lhs_value);
                            const TruthValueResult rhs_truth = to_truth_value(*rhs_value);
                            if (const Error* error = std::get_if<Error>(&lhs_truth)) {
                                return *error;
                            }
                            if (const Error* error = std::get_if<Error>(&rhs_truth)) {
                                return *error;
                            }
                            const TruthValue result = op == compiler::LogicOp::kAnd
                                ? truth_and(
                                      std::get<TruthValue>(lhs_truth),
                                      std::get<TruthValue>(rhs_truth))
                                : truth_or(
                                      std::get<TruthValue>(lhs_truth),
                                      std::get<TruthValue>(rhs_truth));
                            return truth_value_to_value(result);
                        }
                    },
                    node.op);
            } else if constexpr (std::is_same_v<NodeType, compiler::NullTest>) {
                if (!node.operand) {
                    return make_error("NULL test expression has a null operand");
                }
                EvaluationResult operand = evaluate_impl(*node.operand, row, depth + 1U);
                if (const Error* error = std::get_if<Error>(&operand)) {
                    return *error;
                }
                const tinydbms::Value* value = std::get_if<tinydbms::Value>(&operand);
                if (value == nullptr) {
                    return make_error("NULL test operand must be a value");
                }
                const bool is_null = std::holds_alternative<std::monostate>(value->data);
                return tinydbms::Value{
                    node.op == compiler::NullTestOp::kIsNull ? is_null : !is_null};
            } else {
                if (node.op != compiler::UnaryOp::kNot) {
                    return make_error("unknown unary operator");
                }
                if (!node.operand) {
                    return make_error("unary expression has a null operand");
                }
                EvaluationResult operand = evaluate_impl(*node.operand, row, depth + 1U);
                if (const Error* error = std::get_if<Error>(&operand)) {
                    return *error;
                }
                const tinydbms::Value* value = std::get_if<tinydbms::Value>(&operand);
                if (value == nullptr) {
                    return make_error("NOT operand must be a value");
                }
                const TruthValueResult truth = to_truth_value(*value);
                if (const Error* error = std::get_if<Error>(&truth)) {
                    return *error;
                }
                return truth_value_to_value(truth_not(std::get<TruthValue>(truth)));
            }
        },
        expr.kind);
}

}  // namespace

TruthValueResult to_truth_value(const tinydbms::Value& value) {
    if (const auto* boolean = std::get_if<bool>(&value.data)) {
        return *boolean ? TruthValue::kTrue : TruthValue::kFalse;
    }
    if (std::holds_alternative<std::monostate>(value.data)) {
        return TruthValue::kUnknown;
    }
    return Error{"predicate value must be BOOLEAN or NULL"};
}

tinydbms::Value truth_value_to_value(TruthValue value) {
    switch (value) {
        case TruthValue::kFalse:
            return tinydbms::Value{false};
        case TruthValue::kTrue:
            return tinydbms::Value{true};
        case TruthValue::kUnknown:
            return tinydbms::Value{std::monostate{}};
    }
    return tinydbms::Value{std::monostate{}};
}

TruthValue truth_not(TruthValue value) noexcept {
    switch (value) {
        case TruthValue::kFalse:
            return TruthValue::kTrue;
        case TruthValue::kTrue:
            return TruthValue::kFalse;
        case TruthValue::kUnknown:
            return TruthValue::kUnknown;
    }
    return TruthValue::kUnknown;
}

TruthValue truth_and(TruthValue lhs, TruthValue rhs) noexcept {
    if (lhs == TruthValue::kFalse || rhs == TruthValue::kFalse) {
        return TruthValue::kFalse;
    }
    if (lhs == TruthValue::kUnknown || rhs == TruthValue::kUnknown) {
        return TruthValue::kUnknown;
    }
    return TruthValue::kTrue;
}

TruthValue truth_or(TruthValue lhs, TruthValue rhs) noexcept {
    if (lhs == TruthValue::kTrue || rhs == TruthValue::kTrue) {
        return TruthValue::kTrue;
    }
    if (lhs == TruthValue::kUnknown || rhs == TruthValue::kUnknown) {
        return TruthValue::kUnknown;
    }
    return TruthValue::kFalse;
}

ValidationResult validate(const compiler::Expr& expr, const SlotRow& row) {
    return validate_impl(expr, row, 0);
}

std::optional<Error> validate_predicate(
    const compiler::Expr& expr,
    const SlotRow& row) {
    ValidationResult result = validate_impl(expr, row, 0);
    if (const Error* error = std::get_if<Error>(&result)) {
        return *error;
    }
    const ExprType type = std::get<ExprType>(result);
    if (type != ExprType::kBool && type != ExprType::kNull) {
        return Error{"predicate expression must evaluate to BOOL"};
    }
    return std::nullopt;
}

EvaluationResult evaluate(
    const compiler::Expr& expr,
    const SlotRow& row) {
    return evaluate_impl(expr, row, 0);
}

std::variant<bool, Error> evaluate_predicate(
    const compiler::Expr& expr,
    const SlotRow& row) {
    EvaluationResult result = evaluate_impl(expr, row, 0);
    if (const Error* error = std::get_if<Error>(&result)) {
        return *error;
    }
    const tinydbms::Value* value = std::get_if<tinydbms::Value>(&result);
    if (value == nullptr) {
        return Error{"predicate expression did not evaluate to a value"};
    }
    const TruthValueResult truth = to_truth_value(*value);
    if (const Error* error = std::get_if<Error>(&truth)) {
        return *error;
    }
    return std::get<TruthValue>(truth) == TruthValue::kTrue;
}

}  // namespace tinydbms::core::internal::expression
