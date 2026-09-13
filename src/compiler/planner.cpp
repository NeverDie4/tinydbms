#include "planner.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace tinydbms::compiler::internal {
namespace {

struct SlotBinding {
    const TableMeta* table;
    std::vector<ScanColumn> columns;
};

using SlotBindings = std::vector<SlotBinding>;
using SlotBindingResult = std::variant<SlotBindings, CompileError>;
using ExprResult = std::variant<Expr, CompileError>;

[[nodiscard]] CompileError planner_error(std::string message) {
    return CompileError{
        CompileErrorKind::kSemantic,
        SourceLocation{1, 1},
        std::move(message)};
}

[[nodiscard]] const TableMeta* find_table(CatalogView catalog, TableId table_id) {
    const auto table = std::find_if(
        catalog.tables.begin(),
        catalog.tables.end(),
        [table_id](const TableMeta& candidate) {
            return candidate.table_id == table_id;
        });
    return table == catalog.tables.end() ? nullptr : &*table;
}

[[nodiscard]] SlotBindingResult build_slot_bindings(
    CatalogView catalog,
    std::span<const TableId> table_ids) {
    SlotBindings bindings;
    bindings.reserve(table_ids.size());
    std::size_t next_slot = 0;
    for (TableId table_id : table_ids) {
        const TableMeta* table = find_table(catalog, table_id);
        if (table == nullptr) {
            return planner_error("planner could not resolve bound table metadata");
        }

        SlotBinding binding{table, {}};
        binding.columns.reserve(table->columns.size());
        for (std::size_t index = 0; index < table->columns.size(); ++index) {
            if (index > std::numeric_limits<ColumnId>::max() ||
                next_slot > std::numeric_limits<SlotId>::max()) {
                return planner_error("table has too many columns for plan slot binding");
            }
            binding.columns.push_back(ScanColumn{
                static_cast<ColumnId>(index),
                static_cast<SlotId>(next_slot)});
            ++next_slot;
        }
        bindings.push_back(std::move(binding));
    }
    return bindings;
}

[[nodiscard]] std::optional<SlotId> find_slot(
    std::span<const SlotBinding> bindings,
    BoundColumnRef reference) {
    for (const SlotBinding& binding : bindings) {
        if (binding.table->table_id != reference.table_id) {
            continue;
        }
        const auto column = std::find_if(
            binding.columns.begin(),
            binding.columns.end(),
            [reference](const ScanColumn& candidate) {
                return candidate.column_id == reference.column_id;
            });
        return column == binding.columns.end()
            ? std::nullopt
            : std::optional<SlotId>{column->output_slot};
    }
    return std::nullopt;
}

[[nodiscard]] const SlotBinding* find_binding(
    std::span<const SlotBinding> bindings,
    TableId table_id) {
    const auto binding = std::find_if(
        bindings.begin(), bindings.end(), [table_id](const SlotBinding& candidate) {
            return candidate.table->table_id == table_id;
        });
    return binding == bindings.end() ? nullptr : &*binding;
}

[[nodiscard]] const char* aggregate_name(AggregateKind kind) {
    switch (kind) {
        case AggregateKind::kCount: return "COUNT";
        case AggregateKind::kSum: return "SUM";
        case AggregateKind::kAvg: return "AVG";
        case AggregateKind::kMin: return "MIN";
        case AggregateKind::kMax: return "MAX";
    }
    return "AGGREGATE";
}

[[nodiscard]] ExprResult generate_expr(
    BoundExpr expression,
    std::span<const SlotBinding> bindings) {
    if (auto* column = std::get_if<BoundColumnRef>(&expression.kind)) {
        const std::optional<SlotId> slot = find_slot(bindings, *column);
        if (!slot.has_value()) {
            return planner_error("bound expression references an unmapped column");
        }
        return Expr{ColumnRef{*slot}};
    }
    if (auto* literal = std::get_if<BoundLiteral>(&expression.kind)) {
        return Expr{Literal{std::move(literal->value)}};
    }
    if (auto* binary = std::get_if<BoundBinaryExpr>(&expression.kind)) {
        ExprResult lhs_result = generate_expr(std::move(*binary->lhs), bindings);
        if (auto* error = std::get_if<CompileError>(&lhs_result)) {
            return std::move(*error);
        }
        ExprResult rhs_result = generate_expr(std::move(*binary->rhs), bindings);
        if (auto* error = std::get_if<CompileError>(&rhs_result)) {
            return std::move(*error);
        }
        auto lhs = std::make_unique<Expr>(std::get<Expr>(std::move(lhs_result)));
        auto rhs = std::make_unique<Expr>(std::get<Expr>(std::move(rhs_result)));
        if (const auto* comparison = std::get_if<CmpOp>(&binary->op)) {
            return Expr{Binary{*comparison, std::move(lhs), std::move(rhs)}};
        }
        return Expr{Binary{
            std::get<LogicOp>(binary->op),
            std::move(lhs),
            std::move(rhs)}};
    }

    if (auto* null_test = std::get_if<BoundNullTestExpr>(&expression.kind)) {
        ExprResult operand_result = generate_expr(std::move(*null_test->operand), bindings);
        if (auto* error = std::get_if<CompileError>(&operand_result)) {
            return std::move(*error);
        }
        return Expr{NullTest{
            null_test->op,
            std::make_unique<Expr>(std::get<Expr>(std::move(operand_result)))}};
    }

    auto& unary = std::get<BoundUnaryExpr>(expression.kind);
    ExprResult operand_result = generate_expr(std::move(*unary.operand), bindings);
    if (auto* error = std::get_if<CompileError>(&operand_result)) {
        return std::move(*error);
    }
    return Expr{Unary{
        std::make_unique<Expr>(std::get<Expr>(std::move(operand_result)))}};
}

[[nodiscard]] PlannerResult slot_binding_error(SlotBindingResult& result) {
    return PlannerResult{std::get<CompileError>(std::move(result))};
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

PlannerResult generate_plan(BoundSelect statement, CatalogView catalog) {
    std::vector<TableId> table_ids;
    table_ids.reserve(statement.joins.size() + 1U);
    table_ids.push_back(statement.table_id);
    for (const BoundJoin& join : statement.joins) {
        table_ids.push_back(join.table_id);
    }
    SlotBindingResult binding_result = build_slot_bindings(catalog, table_ids);
    if (std::holds_alternative<CompileError>(binding_result)) {
        return slot_binding_error(binding_result);
    }
    SlotBindings bindings = std::get<SlotBindings>(std::move(binding_result));

    std::size_t next_slot = 0;
    for (const SlotBinding& binding : bindings) {
        next_slot += binding.columns.size();
    }

    std::vector<SlotId> group_keys;
    group_keys.reserve(statement.group_by.size());
    for (BoundColumnRef key : statement.group_by) {
        const std::optional<SlotId> slot = find_slot(bindings, key);
        if (!slot.has_value()) {
            return PlannerResult{planner_error(
                "bound GROUP BY key references an unmapped column")};
        }
        group_keys.push_back(*slot);
    }

    std::vector<SlotId> project_outputs;
    std::vector<QueryOutput> query_outputs;
    std::vector<AggregateCall> aggregate_calls;
    project_outputs.reserve(statement.items.size());
    query_outputs.reserve(statement.items.size());
    for (BoundSelectItem& item : statement.items) {
        if (const auto* output = std::get_if<BoundColumnRef>(&item)) {
            const SlotBinding* binding = find_binding(bindings, output->table_id);
            const std::optional<SlotId> slot = find_slot(bindings, *output);
            if (binding == nullptr || !slot.has_value() ||
                output->column_id >= binding->table->columns.size()) {
                return PlannerResult{planner_error(
                    "bound SELECT output references an unmapped column")};
            }
            const ColumnMeta& column = binding->table->columns[output->column_id];
            project_outputs.push_back(*slot);
            query_outputs.push_back(QueryOutput{
                *slot,
                column.name,
                column.type,
                column.nullable});
            continue;
        }

        const BoundAggregateCall& aggregate = std::get<BoundAggregateCall>(item);
        if (next_slot > std::numeric_limits<SlotId>::max()) {
            return PlannerResult{planner_error(
                "query has too many aggregate outputs for plan slot binding")};
        }
        const SlotId output_slot = static_cast<SlotId>(next_slot);
        ++next_slot;
        std::optional<SlotId> input_slot;
        std::string input_name{"*"};
        if (aggregate.argument.has_value()) {
            input_slot = find_slot(bindings, *aggregate.argument);
            const SlotBinding* binding = find_binding(
                bindings, aggregate.argument->table_id);
            if (!input_slot.has_value() || binding == nullptr ||
                aggregate.argument->column_id >= binding->table->columns.size()) {
                return PlannerResult{planner_error(
                    "bound aggregate argument references an unmapped column")};
            }
            input_name = binding->table->columns[aggregate.argument->column_id].name;
        }
        aggregate_calls.push_back(AggregateCall{
            aggregate.kind,
            input_slot,
            output_slot,
            aggregate.output_type,
            aggregate.nullable});
        project_outputs.push_back(output_slot);
        query_outputs.push_back(QueryOutput{
            output_slot,
            std::string{aggregate_name(aggregate.kind)} + "(" + input_name + ")",
            aggregate.output_type,
            aggregate.nullable});
    }

    std::optional<Expr> predicate;
    if (statement.predicate != nullptr) {
        ExprResult predicate_result = generate_expr(
            std::move(*statement.predicate),
            bindings);
        if (auto* error = std::get_if<CompileError>(&predicate_result)) {
            return PlannerResult{std::move(*error)};
        }
        predicate.emplace(std::get<Expr>(std::move(predicate_result)));
    }

    std::vector<SortKey> sort_keys;
    sort_keys.reserve(statement.order_by.size());
    for (const BoundSortKey& key : statement.order_by) {
        const std::optional<SlotId> slot = find_slot(bindings, key.column);
        if (!slot.has_value()) {
            return PlannerResult{planner_error(
                "bound ORDER BY key references an unmapped column")};
        }
        sort_keys.push_back(SortKey{*slot, key.direction});
    }

    std::vector<Expr> join_conditions;
    join_conditions.reserve(statement.joins.size());
    for (std::size_t index = 0; index < statement.joins.size(); ++index) {
        ExprResult condition_result = generate_expr(
            std::move(*statement.joins[index].condition),
            std::span<const SlotBinding>{bindings}.first(index + 2U));
        if (auto* error = std::get_if<CompileError>(&condition_result)) {
            return PlannerResult{std::move(*error)};
        }
        join_conditions.push_back(std::get<Expr>(std::move(condition_result)));
    }

    std::unique_ptr<PlanNode> child = std::make_unique<PlanNode>(SeqScanNode{
        statement.table_id,
        std::move(bindings.front().columns)});
    for (std::size_t index = 0; index < statement.joins.size(); ++index) {
        std::unique_ptr<PlanNode> right = std::make_unique<PlanNode>(SeqScanNode{
            statement.joins[index].table_id,
            std::move(bindings[index + 1U].columns)});
        child = std::make_unique<PlanNode>(JoinNode{
            JoinKind::kInner,
            std::move(join_conditions[index]),
            std::move(child),
            std::move(right)});
    }
    if (predicate.has_value()) {
        child = std::make_unique<PlanNode>(FilterNode{
            std::move(*predicate),
            std::move(child)});
    }
    if (!group_keys.empty() || !aggregate_calls.empty()) {
        child = std::make_unique<PlanNode>(AggregateNode{
            std::move(group_keys),
            std::move(aggregate_calls),
            std::move(child)});
    }
    if (!sort_keys.empty()) {
        child = std::make_unique<PlanNode>(SortNode{
            std::move(sort_keys),
            std::move(child)});
    }

    return PlannerResult{Plan{QueryPlan{
        std::make_unique<PlanNode>(ProjectNode{
            std::move(project_outputs),
            std::move(child)}),
        std::move(query_outputs)}}};
}

PlannerResult generate_plan(BoundDelete statement, CatalogView catalog) {
    const std::array table_ids{statement.table_id};
    SlotBindingResult binding_result = build_slot_bindings(catalog, table_ids);
    if (std::holds_alternative<CompileError>(binding_result)) {
        return slot_binding_error(binding_result);
    }
    SlotBinding binding = std::move(std::get<SlotBindings>(binding_result).front());

    std::optional<Expr> predicate;
    if (statement.predicate != nullptr) {
        ExprResult predicate_result = generate_expr(
            std::move(*statement.predicate),
            std::span<const SlotBinding>{&binding, 1U});
        if (auto* error = std::get_if<CompileError>(&predicate_result)) {
            return PlannerResult{std::move(*error)};
        }
        predicate.emplace(std::get<Expr>(std::move(predicate_result)));
    }
    return PlannerResult{Plan{DeletePlan{
        statement.table_id,
        std::move(binding.columns),
        std::move(predicate)}}};
}

PlannerResult generate_plan(BoundUpdate statement, CatalogView catalog) {
    const std::array table_ids{statement.table_id};
    SlotBindingResult binding_result = build_slot_bindings(catalog, table_ids);
    if (std::holds_alternative<CompileError>(binding_result)) {
        return slot_binding_error(binding_result);
    }
    SlotBinding binding = std::move(std::get<SlotBindings>(binding_result).front());

    std::optional<Expr> predicate;
    if (statement.predicate != nullptr) {
        ExprResult predicate_result = generate_expr(
            std::move(*statement.predicate),
            std::span<const SlotBinding>{&binding, 1U});
        if (auto* error = std::get_if<CompileError>(&predicate_result)) {
            return PlannerResult{std::move(*error)};
        }
        predicate.emplace(std::get<Expr>(std::move(predicate_result)));
    }

    std::vector<UpdateAssignment> assignments;
    assignments.reserve(statement.assignments.size());
    for (BoundUpdateAssignment& assignment : statement.assignments) {
        if (assignment.column_id >= binding.table->columns.size()) {
            return PlannerResult{planner_error(
                "bound UPDATE assignment references an unmapped column")};
        }
        assignments.push_back(UpdateAssignment{
            assignment.column_id,
            std::move(assignment.value)});
    }
    return PlannerResult{Plan{UpdatePlan{
        statement.table_id,
        std::move(binding.columns),
        std::move(assignments),
        std::move(predicate)}}};
}

}  // namespace tinydbms::compiler::internal
