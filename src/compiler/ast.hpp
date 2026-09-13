#ifndef TINYDBMS_COMPILER_AST_HPP
#define TINYDBMS_COMPILER_AST_HPP

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "tinydbms/common.hpp"
#include "tinydbms/diagnostic.hpp"

namespace tinydbms::compiler::internal {

struct AstColumnDef {
    std::string name;
    Type type;
    bool nullable;
    SourceRange source;
};

struct CreateTableAst {
    std::string table_name;
    SourceRange table_source;
    std::vector<AstColumnDef> columns;
};

struct AstInsertColumn {
    std::string name;
    SourceRange source;
};

struct AstLiteral {
    Value value;
    SourceRange source;
};

struct InsertAst {
    std::string table_name;
    SourceRange table_source;
    std::vector<AstInsertColumn> columns;
    std::vector<std::vector<AstLiteral>> rows;
};

enum class AstCompareOp {
    kEq,
    kNe,
    kLt,
    kLe,
    kGt,
    kGe
};

enum class AstLogicOp {
    kAnd,
    kOr
};

enum class AstUnaryOp {
    kNot
};

enum class AstNullTestOp {
    kIsNull,
    kIsNotNull
};

struct AstIdentifierExpr {
    std::optional<std::string> qualifier;
    std::string name;
    SourceRange source;
};

struct AstLiteralExpr {
    Value value;
    SourceRange source;
};

struct AstExpr;
using AstExprPtr = std::unique_ptr<AstExpr>;

struct AstBinaryExpr {
    AstBinaryExpr(AstCompareOp op, SourceRange source, AstExprPtr lhs, AstExprPtr rhs);
    AstBinaryExpr(AstLogicOp op, SourceRange source, AstExprPtr lhs, AstExprPtr rhs);
    ~AstBinaryExpr();
    AstBinaryExpr(AstBinaryExpr&&) noexcept;
    AstBinaryExpr& operator=(AstBinaryExpr&&) noexcept;
    AstBinaryExpr(const AstBinaryExpr&) = delete;
    AstBinaryExpr& operator=(const AstBinaryExpr&) = delete;

    std::variant<AstCompareOp, AstLogicOp> op;
    SourceRange source;
    AstExprPtr lhs;
    AstExprPtr rhs;
};

struct AstUnaryExpr {
    AstUnaryExpr(SourceRange source, AstExprPtr operand);
    ~AstUnaryExpr();
    AstUnaryExpr(AstUnaryExpr&&) noexcept;
    AstUnaryExpr& operator=(AstUnaryExpr&&) noexcept;
    AstUnaryExpr(const AstUnaryExpr&) = delete;
    AstUnaryExpr& operator=(const AstUnaryExpr&) = delete;

    AstUnaryOp op{AstUnaryOp::kNot};
    SourceRange source;
    AstExprPtr operand;
};

struct AstNullTestExpr {
    AstNullTestExpr(AstNullTestOp op, SourceRange source, AstExprPtr operand);
    ~AstNullTestExpr();
    AstNullTestExpr(AstNullTestExpr&&) noexcept;
    AstNullTestExpr& operator=(AstNullTestExpr&&) noexcept;
    AstNullTestExpr(const AstNullTestExpr&) = delete;
    AstNullTestExpr& operator=(const AstNullTestExpr&) = delete;

    AstNullTestOp op;
    SourceRange source;
    AstExprPtr operand;
};

struct AstExpr {
    AstExpr(AstIdentifierExpr identifier);
    AstExpr(AstLiteralExpr literal);
    AstExpr(AstBinaryExpr binary);
    AstExpr(AstUnaryExpr unary);
    AstExpr(AstNullTestExpr null_test);
    ~AstExpr();
    AstExpr(AstExpr&&) noexcept;
    AstExpr& operator=(AstExpr&&) noexcept;
    AstExpr(const AstExpr&) = delete;
    AstExpr& operator=(const AstExpr&) = delete;

