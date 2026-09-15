#include "plan_formatter.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace tinydbms::compiler::internal {
namespace {

constexpr std::string_view kBranch{"├── "};
constexpr std::string_view kLastBranch{"└── "};

[[nodiscard]] std::string_view type_name(Type type) {
    switch (type) {
        case Type::kInt:
            return "INT";
        case Type::kBigInt:
            return "BIGINT";
        case Type::kDouble:
            return "DOUBLE";
        case Type::kBoolean:
            return "BOOLEAN";
        case Type::kVarchar:
            return "VARCHAR";
    }
    return "unknown";
}

[[nodiscard]] std::string_view comparison_name(CmpOp op) {
    switch (op) {
        case CmpOp::kEq:
            return "=";
        case CmpOp::kNe:
            return "!=";
        case CmpOp::kLt:
            return "<";
        case CmpOp::kLe:
            return "<=";
        case CmpOp::kGt:
            return ">";
        case CmpOp::kGe:
            return ">=";
    }
    return "?";
}

[[nodiscard]] std::string_view logic_name(LogicOp op) {
    return op == LogicOp::kAnd ? "AND" : "OR";
}

[[nodiscard]] std::string escape_string(std::string_view value) {
    std::string escaped;
    for (const char character : value) {
        switch (character) {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped += character;
                break;
        }
    }
    return escaped;
}

[[nodiscard]] std::string double_text(double value) {
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc{}) {
        return "<unformattable>";
    }
    std::string result(buffer.data(), end);
    if (result.find_first_of(".eE") == std::string::npos) {
        result += ".0";
    }
    return result;
}

[[nodiscard]] std::string value_text(const Value& value) {
    if (std::holds_alternative<std::monostate>(value.data)) {
        return "NULL";
    }
    if (const auto* integer = std::get_if<std::int32_t>(&value.data)) {
        return "INT:" + std::to_string(*integer);
    }
    if (const auto* bigint = std::get_if<std::int64_t>(&value.data)) {
        return "BIGINT:" + std::to_string(*bigint);
    }
    if (const auto* number = std::get_if<double>(&value.data)) {
        return "DOUBLE:" + double_text(*number);
    }
    if (const auto* boolean = std::get_if<bool>(&value.data)) {
        return *boolean ? "BOOLEAN:TRUE" : "BOOLEAN:FALSE";
    }
    if (const auto* text = std::get_if<std::string>(&value.data)) {
        return "VARCHAR:\"" + escape_string(*text) + "\"";
    }
    return "<unsupported>";
}

[[nodiscard]] std::string column_id_list(const std::vector<ColumnId>& columns) {
    std::string result{"["};
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (index != 0) {
            result += ',';
        }
        result += std::to_string(columns[index]);
    }
    result += ']';
    return result;
}

[[nodiscard]] std::string slot_list(const std::vector<SlotId>& slots) {
    std::string result{"["};
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (index != 0) {
            result += ',';
        }
        result += 's' + std::to_string(slots[index]);
    }
    result += ']';
    return result;
}

[[nodiscard]] std::string scan_columns_text(
    const std::vector<ScanColumn>& columns) {
    std::string result{"["};
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (index != 0) {
            result += ',';
        }
        result += 'c' + std::to_string(columns[index].column_id) + "->s" +
            std::to_string(columns[index].output_slot);
    }
    result += ']';
    return result;
}

[[nodiscard]] std::string sort_keys_text(const std::vector<SortKey>& keys) {
    std::string result{"["};
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (index != 0) {
            result += ',';
        }
        result += 's' + std::to_string(keys[index].slot_id);
        result += keys[index].direction == SortDirection::kAsc ? " ASC" : " DESC";
    }
    result += ']';
    return result;
}

[[nodiscard]] std::string_view aggregate_name(AggregateKind kind) {
    switch (kind) {
        case AggregateKind::kCount: return "COUNT";
        case AggregateKind::kSum: return "SUM";
        case AggregateKind::kAvg: return "AVG";
        case AggregateKind::kMin: return "MIN";
        case AggregateKind::kMax: return "MAX";
    }
    return "AGGREGATE";
}

