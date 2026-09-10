#ifndef TINYDBMS_COMPILER_HPP
#define TINYDBMS_COMPILER_HPP

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "tinydbms/common.hpp"

namespace tinydbms::compiler {

struct CatalogView {
    // 借用 core Catalog 中的表元数据数组，编译调用期间有效
    std::span<const TableMeta> tables;
};

enum class CompileErrorKind {
    kLex,
    kSyntax,
    kSemantic
};

struct CompileError {
    CompileErrorKind kind;
    SourceLocation location;  // 相对该条 sql 文本，1-based
    std::string message;
};

enum class CmpOp {
    kEq, kNe, kLt, kLe, kGt, kGe
};

enum class LogicOp {
    kAnd, kOr
};

enum class UnaryOp {
    kNot
};

struct ColumnRef {
    ColumnId column_id;
};

struct Literal {
    Value value;
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Binary;
struct Unary;

// 二叉树节点：不允许默认构造，lhs/rhs 必须由调用方提供
struct Binary {
    Binary(CmpOp op, ExprPtr lhs, ExprPtr rhs);
    Binary(LogicOp op, ExprPtr lhs, ExprPtr rhs);
    ~Binary();
    Binary(Binary&&) noexcept;
    Binary& operator=(Binary&&) noexcept;
    Binary(const Binary&) = delete;
    Binary& operator=(const Binary&) = delete;

    std::variant<CmpOp, LogicOp> op;
    ExprPtr lhs;
    ExprPtr rhs;
};

// 初版仅支持 NOT，operand 必须由调用方提供
struct Unary {
    explicit Unary(ExprPtr operand);
    ~Unary();
    Unary(Unary&&) noexcept;
    Unary& operator=(Unary&&) noexcept;
    Unary(const Unary&) = delete;
    Unary& operator=(const Unary&) = delete;

    UnaryOp op{UnaryOp::kNot};
    ExprPtr operand;
};

// 表达式不可默认构造、不可复制；只能从四种节点构造
struct Expr {
    Expr(ColumnRef value);
    Expr(Literal value);
    Expr(Binary value);
    Expr(Unary value);
    ~Expr();
    Expr(Expr&&) noexcept;
    Expr& operator=(Expr&&) noexcept;
    Expr(const Expr&) = delete;
    Expr& operator=(const Expr&) = delete;

    std::variant<ColumnRef, Literal, Binary, Unary> kind;
};

inline Binary::Binary(CmpOp op_, ExprPtr lhs_, ExprPtr rhs_)
    : op{op_}, lhs{std::move(lhs_)}, rhs{std::move(rhs_)} {}

inline Binary::Binary(LogicOp op_, ExprPtr lhs_, ExprPtr rhs_)
    : op{op_}, lhs{std::move(lhs_)}, rhs{std::move(rhs_)} {}

inline Binary::~Binary() = default;
inline Binary::Binary(Binary&&) noexcept = default;
inline Binary& Binary::operator=(Binary&&) noexcept = default;

inline Unary::Unary(ExprPtr operand_) : operand{std::move(operand_)} {}
inline Unary::~Unary() = default;
inline Unary::Unary(Unary&&) noexcept = default;
inline Unary& Unary::operator=(Unary&&) noexcept = default;

inline Expr::Expr(ColumnRef value) : kind{std::move(value)} {}
inline Expr::Expr(Literal value) : kind{std::move(value)} {}
inline Expr::Expr(Binary value) : kind{std::move(value)} {}
inline Expr::Expr(Unary value) : kind{std::move(value)} {}
inline Expr::~Expr() = default;
inline Expr::Expr(Expr&&) noexcept = default;
inline Expr& Expr::operator=(Expr&&) noexcept = default;

struct SplitStatement {
    std::string sql;       // 一条语句文本，保留原文中从 start 到自身结尾分号的内容
    SourceLocation start;  // sql 首字符在整段输入中的绝对位置（1-based）
};

// 分句只能成功返回语句列表，或以 CompileError 返回整段输入错误。
struct SplitStatementsResult {
    SplitStatementsResult(std::vector<SplitStatement> statements);
    SplitStatementsResult(CompileError error);
    ~SplitStatementsResult();
    SplitStatementsResult(SplitStatementsResult&&) noexcept;
    SplitStatementsResult& operator=(SplitStatementsResult&&) noexcept;
    SplitStatementsResult(const SplitStatementsResult&) = delete;
    SplitStatementsResult& operator=(const SplitStatementsResult&) = delete;

