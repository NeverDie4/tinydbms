#ifndef TINYDBMS_CORE_EXPRESSION_HPP
#define TINYDBMS_CORE_EXPRESSION_HPP

#include "tinydbms/compiler.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace tinydbms::core::internal::expression {

inline constexpr std::size_t kMaxExpressionDepth = 256;

enum class ExprType {
    kInt,
    kBigInt,
    kDouble,
    kVarchar,
    kBool,
    kNull
};

struct Error {
    std::string message;
};

struct SlotValue {
    SlotId slot_id;
    Value value;
};

using SlotRow = std::vector<SlotValue>;

enum class TruthValue {
    kFalse,
    kTrue,
    kUnknown
};

using TruthValueResult = std::variant<TruthValue, Error>;
using ValidationResult = std::variant<ExprType, Error>;
using EvaluationResult = std::variant<tinydbms::Value, Error>;

TruthValueResult to_truth_value(const tinydbms::Value& value);
tinydbms::Value truth_value_to_value(TruthValue value);
TruthValue truth_not(TruthValue value) noexcept;
TruthValue truth_and(TruthValue lhs, TruthValue rhs) noexcept;
TruthValue truth_or(TruthValue lhs, TruthValue rhs) noexcept;

// 只检查表达式结构、slot 引用和运行时值类型，不读取 storage，也不产生副作用。
ValidationResult validate(const compiler::Expr& expr, const SlotRow& row);
std::optional<Error> validate_predicate(
    const compiler::Expr& expr,
    const SlotRow& row);

EvaluationResult evaluate(
    const compiler::Expr& expr,
    const SlotRow& row);
std::variant<bool, Error> evaluate_predicate(
    const compiler::Expr& expr,
    const SlotRow& row);

}  // namespace tinydbms::core::internal::expression

#endif  // TINYDBMS_CORE_EXPRESSION_HPP
