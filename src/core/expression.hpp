#ifndef TINYDBMS_CORE_EXPRESSION_HPP
#define TINYDBMS_CORE_EXPRESSION_HPP

#include "tinydbms/compiler.hpp"
#include "tinydbms/core.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>

namespace tinydbms::core::internal::expression {

inline constexpr std::size_t kMaxExpressionDepth = 256;

enum class ExprType {
    kInt,
    kVarchar,
    kBool
};

struct Error {
    std::string message;
};

using EvaluatedValue = std::variant<tinydbms::Value, bool>;
using ValidationResult = std::variant<ExprType, Error>;
using EvaluationResult = std::variant<EvaluatedValue, Error>;

// 返回空表示行满足 Catalog 定义；非空表示 core/storage 契约被破坏。
std::optional<Error> validate_row(const TableMeta& table, const Row& row);

// 只检查表达式结构、列引用和静态类型，不读取 storage，也不产生副作用。
ValidationResult validate(const compiler::Expr& expr, const TableMeta& table);
std::optional<Error> validate_predicate(
    const compiler::Expr& expr,
    const TableMeta& table);

// 调用方必须先完成 validate_row 和 validate_predicate。
EvaluationResult evaluate(
    const compiler::Expr& expr,
    const TableMeta& table,
    const Row& row);
std::variant<bool, Error> evaluate_predicate(
    const compiler::Expr& expr,
    const TableMeta& table,
    const Row& row);

}  // namespace tinydbms::core::internal::expression

#endif  // TINYDBMS_CORE_EXPRESSION_HPP
