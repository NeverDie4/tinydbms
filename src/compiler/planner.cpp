#include "planner.hpp"

#include <memory>
#include <optional>
#include <utility>
#include <variant>

namespace tinydbms::compiler::internal {
namespace {

Expr generate_expr(BoundExpr expression) {
    if (auto* column = std::get_if<BoundColumnRef>(&expression.kind)) {
        return Expr{ColumnRef{column->column_id}};
    }
    if (auto* literal = std::get_if<BoundLiteral>(&expression.kind)) {
        return Expr{Literal{std::move(literal->value)}};
    }
    if (auto* binary = std::get_if<BoundBinaryExpr>(&expression.kind)) {
        auto lhs = std::make_unique<Expr>(generate_expr(std::move(*binary->lhs)));
        auto rhs = std::make_unique<Expr>(generate_expr(std::move(*binary->rhs)));
        if (const auto* comparison = std::get_if<CmpOp>(&binary->op)) {
            return Expr{Binary{*comparison, std::move(lhs), std::move(rhs)}};
        }
        return Expr{Binary{
            std::get<LogicOp>(binary->op),
            std::move(lhs),
            std::move(rhs)
        }};
    }

    auto& unary = std::get<BoundUnaryExpr>(expression.kind);
    return Expr{Unary{
        std::make_unique<Expr>(generate_expr(std::move(*unary.operand)))
    }};
}

}  // namespace

Plan generate_plan(BoundCreateTable statement) {
    return Plan{CreateTablePlan{
        std::move(statement.table_name),
        std::move(statement.columns)
    }};
}

Plan generate_plan(BoundInsert statement) {
    return Plan{InsertPlan{
        statement.table_id,
        std::move(statement.columns),
        std::move(statement.rows)
    }};
}

Plan generate_plan(BoundSelect statement) {
    std::unique_ptr<PlanNode> child =
        std::make_unique<PlanNode>(SeqScanNode{statement.table_id});
    if (statement.predicate != nullptr) {
        child = std::make_unique<PlanNode>(FilterNode{
            generate_expr(std::move(*statement.predicate)),
            std::move(child)
        });
    }

    return Plan{QueryPlan{std::make_unique<PlanNode>(ProjectNode{
        std::move(statement.outputs),
        std::move(child)
    })}};
}

Plan generate_plan(BoundDelete statement) {
    std::optional<Expr> predicate;
    if (statement.predicate != nullptr) {
        predicate.emplace(generate_expr(std::move(*statement.predicate)));
    }
    return Plan{DeletePlan{statement.table_id, std::move(predicate)}};
}

}  // namespace tinydbms::compiler::internal