    std::variant<
        AstIdentifierExpr,
        AstLiteralExpr,
        AstBinaryExpr,
        AstUnaryExpr,
        AstNullTestExpr> kind;
};

inline AstBinaryExpr::AstBinaryExpr(
    AstCompareOp op_,
    SourceRange source_,
    AstExprPtr lhs_,
    AstExprPtr rhs_)
    : op{op_}, source{source_}, lhs{std::move(lhs_)}, rhs{std::move(rhs_)} {}

inline AstBinaryExpr::AstBinaryExpr(
    AstLogicOp op_,
    SourceRange source_,
    AstExprPtr lhs_,
    AstExprPtr rhs_)
    : op{op_}, source{source_}, lhs{std::move(lhs_)}, rhs{std::move(rhs_)} {}

inline AstBinaryExpr::~AstBinaryExpr() = default;
inline AstBinaryExpr::AstBinaryExpr(AstBinaryExpr&&) noexcept = default;
inline AstBinaryExpr& AstBinaryExpr::operator=(AstBinaryExpr&&) noexcept = default;

inline AstUnaryExpr::AstUnaryExpr(SourceRange source_, AstExprPtr operand_)
    : source{source_}, operand{std::move(operand_)} {}

inline AstUnaryExpr::~AstUnaryExpr() = default;
inline AstUnaryExpr::AstUnaryExpr(AstUnaryExpr&&) noexcept = default;
inline AstUnaryExpr& AstUnaryExpr::operator=(AstUnaryExpr&&) noexcept = default;

inline AstNullTestExpr::AstNullTestExpr(
    AstNullTestOp op_, SourceRange source_, AstExprPtr operand_)
    : op{op_}, source{source_}, operand{std::move(operand_)} {}
inline AstNullTestExpr::~AstNullTestExpr() = default;
inline AstNullTestExpr::AstNullTestExpr(AstNullTestExpr&&) noexcept = default;
inline AstNullTestExpr& AstNullTestExpr::operator=(AstNullTestExpr&&) noexcept = default;

inline AstExpr::AstExpr(AstIdentifierExpr value) : kind{std::move(value)} {}
inline AstExpr::AstExpr(AstLiteralExpr value) : kind{std::move(value)} {}
inline AstExpr::AstExpr(AstBinaryExpr value) : kind{std::move(value)} {}
inline AstExpr::AstExpr(AstUnaryExpr value) : kind{std::move(value)} {}
inline AstExpr::AstExpr(AstNullTestExpr value) : kind{std::move(value)} {}
inline AstExpr::~AstExpr() = default;
inline AstExpr::AstExpr(AstExpr&&) noexcept = default;
inline AstExpr& AstExpr::operator=(AstExpr&&) noexcept = default;

struct AstSelectColumn {
    std::optional<std::string> qualifier;
    std::string name;
    SourceRange source;
};

enum class AstAggregateKind {
    kCount,
    kSum,
    kAvg,
    kMin,
    kMax
};

struct AstAggregateCall {
    AstAggregateKind kind;
    std::optional<AstSelectColumn> argument;
    SourceRange source;
};

using AstSelectItem = std::variant<AstSelectColumn, AstAggregateCall>;

enum class AstSortDirection {
    kAsc,
    kDesc
};

struct AstSortKey {
    std::optional<std::string> qualifier;
    std::string column_name;
    SourceRange source;
    AstSortDirection direction;
};

struct AstJoin {
    std::string table_name;
    SourceRange table_source;
    AstExprPtr condition;
};

struct SelectAst {
    std::string table_name;
    SourceRange table_source;
    std::vector<AstJoin> joins;
    bool select_all;
    std::vector<AstSelectItem> items;
    AstExprPtr predicate;
    std::vector<AstSelectColumn> group_by;
    std::vector<AstSortKey> order_by;
};

struct DeleteAst {
    std::string table_name;
    SourceRange table_source;
    AstExprPtr predicate;
};

struct AstUpdateAssignment {
    std::string column_name;
    SourceRange column_source;
    AstLiteral literal;
};

struct UpdateAst {
    std::string table_name;
    SourceRange table_source;
    std::vector<AstUpdateAssignment> assignments;
    AstExprPtr predicate;
};

struct StatementAst {
    std::variant<CreateTableAst, InsertAst, SelectAst, DeleteAst, UpdateAst> kind;
};

}  // namespace tinydbms::compiler::internal

#endif  // TINYDBMS_COMPILER_AST_HPP
