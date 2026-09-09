#include "plan_formatter.hpp"

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
    return type == Type::kInt ? "INT" : "VARCHAR";
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

[[nodiscard]] std::string value_text(const Value& value) {
    if (const auto* integer = std::get_if<std::int32_t>(&value.data)) {
        return "INT:" + std::to_string(*integer);
    }
    return "VARCHAR:\"" + escape_string(std::get<std::string>(value.data)) + "\"";
}

[[nodiscard]] std::string id_list(const std::vector<ColumnId>& columns) {
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
        return "ColumnRef[" + std::to_string(column->column_id) + ']';
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
            "SeqScan table_id=" + std::to_string(scan->table_id));
        return;
    }

    if (const auto* filter = std::get_if<FilterNode>(&node.kind)) {
        append_line(output, prefix, connector, "Filter");
        const std::string nested_prefix = child_prefix(prefix, connector);
        append_expression(output, filter->predicate, nested_prefix, kBranch, "predicate: ");
        append_plan_node(output, *filter->child, nested_prefix, kLastBranch);
        return;
    }

    const auto& project = std::get<ProjectNode>(node.kind);
    append_line(
        output,
        prefix,
        connector,
        "Project outputs=" + id_list(project.outputs));
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
        : "columns=" + id_list(insert.columns);
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
    std::string output = "Delete table_id=" + std::to_string(deletion.table_id) + '\n';
    if (!deletion.predicate.has_value()) {
        append_line(output, {}, kLastBranch, "predicate=<none>");
    } else {
        append_expression(output, *deletion.predicate, {}, kLastBranch, "predicate: ");
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

    std::string output;
    const auto& query = std::get<QueryPlan>(plan.kind);
    append_plan_node(output, *query.root, {}, {});
    return output;
}

}  // namespace tinydbms::compiler::internal