    std::variant<std::vector<SplitStatement>, CompileError> outcome;
};

struct CreateTablePlan {
    std::string table_name;          // 尚无 ID，执行时由 core 分配
    std::vector<ColumnMeta> columns;
};

struct InsertPlan {
    TableId table_id;
    std::vector<ColumnId> columns;          // 空 = 全列按建表列序；非空 = 全列的重排
    std::vector<std::vector<Value>> rows;   // VALUES 多行；每行顺序与 columns 一致
};

struct DeletePlan {
    TableId table_id;
    std::optional<Expr> predicate;  // nullopt = 无 WHERE，删除全部
};

struct SeqScanNode {
    TableId table_id;
};

struct PlanNode;
struct FilterNode;
struct ProjectNode;

// 树节点不允许默认构造，child 必须非空
struct FilterNode {
    FilterNode(Expr predicate, std::unique_ptr<PlanNode> child);
    ~FilterNode();
    FilterNode(FilterNode&&) noexcept;
    FilterNode& operator=(FilterNode&&) noexcept;
    FilterNode(const FilterNode&) = delete;
    FilterNode& operator=(const FilterNode&) = delete;

    Expr predicate;
    std::unique_ptr<PlanNode> child;
};

struct ProjectNode {
    ProjectNode(std::vector<ColumnId> outputs, std::unique_ptr<PlanNode> child);
    ~ProjectNode();
    ProjectNode(ProjectNode&&) noexcept;
    ProjectNode& operator=(ProjectNode&&) noexcept;
    ProjectNode(const ProjectNode&) = delete;
    ProjectNode& operator=(const ProjectNode&) = delete;

    std::vector<ColumnId> outputs;  // SELECT 列表，* 已由 compiler 展开
    std::unique_ptr<PlanNode> child;
};

struct PlanNode {
    PlanNode(SeqScanNode scan);
    PlanNode(FilterNode filter);
    PlanNode(ProjectNode project);
    ~PlanNode();
    PlanNode(PlanNode&&) noexcept;
    PlanNode& operator=(PlanNode&&) noexcept;
    PlanNode(const PlanNode&) = delete;
    PlanNode& operator=(const PlanNode&) = delete;

    std::variant<SeqScanNode, FilterNode, ProjectNode> kind;
};

inline FilterNode::FilterNode(Expr predicate_, std::unique_ptr<PlanNode> child_)
    : predicate{std::move(predicate_)}, child{std::move(child_)} {}

inline ProjectNode::ProjectNode(std::vector<ColumnId> outputs_, std::unique_ptr<PlanNode> child_)
    : outputs{std::move(outputs_)}, child{std::move(child_)} {}

inline FilterNode::~FilterNode() = default;
inline FilterNode::FilterNode(FilterNode&&) noexcept = default;
inline FilterNode& FilterNode::operator=(FilterNode&&) noexcept = default;

inline ProjectNode::~ProjectNode() = default;
inline ProjectNode::ProjectNode(ProjectNode&&) noexcept = default;
inline ProjectNode& ProjectNode::operator=(ProjectNode&&) noexcept = default;

inline PlanNode::PlanNode(SeqScanNode value) : kind{std::move(value)} {}
inline PlanNode::PlanNode(FilterNode value) : kind{std::move(value)} {}
inline PlanNode::PlanNode(ProjectNode value) : kind{std::move(value)} {}
inline PlanNode::~PlanNode() = default;
inline PlanNode::PlanNode(PlanNode&&) noexcept = default;
inline PlanNode& PlanNode::operator=(PlanNode&&) noexcept = default;

// 根节点不允许默认构造，root 必须非空
struct QueryPlan {
    explicit QueryPlan(std::unique_ptr<PlanNode> root);
    ~QueryPlan();
    QueryPlan(QueryPlan&&) noexcept;
    QueryPlan& operator=(QueryPlan&&) noexcept;
    QueryPlan(const QueryPlan&) = delete;
    QueryPlan& operator=(const QueryPlan&) = delete;

