#include "plan_text.hpp"

#include "expression.hpp"
#include "internal.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace tinydbms::core::internal {
namespace {

constexpr std::size_t kIndentWidth = 2;

// 与 executor 的 Plan 深度防御同口径：即使 Plan 来自违约实现，渲染也以 kInternal
// 结束，而不是靠递归把栈打穿。compiler 的表达式复杂度预算另有 kMaxExpressionDepth。
constexpr std::size_t kMaxPlanDepth = 256;

std::string_view type_name(Type type) noexcept {
    switch (type) {
    case Type::kInt:
        return "INT";
    case Type::kBigInt:
        return "BIGINT";
    case Type::kVarchar:
        return "VARCHAR";
    case Type::kDouble:
        return "DOUBLE";
    case Type::kBoolean:
        return "BOOLEAN";
    }
    return "unknown";
}

std::string_view comparison_name(compiler::CmpOp op) noexcept {
    switch (op) {
    case compiler::CmpOp::kEq:
        return "=";
    case compiler::CmpOp::kNe:
        return "<>";
    case compiler::CmpOp::kLt:
        return "<";
    case compiler::CmpOp::kLe:
        return "<=";
    case compiler::CmpOp::kGt:
        return ">";
    case compiler::CmpOp::kGe:
        return ">=";
    }
    return "?";
}

std::string_view logic_name(compiler::LogicOp op) noexcept {
    return op == compiler::LogicOp::kAnd ? "AND" : "OR";
}

std::string_view aggregate_name(compiler::AggregateKind kind) noexcept {
    switch (kind) {
    case compiler::AggregateKind::kCount:
        return "COUNT";
    case compiler::AggregateKind::kSum:
        return "SUM";
    case compiler::AggregateKind::kAvg:
        return "AVG";
    case compiler::AggregateKind::kMin:
        return "MIN";
    case compiler::AggregateKind::kMax:
        return "MAX";
    }
    return "AGGREGATE";
}

std::string_view join_kind_name(compiler::JoinKind kind) noexcept {
    return kind == compiler::JoinKind::kInner ? "INNER" : "UNKNOWN";
}

const TableMeta* find_table(
    const std::span<const TableMeta> catalog,
    TableId table_id) noexcept {
    for (const TableMeta& table : catalog) {
        if (table.table_id == table_id) {
            return &table;
        }
    }
    return nullptr;
}

std::string number_text(std::uint64_t value) {
    return std::to_string(value);
}

std::string slot_label(SlotId slot_id) {
    return "slot" + number_text(slot_id);
}

std::string table_label(
    const std::span<const TableMeta> catalog,
    TableId table_id) {
    const TableMeta* table = find_table(catalog, table_id);
    return table == nullptr ? "table#" + number_text(table_id) : table->table_name;
}

std::string column_label(
    const std::span<const TableMeta> catalog,
    TableId table_id,
    ColumnId column_id) {
    const TableMeta* table = find_table(catalog, table_id);
    if (table != nullptr && column_id < table->columns.size()) {
        return table->columns[column_id].name;
    }
    return "column#" + number_text(column_id);
}

// 计划文本里的字符串字面量使用 SQL 单引号写法，内部单引号翻倍。
std::string quoted_text(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2U);
    result.push_back('\'');
    for (const char character : value) {
        if (character == '\'') {
            result += "''";
            continue;
        }
        result.push_back(character);
    }
    result.push_back('\'');
    return result;
}

std::string double_text(double value) {
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc{}) {
        return "<unsupported DOUBLE value>";
    }
    std::string result(buffer.data(), end);
    if (result.find_first_of(".eE") == std::string::npos) {
        result += ".0";
    }
    return result;
}

std::string value_text(const Value& value) {
    return std::visit(
        [](const auto& item) -> std::string {
            using Item = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Item, std::monostate>) {
                return "NULL";
            } else if constexpr (std::is_same_v<Item, std::int32_t>) {
                return std::to_string(item);
            } else if constexpr (std::is_same_v<Item, std::int64_t>) {
                return std::to_string(item);
            } else if constexpr (std::is_same_v<Item, double>) {
                return double_text(item);
            } else if constexpr (std::is_same_v<Item, bool>) {
                return item ? "TRUE" : "FALSE";
            } else {
                return quoted_text(item);
            }
        },
        value.data);
}

