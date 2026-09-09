#ifndef TINYDBMS_COMPILER_BOUND_AST_HPP
#define TINYDBMS_COMPILER_BOUND_AST_HPP

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "tinydbms/compiler.hpp"

namespace tinydbms::compiler::internal {

struct BoundColumnRef {
    ColumnId column_id;
};

struct BoundLiteral {
    Value value;
};

struct BoundExpr;
using BoundExprPtr = std::unique_ptr<BoundExpr>;

struct BoundBinaryExpr {
    BoundBinaryExpr(CmpOp op, BoundExprPtr lhs, BoundExprPtr rhs);
    BoundBinaryExpr(LogicOp op, BoundExprPtr lhs, BoundExprPtr rhs);
    ~BoundBinaryExpr();
    BoundBinaryExpr(BoundBinaryExpr&&) noexcept;
    BoundBinaryExpr& operator=(BoundBinaryExpr&&) noexcept;
    BoundBinaryExpr(const BoundBinaryExpr&) = delete;
    BoundBinaryExpr& operator=(const BoundBinaryExpr&) = delete;

    std::variant<CmpOp, LogicOp> op;
    BoundExprPtr lhs;
    BoundExprPtr rhs;
};

struct BoundUnaryExpr {
    explicit BoundUnaryExpr(BoundExprPtr operand);
    ~BoundUnaryExpr();
    BoundUnaryExpr(BoundUnaryExpr&&) noexcept;
    BoundUnaryExpr& operator=(BoundUnaryExpr&&) noexcept;
    BoundUnaryExpr(const BoundUnaryExpr&) = delete;
    BoundUnaryExpr& operator=(const BoundUnaryExpr&) = delete;

    UnaryOp op{UnaryOp::kNot};
    BoundExprPtr operand;
};

struct BoundExpr {
    BoundExpr(BoundColumnRef column);
    BoundExpr(BoundLiteral literal);
    BoundExpr(BoundBinaryExpr binary);
    BoundExpr(BoundUnaryExpr unary);
    ~BoundExpr();
    BoundExpr(BoundExpr&&) noexcept;
    BoundExpr& operator=(BoundExpr&&) noexcept;
    BoundExpr(const BoundExpr&) = delete;
    BoundExpr& operator=(const BoundExpr&) = delete;

    std::variant<BoundColumnRef, BoundLiteral, BoundBinaryExpr, BoundUnaryExpr> kind;
};

inline BoundBinaryExpr::BoundBinaryExpr(CmpOp op_, BoundExprPtr lhs_, BoundExprPtr rhs_)
    : op{op_}, lhs{std::move(lhs_)}, rhs{std::move(rhs_)} {}

inline BoundBinaryExpr::BoundBinaryExpr(LogicOp op_, BoundExprPtr lhs_, BoundExprPtr rhs_)
    : op{op_}, lhs{std::move(lhs_)}, rhs{std::move(rhs_)} {}

inline BoundBinaryExpr::~BoundBinaryExpr() = default;
inline BoundBinaryExpr::BoundBinaryExpr(BoundBinaryExpr&&) noexcept = default;
inline BoundBinaryExpr& BoundBinaryExpr::operator=(BoundBinaryExpr&&) noexcept = default;

inline BoundUnaryExpr::BoundUnaryExpr(BoundExprPtr operand_)
    : operand{std::move(operand_)} {}

inline BoundUnaryExpr::~BoundUnaryExpr() = default;
inline BoundUnaryExpr::BoundUnaryExpr(BoundUnaryExpr&&) noexcept = default;
inline BoundUnaryExpr& BoundUnaryExpr::operator=(BoundUnaryExpr&&) noexcept = default;

inline BoundExpr::BoundExpr(BoundColumnRef value) : kind{std::move(value)} {}
inline BoundExpr::BoundExpr(BoundLiteral value) : kind{std::move(value)} {}
inline BoundExpr::BoundExpr(BoundBinaryExpr value) : kind{std::move(value)} {}
inline BoundExpr::BoundExpr(BoundUnaryExpr value) : kind{std::move(value)} {}
inline BoundExpr::~BoundExpr() = default;
inline BoundExpr::BoundExpr(BoundExpr&&) noexcept = default;
inline BoundExpr& BoundExpr::operator=(BoundExpr&&) noexcept = default;

struct BoundCreateTable {
    std::string table_name;
    std::vector<ColumnMeta> columns;
};

struct BoundInsert {
    TableId table_id;
    std::vector<ColumnId> columns;
    std::vector<std::vector<Value>> rows;
};

struct BoundSelect {
    TableId table_id;
    std::vector<ColumnId> outputs;
    BoundExprPtr predicate;
};

struct BoundDelete {
    TableId table_id;
    BoundExprPtr predicate;
};

struct BoundStatement {
    std::variant<BoundCreateTable, BoundInsert, BoundSelect, BoundDelete> kind;
};

}  // namespace tinydbms::compiler::internal

#endif  // TINYDBMS_COMPILER_BOUND_AST_HPP