    std::unique_ptr<PlanNode> root;
};

inline QueryPlan::QueryPlan(std::unique_ptr<PlanNode> root_) : root{std::move(root_)} {}
inline QueryPlan::~QueryPlan() = default;
inline QueryPlan::QueryPlan(QueryPlan&&) noexcept = default;
inline QueryPlan& QueryPlan::operator=(QueryPlan&&) noexcept = default;

struct Plan {
    Plan(CreateTablePlan value);
    Plan(InsertPlan value);
    Plan(DeletePlan value);
    Plan(QueryPlan value);
    ~Plan();
    Plan(Plan&&) noexcept;
    Plan& operator=(Plan&&) noexcept;
    Plan(const Plan&) = delete;
    Plan& operator=(const Plan&) = delete;

    std::variant<
        CreateTablePlan,
        InsertPlan,
        DeletePlan,
        QueryPlan
    > kind;
};

inline Plan::Plan(CreateTablePlan value) : kind{std::move(value)} {}
inline Plan::Plan(InsertPlan value) : kind{std::move(value)} {}
inline Plan::Plan(DeletePlan value) : kind{std::move(value)} {}
inline Plan::Plan(QueryPlan value) : kind{std::move(value)} {}
inline Plan::~Plan() = default;
inline Plan::Plan(Plan&&) noexcept = default;
inline Plan& Plan::operator=(Plan&&) noexcept = default;

struct CompileRequest {
    std::string sql;      // 一条语句文本（来自 SplitStatement.sql）
    CatalogView catalog;  // 只读借用
};

// 只能由 Plan 或 CompileError 构造，不允许默认的“空成功结果”
struct CompileResult {
    CompileResult(Plan plan);
    CompileResult(CompileError error);
    ~CompileResult();
    CompileResult(CompileResult&&) noexcept;
    CompileResult& operator=(CompileResult&&) noexcept;
    CompileResult(const CompileResult&) = delete;
    CompileResult& operator=(const CompileResult&) = delete;

    std::variant<Plan, CompileError> outcome;
};

inline CompileResult::CompileResult(Plan value) : outcome{std::move(value)} {}
inline CompileResult::CompileResult(CompileError value) : outcome{std::move(value)} {}
inline CompileResult::~CompileResult() = default;
inline CompileResult::CompileResult(CompileResult&&) noexcept = default;
inline CompileResult& CompileResult::operator=(CompileResult&&) noexcept = default;

inline SplitStatementsResult::SplitStatementsResult(std::vector<SplitStatement> value)
    : outcome{std::move(value)} {}
inline SplitStatementsResult::SplitStatementsResult(CompileError value)
    : outcome{std::move(value)} {}
inline SplitStatementsResult::~SplitStatementsResult() = default;
inline SplitStatementsResult::SplitStatementsResult(SplitStatementsResult&&) noexcept = default;
inline SplitStatementsResult& SplitStatementsResult::operator=(
    SplitStatementsResult&&) noexcept = default;

// 输入整段 SQL 文本，成功时返回带起点的语句列表；列表顺序即原文顺序。
SplitStatementsResult split_statements(std::string_view text);

// 无状态纯函数：相同 SQL + 相同 CatalogView 得到相同结果
CompileResult compile(const CompileRequest& request);

}  // namespace tinydbms::compiler

#endif  // TINYDBMS_COMPILER_HPP