// slot → 展示用名字。扫描列的 slot 额外记录表名，多表场景据此加表名限定。
struct SlotNames {
    struct Entry {
        std::string column;  // 解析不到列名时为空
        std::string table;   // 只来自扫描列；输出 slot 为空
    };

    std::unordered_map<SlotId, Entry> entries;
    bool qualify_with_table = false;
};

void collect_scan_slots(
    const compiler::PlanNode& node,
    const std::span<const TableMeta> catalog,
    SlotNames& names) {
    std::visit(
        [&](const auto& typed) {
            using Node = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Node, compiler::SeqScanNode>) {
                const TableMeta* table = find_table(catalog, typed.table_id);
                const std::string table_text = table_label(catalog, typed.table_id);
                for (const compiler::ScanColumn& column : typed.columns) {
                    SlotNames::Entry entry;
                    if (table != nullptr && column.column_id < table->columns.size()) {
                        entry.column = table->columns[column.column_id].name;
                        entry.table = table_text;
                    }
                    names.entries.insert_or_assign(column.output_slot, std::move(entry));
                }
            } else if constexpr (std::is_same_v<Node, compiler::JoinNode>) {
                collect_scan_slots(*typed.left, catalog, names);
                collect_scan_slots(*typed.right, catalog, names);
            } else {
                collect_scan_slots(*typed.child, catalog, names);
            }
        },
        node.kind);
}

std::size_t count_scans(const compiler::PlanNode& node) {
    return std::visit(
        [&](const auto& typed) -> std::size_t {
            using Node = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Node, compiler::SeqScanNode>) {
                return 1U;
            } else if constexpr (std::is_same_v<Node, compiler::JoinNode>) {
                return count_scans(*typed.left) + count_scans(*typed.right);
            } else {
                return count_scans(*typed.child);
            }
        },
        node.kind);
}

std::string slot_ref(const SlotId slot_id, const SlotNames& names) {
    const auto entry = names.entries.find(slot_id);
    if (entry == names.entries.end() || entry->second.column.empty()) {
        // 解析不到列名时整个“名字”退化成分隔符前的 slot，保持 <名字>#<slot> 的形状。
        return "slot#" + number_text(slot_id);
    }
    std::string result;
    if (names.qualify_with_table && !entry->second.table.empty()) {
        result += entry->second.table;
        result.push_back('.');
    }
    result += entry->second.column;
    result.push_back('#');
    result += number_text(slot_id);
    return result;
}

std::optional<std::string> validate_expression(
    const compiler::Expr& expression,
    const std::size_t depth) {
    if (depth >= expression::kMaxExpressionDepth) {
        return std::optional<std::string>{"expression depth exceeds core limit"};
    }
    return std::visit(
        [&](const auto& typed) -> std::optional<std::string> {
            using Node = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Node, compiler::Binary>) {
                if (typed.lhs == nullptr || typed.rhs == nullptr) {
                    return std::optional<std::string>{
                        "binary expression is missing an operand"};
                }
                if (auto error = validate_expression(*typed.lhs, depth + 1U);
                    error.has_value()) {
                    return error;
                }
                return validate_expression(*typed.rhs, depth + 1U);
            } else if constexpr (std::is_same_v<Node, compiler::Unary>) {
                if (typed.operand == nullptr) {
                    return std::optional<std::string>{
                        "unary expression is missing an operand"};
                }
                return validate_expression(*typed.operand, depth + 1U);
            } else if constexpr (std::is_same_v<Node, compiler::NullTest>) {
                if (typed.operand == nullptr) {
                    return std::optional<std::string>{
                        "null test is missing an operand"};
                }
                return validate_expression(*typed.operand, depth + 1U);
            } else {
                return std::nullopt;
            }
        },
        expression.kind);
}

