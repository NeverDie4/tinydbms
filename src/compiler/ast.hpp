#ifndef TINYDBMS_COMPILER_AST_HPP
#define TINYDBMS_COMPILER_AST_HPP

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "tinydbms/common.hpp"

namespace tinydbms::compiler::internal {

struct AstColumnDef {
    std::string name;
    Type type;
    SourceLocation location;
};

struct CreateTableAst {
    std::string table_name;
    SourceLocation table_location;
    std::vector<AstColumnDef> columns;
};

struct AstInsertColumn {
    std::string name;
    SourceLocation location;
};

struct AstLiteral {
    Value value;
    SourceLocation location;
};

struct InsertAst {
    std::string table_name;
    SourceLocation table_location;
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

struct AstIdentifierExpr {
    std::string name;
    SourceLocation location;
};

struct AstLiteralExpr {
    Value value;
    SourceLocation location;
};

struct AstExpr;
using AstExprPtr = std::unique_ptr<AstExpr>;

struct AstBinaryExpr {
    AstBinaryExpr(AstCompareOp op, SourceLocation location, AstExprPtr lhs, AstExprPtr rhs);
    AstBinaryExpr(AstLogicOp op, SourceLocation location, AstExprPtr lhs, AstExprPtr rhs);
    ~AstBinaryExpr();
    AstBinaryExpr(AstBinaryExpr&&) noexcept;
    AstBinaryExpr& operator=(AstBinaryExpr&&) noexcept;
    AstBinaryExpr(const AstBinaryExpr&) = delete;
    AstBinaryExpr& operator=(const AstBinaryExpr&) = delete;

    std::variant<AstCompareOp, AstLogicOp> op;
    SourceLocation location;
    AstExprPtr lhs;
    AstExprPtr rhs;
};

struct AstUnaryExpr {
    AstUnaryExpr(SourceLocation location, AstExprPtr operand);
    ~AstUnaryExpr();
    AstUnaryExpr(AstUnaryExpr&&) noexcept;
    AstUnaryExpr& operator=(AstUnaryExpr&&) noexcept;
    AstUnaryExpr(const AstUnaryExpr&) = delete;
    AstUnaryExpr& operator=(const AstUnaryExpr&) = delete;

    AstUnaryOp op{AstUnaryOp::kNot};
    SourceLocation location;
    AstExprPtr operand;
};

struct AstExpr {
    AstExpr(AstIdentifierExpr identifier);
    AstExpr(AstLiteralExpr literal);
    AstExpr(AstBinaryExpr binary);
    AstExpr(AstUnaryExpr unary);
    ~AstExpr();
    AstExpr(AstExpr&&) noexcept;
    AstExpr& operator=(AstExpr&&) noexcept;
    AstExpr(const AstExpr&) = delete;
    AstExpr& operator=(const AstExpr&) = delete;

    std::variant<AstIdentifierExpr, AstLiteralExpr, AstBinaryExpr, AstUnaryExpr> kind;
};

inline AstBinaryExpr::AstBinaryExpr(
    AstCompareOp op_,
    SourceLocation location_,
    AstExprPtr lhs_,
    AstExprPtr rhs_)
    : op{op_}, location{location_}, lhs{std::move(lhs_)}, rhs{std::move(rhs_)} {}

inline AstBinaryExpr::AstBinaryExpr(
    AstLogicOp op_,
    SourceLocation location_,
    AstExprPtr lhs_,
    AstExprPtr rhs_)
    : op{op_}, location{location_}, lhs{std::move(lhs_)}, rhs{std::move(rhs_)} {}

inline AstBinaryExpr::~AstBinaryExpr() = default;
inline AstBinaryExpr::AstBinaryExpr(AstBinaryExpr&&) noexcept = default;
inline AstBinaryExpr& AstBinaryExpr::operator=(AstBinaryExpr&&) noexcept = default;

inline AstUnaryExpr::AstUnaryExpr(SourceLocation location_, AstExprPtr operand_)
    : location{location_}, operand{std::move(operand_)} {}

inline AstUnaryExpr::~AstUnaryExpr() = default;
inline AstUnaryExpr::AstUnaryExpr(AstUnaryExpr&&) noexcept = default;
inline AstUnaryExpr& AstUnaryExpr::operator=(AstUnaryExpr&&) noexcept = default;

inline AstExpr::AstExpr(AstIdentifierExpr value) : kind{std::move(value)} {}
inline AstExpr::AstExpr(AstLiteralExpr value) : kind{std::move(value)} {}
inline AstExpr::AstExpr(AstBinaryExpr value) : kind{std::move(value)} {}
inline AstExpr::AstExpr(AstUnaryExpr value) : kind{std::move(value)} {}
inline AstExpr::~AstExpr() = default;
inline AstExpr::AstExpr(AstExpr&&) noexcept = default;
inline AstExpr& AstExpr::operator=(AstExpr&&) noexcept = default;

struct AstSelectColumn {
    std::string name;
    SourceLocation location;
};

struct SelectAst {
    std::string table_name;
    SourceLocation table_location;
    bool select_all;
    std::vector<AstSelectColumn> columns;
    AstExprPtr predicate;
};

struct DeleteAst {
    std::string table_name;
    SourceLocation table_location;
    AstExprPtr predicate;
};

struct StatementAst {
    std::variant<CreateTableAst, InsertAst, SelectAst, DeleteAst> kind;
};

}  // namespace tinydbms::compiler::internal

#endif  // TINYDBMS_COMPILER_AST_HPP
