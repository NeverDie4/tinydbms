#pragma once

#include "tinydbms/types.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tinydbms::compiler {

struct SplitStatement {
    std::string sql;
    SourceLocation start;
};

std::vector<SplitStatement> split_statements(std::string_view script);

struct CatalogView {
    std::span<const TableMeta> tables;
};

struct ColumnRef {
    ColumnId column_id = 0;
};

struct Literal {
    Value value;
};

enum class CmpOp {
    kEq,
    kNe,
    kLt,
    kLe,
    kGt,
    kGe,
};

enum class LogicOp {
    kAnd,
    kOr,
};

enum class UnaryOp {
    kNot,
};

struct Expr;

struct BinaryExpr {
    std::variant<CmpOp, LogicOp> op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
};

struct UnaryExpr {
    UnaryOp op = UnaryOp::kNot;
    std::unique_ptr<Expr> child;
};

struct Expr {
    std::variant<ColumnRef, Literal, BinaryExpr, UnaryExpr> value;
};

struct CreateTablePlan {
    std::string table_name;
    std::vector<ColumnMeta> columns;
};

struct InsertPlan {
    TableId table_id = 0;
    std::vector<ColumnId> columns;
    std::vector<std::vector<Value>> rows;
};

struct DeletePlan {
    TableId table_id = 0;
    std::optional<Expr> predicate;
};

struct PlanNode;

struct SeqScanNode {
    TableId table_id = 0;
};

struct FilterNode {
    Expr predicate;
    std::unique_ptr<PlanNode> child;
};

struct ProjectNode {
    std::vector<ColumnId> outputs;
    std::unique_ptr<PlanNode> child;
};

struct PlanNode {
    std::variant<SeqScanNode, FilterNode, ProjectNode> value;
};

struct QueryPlan {
    std::unique_ptr<PlanNode> root;
};

using Plan = std::variant<CreateTablePlan, InsertPlan, DeletePlan, QueryPlan>;

enum class CompileErrorKind {
    kLex,
    kSyntax,
    kSemantic,
};

struct CompileError {
    CompileErrorKind kind = CompileErrorKind::kSyntax;
    SourceLocation location;
    std::string message;
};

struct CompileRequest {
    std::string sql;
    CatalogView catalog;
};

struct CompileResult {
    std::variant<Plan, CompileError> value;
};

CompileResult compile(const CompileRequest& request);

}  // namespace tinydbms::compiler