std::optional<std::string> validate_node(
    const compiler::PlanNode& node,
    const std::size_t depth) {
    if (depth >= kMaxPlanDepth) {
        return std::optional<std::string>{"query plan depth exceeds core limit"};
    }
    return std::visit(
        [&](const auto& typed) -> std::optional<std::string> {
            using Node = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Node, compiler::SeqScanNode>) {
                return std::nullopt;
            } else if constexpr (std::is_same_v<Node, compiler::JoinNode>) {
                if (typed.left == nullptr || typed.right == nullptr) {
                    return std::optional<std::string>{"join node is missing a child"};
                }
                if (auto error = validate_expression(typed.condition, 0U);
                    error.has_value()) {
                    return error;
                }
                if (auto error = validate_node(*typed.left, depth + 1U);
                    error.has_value()) {
                    return error;
                }
                return validate_node(*typed.right, depth + 1U);
            } else if constexpr (std::is_same_v<Node, compiler::FilterNode>) {
                if (typed.child == nullptr) {
                    return std::optional<std::string>{"filter node is missing a child"};
                }
                if (auto error = validate_expression(typed.predicate, 0U);
                    error.has_value()) {
                    return error;
                }
                return validate_node(*typed.child, depth + 1U);
            } else {
                if (typed.child == nullptr) {
                    return std::optional<std::string>{"plan node is missing a child"};
                }
                return validate_node(*typed.child, depth + 1U);
            }
        },
        node.kind);
}

std::optional<std::string> validate_plan(const compiler::Plan& plan) {
    return std::visit(
        [&](const auto& typed) -> std::optional<std::string> {
            using PlanType = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<PlanType, compiler::QueryPlan>) {
                if (typed.root == nullptr) {
                    return std::optional<std::string>{"query plan is missing a root node"};
                }
                return validate_node(*typed.root, 0U);
            } else if constexpr (std::is_same_v<PlanType, compiler::DeletePlan>) {
                if (!typed.predicate.has_value()) {
                    return std::nullopt;
                }
                return validate_expression(*typed.predicate, 0U);
            } else if constexpr (std::is_same_v<PlanType, compiler::UpdatePlan>) {
                if (!typed.predicate.has_value()) {
                    return std::nullopt;
                }
                return validate_expression(*typed.predicate, 0U);
            } else {
                return std::nullopt;
            }
        },
        plan.kind);
}

class PlanRenderer {
public:
    explicit PlanRenderer(const std::span<const TableMeta> catalog) : catalog_{catalog} {}

    std::vector<std::string> statement(const compiler::Plan& plan) const {
        return std::visit(
            [&](const auto& typed) -> std::vector<std::string> {
                using PlanType = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<PlanType, compiler::CreateTablePlan>) {
                    return std::vector<std::string>{create_table_text(typed)};
                } else if constexpr (std::is_same_v<PlanType, compiler::InsertPlan>) {
                    return std::vector<std::string>{insert_text(typed)};
                } else if constexpr (std::is_same_v<PlanType, compiler::DeletePlan>) {
                    return std::vector<std::string>{delete_text(typed)};
                } else if constexpr (std::is_same_v<PlanType, compiler::UpdatePlan>) {
                    return std::vector<std::string>{update_text(typed)};
                } else {
                    return query_text(typed);
                }
            },
            plan.kind);
    }