[[nodiscard]] std::string aggregate_text(const AggregateCall& aggregate) {
    std::string result{aggregate_name(aggregate.kind)};
    result += '(';
    result += aggregate.input_slot.has_value()
        ? "s" + std::to_string(*aggregate.input_slot)
        : "*";
    result += ") -> s" + std::to_string(aggregate.output_slot);
    result += " type=" + std::string{type_name(aggregate.output_type)};
    result += aggregate.nullable ? " nullable=true" : " nullable=false";
    return result;
}

[[nodiscard]] std::string row_text(const std::vector<Value>& row) {
    std::string result{"["};
    for (std::size_t index = 0; index < row.size(); ++index) {
        if (index != 0) {
            result += ',';
        }
        result += value_text(row[index]);
    }
    result += ']';
    return result;
}

void append_line(
    std::string& output,
    std::string_view prefix,
    std::string_view connector,
    std::string_view text) {
    output += prefix;
    output += connector;
    output += text;
    output += '\n';
}

[[nodiscard]] std::string child_prefix(
    std::string_view prefix,
    std::string_view connector) {
    std::string result{prefix};
    if (connector == kBranch) {
        result += "│   ";
    } else if (connector == kLastBranch) {
        result += "    ";
    }
    return result;
}

[[nodiscard]] std::string expression_name(const Expr& expression) {
    if (const auto* column = std::get_if<ColumnRef>(&expression.kind)) {
        return "ColumnRef[s" + std::to_string(column->slot_id) + ']';
    }
    if (const auto* literal = std::get_if<Literal>(&expression.kind)) {
        return "Literal(" + value_text(literal->value) + ')';
    }
    if (const auto* binary = std::get_if<Binary>(&expression.kind)) {
        if (const auto* comparison = std::get_if<CmpOp>(&binary->op)) {
            return std::string{comparison_name(*comparison)};
        }
        return std::string{logic_name(std::get<LogicOp>(binary->op))};
    }
    if (const auto* null_test = std::get_if<NullTest>(&expression.kind)) {
        return null_test->op == NullTestOp::kIsNull ? "IS NULL" : "IS NOT NULL";
    }
    return "NOT";
}

void append_expression(
    std::string& output,
    const Expr& expression,
    std::string_view prefix,
    std::string_view connector,
    std::string_view label_prefix = {}) {
    append_line(output, prefix, connector, std::string{label_prefix} + expression_name(expression));
    const std::string nested_prefix = child_prefix(prefix, connector);

    if (const auto* binary = std::get_if<Binary>(&expression.kind)) {
        append_expression(output, *binary->lhs, nested_prefix, kBranch);
        append_expression(output, *binary->rhs, nested_prefix, kLastBranch);
    } else if (const auto* unary = std::get_if<Unary>(&expression.kind)) {
        append_expression(output, *unary->operand, nested_prefix, kLastBranch);
    } else if (const auto* null_test = std::get_if<NullTest>(&expression.kind)) {
        append_expression(output, *null_test->operand, nested_prefix, kLastBranch);
    }
}

void append_plan_node(
    std::string& output,
    const PlanNode& node,
    std::string_view prefix,
    std::string_view connector) {
    if (const auto* scan = std::get_if<SeqScanNode>(&node.kind)) {
        append_line(
            output,
            prefix,
            connector,
            "SeqScan table_id=" + std::to_string(scan->table_id) +
                " columns=" + scan_columns_text(scan->columns));
        return;
    }

    if (const auto* filter = std::get_if<FilterNode>(&node.kind)) {
        append_line(output, prefix, connector, "Filter");
        const std::string nested_prefix = child_prefix(prefix, connector);
        append_expression(output, filter->predicate, nested_prefix, kBranch, "predicate: ");
        append_plan_node(output, *filter->child, nested_prefix, kLastBranch);
        return;
    }

    if (const auto* join = std::get_if<JoinNode>(&node.kind)) {
        append_line(output, prefix, connector, "InnerJoin");
        const std::string nested_prefix = child_prefix(prefix, connector);
        append_expression(output, join->condition, nested_prefix, kBranch, "condition: ");
        append_plan_node(output, *join->left, nested_prefix, kBranch);
        append_plan_node(output, *join->right, nested_prefix, kLastBranch);
        return;
    }

    if (const auto* aggregate = std::get_if<AggregateNode>(&node.kind)) {
        append_line(
            output,
            prefix,
            connector,
            "Aggregate group_by=" + slot_list(aggregate->group_keys));
        const std::string nested_prefix = child_prefix(prefix, connector);
        for (std::size_t index = 0; index < aggregate->aggregates.size(); ++index) {
            append_line(
                output,
                nested_prefix,
                kBranch,
                "aggregate[" + std::to_string(index) + "]: " +
                    aggregate_text(aggregate->aggregates[index]));
        }
        append_plan_node(output, *aggregate->child, nested_prefix, kLastBranch);
        return;
    }

    if (const auto* sort = std::get_if<SortNode>(&node.kind)) {
        append_line(
            output,
            prefix,
            connector,
            "Sort keys=" + sort_keys_text(sort->keys));
        append_plan_node(
            output,
            *sort->child,
            child_prefix(prefix, connector),
            kLastBranch);
        return;
    }

    const auto& project = std::get<ProjectNode>(node.kind);
    append_line(
        output,
        prefix,
        connector,
        "Project outputs=" + slot_list(project.outputs));
    append_plan_node(
        output,
        *project.child,
        child_prefix(prefix, connector),
        kLastBranch);
}