private:
    std::string expression_text(
        const compiler::Expr& expression,
        const SlotNames& names) const {
        return std::visit(
            [&](const auto& typed) -> std::string {
                using Node = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<Node, compiler::ColumnRef>) {
                    return slot_ref(typed.slot_id, names);
                } else if constexpr (std::is_same_v<Node, compiler::Literal>) {
                    return value_text(typed.value);
                } else if constexpr (std::is_same_v<Node, compiler::Binary>) {
                    std::string result{"("};
                    result += expression_text(*typed.lhs, names);
                    result.push_back(' ');
                    result += std::visit(
                        [](const auto& op) -> std::string {
                            using Op = std::decay_t<decltype(op)>;
                            if constexpr (std::is_same_v<Op, compiler::CmpOp>) {
                                return std::string{comparison_name(op)};
                            } else {
                                return std::string{logic_name(op)};
                            }
                        },
                        typed.op);
                    result.push_back(' ');
                    result += expression_text(*typed.rhs, names);
                    result.push_back(')');
                    return result;
                } else if constexpr (std::is_same_v<Node, compiler::Unary>) {
                    return "NOT (" + expression_text(*typed.operand, names) + ')';
                } else {
                    return '(' + expression_text(*typed.operand, names) +
                        (typed.op == compiler::NullTestOp::kIsNull
                             ? " IS NULL)"
                             : " IS NOT NULL)");
                }
            },
            expression.kind);
    }

    SlotNames scan_slot_names(
        const TableId table_id,
        const std::vector<compiler::ScanColumn>& columns) const {
        SlotNames names;
        const TableMeta* table = find_table(catalog_, table_id);
        for (const compiler::ScanColumn& column : columns) {
            SlotNames::Entry entry;
            if (table != nullptr && column.column_id < table->columns.size()) {
                entry.column = table->columns[column.column_id].name;
            }
            names.entries.insert_or_assign(column.output_slot, std::move(entry));
        }
        return names;
    }

    std::string create_table_text(const compiler::CreateTablePlan& plan) const {
        std::string result{"CreateTable "};
        result += plan.table_name.empty() ? std::string{"<unnamed>"} : plan.table_name;
        result.push_back('(');
        for (std::size_t index = 0; index < plan.columns.size(); ++index) {
            if (index != 0) {
                result += ", ";
            }
            const ColumnMeta& column = plan.columns[index];
            result += column.name.empty() ? std::string{"<unnamed>"} : column.name;
            result.push_back(' ');
            result += type_name(column.type);
            if (!column.nullable) {
                result += " NOT NULL";
            }
        }
        result.push_back(')');
        return result;
    }

    std::string insert_text(const compiler::InsertPlan& plan) const {
        std::string result{"Insert table="};
        result += table_label(catalog_, plan.table_id);
        result += " columns=[";
        if (plan.columns.empty()) {
            result.push_back('*');
        } else {
            for (std::size_t index = 0; index < plan.columns.size(); ++index) {
                if (index != 0) {
                    result += ", ";
                }
                result += column_label(catalog_, plan.table_id, plan.columns[index]);
            }
        }
        result += "] rows=";
        result += number_text(plan.rows.size());
        return result;
    }

    std::string delete_text(const compiler::DeletePlan& plan) const {
        const SlotNames names = scan_slot_names(plan.table_id, plan.input_columns);
        std::string result{"Delete table="};
        result += table_label(catalog_, plan.table_id);
        result += " predicate=";
        result += plan.predicate.has_value()
            ? expression_text(*plan.predicate, names)
            : std::string{"ALL"};
        return result;
    }

    std::string update_text(const compiler::UpdatePlan& plan) const {
        const SlotNames names = scan_slot_names(plan.table_id, plan.input_columns);
        std::string result{"Update table="};
        result += table_label(catalog_, plan.table_id);
        result += " assignments=[";
        for (std::size_t index = 0; index < plan.assignments.size(); ++index) {
            if (index != 0) {
                result += ", ";
            }
            result += column_label(catalog_, plan.table_id, plan.assignments[index].column_id);
            result += " = ";
            result += value_text(plan.assignments[index].value);
        }
        result += "] predicate=";
        result += plan.predicate.has_value()
            ? expression_text(*plan.predicate, names)
            : std::string{"ALL"};
        return result;
    }

    std::vector<std::string> query_text(const compiler::QueryPlan& plan) const {
        SlotNames names;
        names.qualify_with_table = count_scans(*plan.root) > 1U;
        collect_scan_slots(*plan.root, catalog_, names);
        for (const compiler::QueryOutput& output : plan.outputs) {
            if (names.entries.find(output.slot_id) != names.entries.end()) {
                continue;
            }
            names.entries.insert_or_assign(
                output.slot_id,
                SlotNames::Entry{output.name, std::string{}});
        }

        std::vector<std::string> lines;
        std::string header{"QueryPlan outputs=["};
        for (std::size_t index = 0; index < plan.outputs.size(); ++index) {
            if (index != 0) {
                header += ", ";
            }
            header += plan.outputs[index].name;
            header.push_back(':');
            header += type_name(plan.outputs[index].type);
        }
        header.push_back(']');
        lines.push_back(std::move(header));
        append_node(*plan.root, 1U, names, lines);
        return lines;
    }

    void append_node(
        const compiler::PlanNode& node,
        const std::size_t level,
        const SlotNames& names,
        std::vector<std::string>& lines) const {
        const std::string indent(level * kIndentWidth, ' ');
        std::visit(
            [&](const auto& typed) {
                using Node = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<Node, compiler::SeqScanNode>) {
                    lines.push_back(indent + "SeqScan " + table_label(catalog_, typed.table_id));
                    std::string mapping{indent + "  columns=["};
                    for (std::size_t index = 0; index < typed.columns.size(); ++index) {
                        if (index != 0) {
                            mapping += ", ";
                        }
                        mapping += column_label(
                            catalog_, typed.table_id, typed.columns[index].column_id);
                        mapping += " -> ";
                        mapping += slot_label(typed.columns[index].output_slot);
                    }
                    mapping.push_back(']');
                    lines.push_back(std::move(mapping));
                } else if constexpr (std::is_same_v<Node, compiler::FilterNode>) {
                    lines.push_back(
                        indent + "Filter " + expression_text(typed.predicate, names));
                    append_node(*typed.child, level + 1U, names, lines);
                } else if constexpr (std::is_same_v<Node, compiler::JoinNode>) {
                    lines.push_back(
                        indent + "Join kind=" +
                        std::string{join_kind_name(typed.kind)} + " condition=" +
                        expression_text(typed.condition, names));
                    lines.push_back(indent + "  left:");
                    append_node(*typed.left, level + 2U, names, lines);
                    lines.push_back(indent + "  right:");
                    append_node(*typed.right, level + 2U, names, lines);
                } else if constexpr (std::is_same_v<Node, compiler::AggregateNode>) {
                    std::string text{indent + "Aggregate group=["};
                    for (std::size_t index = 0; index < typed.group_keys.size(); ++index) {
                        if (index != 0) {
                            text += ", ";
                        }
                        text += slot_label(typed.group_keys[index]);
                    }
                    text += "] calls=[";
                    for (std::size_t index = 0; index < typed.aggregates.size(); ++index) {
                        if (index != 0) {
                            text += ", ";
                        }
                        const compiler::AggregateCall& call = typed.aggregates[index];
                        text += aggregate_name(call.kind);
                        text.push_back('(');
                        text += call.input_slot.has_value()
                            ? slot_ref(*call.input_slot, names)
                            : std::string{"*"};
                        text += ") -> ";
                        text += slot_label(call.output_slot);
                        text.push_back(' ');
                        text += type_name(call.output_type);
                    }
                    text.push_back(']');
                    lines.push_back(std::move(text));
                    append_node(*typed.child, level + 1U, names, lines);
                } else if constexpr (std::is_same_v<Node, compiler::SortNode>) {
                    std::string text{indent + "Sort keys=["};
                    for (std::size_t index = 0; index < typed.keys.size(); ++index) {
                        if (index != 0) {
                            text += ", ";
                        }
                        text += slot_label(typed.keys[index].slot_id);
                        text += typed.keys[index].direction == compiler::SortDirection::kAsc
                            ? " ASC"
                            : " DESC";
                    }
                    text.push_back(']');
                    lines.push_back(std::move(text));
                    append_node(*typed.child, level + 1U, names, lines);
                } else {
                    std::string text{indent + "Project ["};
                    for (std::size_t index = 0; index < typed.outputs.size(); ++index) {
                        if (index != 0) {
                            text += ", ";
                        }
                        text += slot_ref(typed.outputs[index], names);
                    }
                    text.push_back(']');
                    lines.push_back(std::move(text));
                    append_node(*typed.child, level + 1U, names, lines);
                }
            },
            node.kind);
    }

    std::span<const TableMeta> catalog_;
};

}  // namespace

std::variant<QueryResult, Error> render_plan_result(
    const compiler::Plan& plan,
    const std::span<const TableMeta> catalog) {
    if (const std::optional<std::string> violation = validate_plan(plan);
        violation.has_value()) {
        return make_error(
            ErrorKind::kInternal,
            "compiler returned an invalid plan: " + *violation);
    }

    const PlanRenderer renderer{catalog};
    std::vector<std::string> lines = renderer.statement(plan);

    QueryResult result;
    result.columns.push_back(ColumnHeader{"plan", Type::kVarchar});
    result.rows.reserve(lines.size());
    for (std::string& line : lines) {
        result.rows.push_back(Row{Value{std::move(line)}});
    }
    return result;
}

}  // namespace tinydbms::core::internal