[[nodiscard]] std::string format_create(const CreateTablePlan& create) {
    std::string output = "CreateTable name=" + create.table_name + '\n';
    for (std::size_t index = 0; index < create.columns.size(); ++index) {
        const ColumnMeta& column = create.columns[index];
        const std::string_view connector = index + 1 == create.columns.size()
            ? kLastBranch
            : kBranch;
        append_line(
            output,
            {},
            connector,
            "column[" + std::to_string(index) + "] name=" + column.name +
                " type=" + std::string{type_name(column.type)});
    }
    return output;
}

[[nodiscard]] std::string format_insert(const InsertPlan& insert) {
    std::string output = "Insert table_id=" + std::to_string(insert.table_id) + '\n';
    const std::string columns = insert.columns.empty()
        ? "columns=<schema-order>"
        : "columns=" + column_id_list(insert.columns);
    append_line(output, {}, insert.rows.empty() ? kLastBranch : kBranch, columns);
    for (std::size_t index = 0; index < insert.rows.size(); ++index) {
        const std::string_view connector = index + 1 == insert.rows.size()
            ? kLastBranch
            : kBranch;
        append_line(
            output,
            {},
            connector,
            "row[" + std::to_string(index) + "]=" + row_text(insert.rows[index]));
    }
    return output;
}

[[nodiscard]] std::string format_delete(const DeletePlan& deletion) {
    std::string output = "Delete table_id=" + std::to_string(deletion.table_id) +
        " input=" + scan_columns_text(deletion.input_columns) + '\n';
    if (!deletion.predicate.has_value()) {
        append_line(output, {}, kLastBranch, "predicate=<none>");
    } else {
        append_expression(output, *deletion.predicate, {}, kLastBranch, "predicate: ");
    }
    return output;
}

[[nodiscard]] std::string format_update(const UpdatePlan& update) {
    std::string output = "Update table_id=" + std::to_string(update.table_id) +
        " input=" + scan_columns_text(update.input_columns) + '\n';
    for (std::size_t index = 0; index < update.assignments.size(); ++index) {
        const UpdateAssignment& assignment = update.assignments[index];
        const bool last = index + 1U == update.assignments.size() &&
            !update.predicate.has_value();
        append_line(
            output,
            {},
            last ? kLastBranch : kBranch,
            "assignment[c" + std::to_string(assignment.column_id) + "]=" +
                value_text(assignment.value));
    }
    if (update.predicate.has_value()) {
        append_expression(output, *update.predicate, {}, kLastBranch, "predicate: ");
    }
    return output;
}

}  // namespace

std::string format_plan(const Plan& plan) {
    if (const auto* create = std::get_if<CreateTablePlan>(&plan.kind)) {
        return format_create(*create);
    }
    if (const auto* insert = std::get_if<InsertPlan>(&plan.kind)) {
        return format_insert(*insert);
    }
    if (const auto* deletion = std::get_if<DeletePlan>(&plan.kind)) {
        return format_delete(*deletion);
    }
    if (const auto* update = std::get_if<UpdatePlan>(&plan.kind)) {
        return format_update(*update);
    }

    std::string output;
    const auto& query = std::get<QueryPlan>(plan.kind);
    append_plan_node(output, *query.root, {}, {});
    return output;
}

}  // namespace tinydbms::compiler::internal
