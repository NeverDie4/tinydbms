#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "plan_formatter.hpp"
#include "tinydbms/compiler.hpp"

namespace {

using namespace tinydbms;
using namespace tinydbms::compiler;
using tinydbms::compiler::internal::format_plan;

constexpr std::uint32_t kSeed = 20260909U;
constexpr std::size_t kFuzzCount = 2000;
constexpr int kMaxExpressionDepth = 3;

enum class StatementKind {
    kCreate,
    kInsert,
    kSelect,
    kDelete
};

struct GeneratedSql {
    explicit GeneratedSql(StatementKind kind_) : kind{kind_} {}

    StatementKind kind;
    std::string sql;
    TableId table_id{0U};
    std::string table_name;
    std::vector<ColumnMeta> create_columns;
    std::vector<ColumnId> insert_columns;
    std::vector<std::vector<Value>> insert_rows;
    std::vector<ColumnId> select_outputs;
};

struct FuzzCounts {
    std::size_t create{0};
    std::size_t insert{0};
    std::size_t select{0};
    std::size_t deletion{0};
};

[[nodiscard]] int random_int(std::mt19937& engine, int minimum, int maximum) {
    return std::uniform_int_distribution<int>{minimum, maximum}(engine);
}

[[nodiscard]] std::size_t random_index(std::mt19937& engine, std::size_t size) {
    return std::uniform_int_distribution<std::size_t>{0, size - 1}(engine);
}

[[nodiscard]] bool random_chance(std::mt19937& engine, int percent) {
    return random_int(engine, 0, 99) < percent;
}

[[nodiscard]] std::int32_t generate_integer(std::mt19937& engine) {
    switch (random_int(engine, 0, 4)) {
        case 0:
            return 0;
        case 1:
            return 1;
        case 2:
            return std::numeric_limits<std::int32_t>::max();
        case 3:
            return std::numeric_limits<std::int32_t>::max() -
                   std::uniform_int_distribution<std::int32_t>{0, 1000}(engine);
        default:
            return std::uniform_int_distribution<std::int32_t>{0, 100000}(engine);
    }
}

[[nodiscard]] std::string generate_string_value(std::mt19937& engine) {
    static const std::vector<std::string> special_values{
        "", ";", ",", "--", "/*", "*/", "Tom's", "mix; -- /* */"
    };
    if (random_chance(engine, 30)) {
        return special_values[random_index(engine, special_values.size())];
    }

    constexpr std::string_view characters =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
    const int length = random_int(engine, 0, 30);
    std::string value;
    value.reserve(static_cast<std::size_t>(length));
    for (int index = 0; index < length; ++index) {
        value += characters[random_index(engine, characters.size())];
    }
    return value;
}

[[nodiscard]] std::string sql_string_literal(std::string_view value) {
    std::string literal{"'"};
    for (const char character : value) {
        if (character == '\'') {
            literal += "''";
        } else {
            literal += character;
        }
    }
    literal += '\'';
    return literal;
}

[[nodiscard]] std::vector<ColumnId> columns_of_type(const TableMeta& table, Type type) {
    std::vector<ColumnId> columns;
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
        if (table.columns[index].type == type) {
            columns.push_back(static_cast<ColumnId>(index));
        }
    }
    return columns;
}

[[nodiscard]] std::string column_operand(
    std::mt19937& engine,
    const TableMeta& table,
    Type type) {
    const std::vector<ColumnId> columns = columns_of_type(table, type);
    return table.columns[columns[random_index(engine, columns.size())]].name;
}

[[nodiscard]] std::string literal_operand(std::mt19937& engine, Type type) {
    if (type == Type::kInt) {
        return std::to_string(generate_integer(engine));
    }
    return sql_string_literal(generate_string_value(engine));
}

[[nodiscard]] std::string generate_comparison(
    std::mt19937& engine,
    const TableMeta& table) {
    const Type type = random_chance(engine, 65) ? Type::kInt : Type::kVarchar;
    static const std::vector<std::string_view> int_operators{
        "=", "!=", "<", "<=", ">", ">="
    };
    static const std::vector<std::string_view> varchar_operators{"=", "!="};
    const auto& operators = type == Type::kInt ? int_operators : varchar_operators;

    std::string lhs;
    std::string rhs;
    switch (random_int(engine, 0, 3)) {
        case 0:
            lhs = column_operand(engine, table, type);
            rhs = literal_operand(engine, type);
            break;
        case 1:
            lhs = literal_operand(engine, type);
            rhs = column_operand(engine, table, type);
            break;
        case 2:
            lhs = column_operand(engine, table, type);
            rhs = column_operand(engine, table, type);
            break;
        default:
            lhs = literal_operand(engine, type);
            rhs = literal_operand(engine, type);
            break;
    }
    return lhs + ' ' + std::string{operators[random_index(engine, operators.size())]} +
           ' ' + rhs;
}

[[nodiscard]] std::string generate_bool_expression(
    std::mt19937& engine,
    const TableMeta& table,
    int depth) {
    if (depth == 0) {
        return generate_comparison(engine, table);
    }

    switch (random_int(engine, 0, 3)) {
        case 0:
            return generate_comparison(engine, table);
        case 1:
            return "NOT (" + generate_bool_expression(engine, table, depth - 1) + ')';
        case 2:
            return '(' + generate_bool_expression(engine, table, depth - 1) +
                   " AND " + generate_bool_expression(engine, table, depth - 1) + ')';
        default:
            return '(' + generate_bool_expression(engine, table, depth - 1) +
                   " OR " + generate_bool_expression(engine, table, depth - 1) + ')';
    }
}

[[nodiscard]] GeneratedSql generate_create(std::mt19937& engine, std::size_t iteration) {
    GeneratedSql generated{StatementKind::kCreate};
    generated.table_name = "fuzz_t_" + std::to_string(iteration) + '_' +
                           std::to_string(random_int(engine, 0, 9999));
    const int column_count = random_int(engine, 1, 5);
    generated.sql = "CREATE TABLE " + generated.table_name + '(';
    for (int index = 0; index < column_count; ++index) {
        if (index != 0) {
            generated.sql += ',';
        }
        const std::string name = "c" + std::to_string(index) + '_' +
                                 std::to_string(random_int(engine, 0, 9999));
        const Type type = random_chance(engine, 50) ? Type::kInt : Type::kVarchar;
        generated.create_columns.push_back(ColumnMeta{name, type});
        generated.sql += name + ' ' + std::string{type == Type::kInt ? "INT" : "VARCHAR"};
    }
    generated.sql += ");";
    return generated;
}

[[nodiscard]] GeneratedSql generate_insert(
    std::mt19937& engine,
    const std::vector<TableMeta>& tables) {
    const TableMeta& table = tables[random_index(engine, tables.size())];
    GeneratedSql generated{StatementKind::kInsert};
    generated.table_id = table.table_id;
    generated.sql = "INSERT INTO " + table.table_name;

    std::vector<ColumnId> value_order;
    value_order.reserve(table.columns.size());
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
        value_order.push_back(static_cast<ColumnId>(index));
    }
    const bool explicit_columns = random_chance(engine, 50);
    if (explicit_columns) {
        std::shuffle(value_order.begin(), value_order.end(), engine);
        generated.insert_columns = value_order;
        generated.sql += '(';
        for (std::size_t index = 0; index < value_order.size(); ++index) {
            if (index != 0) {
                generated.sql += ',';
            }
            generated.sql += table.columns[value_order[index]].name;
        }
        generated.sql += ')';
    }

    generated.sql += " VALUES ";
    const int row_count = random_int(engine, 1, 4);
    for (int row_index = 0; row_index < row_count; ++row_index) {
        if (row_index != 0) {
            generated.sql += ',';
        }
        generated.sql += '(';
        std::vector<Value> row;
        row.reserve(value_order.size());
        for (std::size_t value_index = 0; value_index < value_order.size(); ++value_index) {
            if (value_index != 0) {
                generated.sql += ',';
            }
            const Type type = table.columns[value_order[value_index]].type;
            if (type == Type::kInt) {
                const std::int32_t value = generate_integer(engine);
                generated.sql += std::to_string(value);
                row.push_back(Value{value});
            } else {
                std::string value = generate_string_value(engine);
                generated.sql += sql_string_literal(value);
                row.push_back(Value{std::move(value)});
            }
        }
        generated.sql += ')';
        generated.insert_rows.push_back(std::move(row));
    }
    generated.sql += ';';
    return generated;
}

[[nodiscard]] GeneratedSql generate_select(
    std::mt19937& engine,
    const std::vector<TableMeta>& tables) {
    const TableMeta& table = tables[random_index(engine, tables.size())];
    GeneratedSql generated{StatementKind::kSelect};
    generated.table_id = table.table_id;
    generated.sql = "SELECT ";
    if (random_chance(engine, 35)) {
        generated.sql += '*';
        for (std::size_t index = 0; index < table.columns.size(); ++index) {
            generated.select_outputs.push_back(static_cast<ColumnId>(index));
        }
    } else {
        const int output_count = random_int(
            engine,
            1,
            static_cast<int>(table.columns.size()) + 2);
        for (int index = 0; index < output_count; ++index) {
            if (index != 0) {
                generated.sql += ',';
            }
            const ColumnId column = static_cast<ColumnId>(
                random_index(engine, table.columns.size()));
            generated.select_outputs.push_back(column);
            generated.sql += table.columns[column].name;
        }
    }
    generated.sql += " FROM " + table.table_name;
    if (random_chance(engine, 70)) {
        generated.sql += " WHERE " +
                         generate_bool_expression(engine, table, kMaxExpressionDepth);
    }
    generated.sql += ';';
    return generated;
}

[[nodiscard]] GeneratedSql generate_delete(
    std::mt19937& engine,
    const std::vector<TableMeta>& tables) {
    const TableMeta& table = tables[random_index(engine, tables.size())];
    GeneratedSql generated{StatementKind::kDelete};
    generated.table_id = table.table_id;
    generated.sql = "DELETE FROM " + table.table_name;
    if (random_chance(engine, 70)) {
        generated.sql += " WHERE " +
                         generate_bool_expression(engine, table, kMaxExpressionDepth);
    }
    generated.sql += ';';
    return generated;
}

[[nodiscard]] std::vector<GeneratedSql> generate_sequence(
    std::uint32_t seed,
    std::size_t count,
    const std::vector<TableMeta>& tables) {
    std::mt19937 engine{seed};
    std::vector<GeneratedSql> sequence;
    sequence.reserve(count);
    for (std::size_t iteration = 0; iteration < count; ++iteration) {
        const int category = random_int(engine, 0, 99);
        if (category < 15) {
            sequence.push_back(generate_create(engine, iteration));
        } else if (category < 40) {
            sequence.push_back(generate_insert(engine, tables));
        } else if (category < 80) {
            sequence.push_back(generate_select(engine, tables));
        } else {
            sequence.push_back(generate_delete(engine, tables));
        }
    }
    return sequence;
}

[[nodiscard]] bool values_equal(const Value& lhs, const Value& rhs) {
    return lhs.data == rhs.data;
}

[[nodiscard]] bool validate_expression(const Expr& expression, std::string& reason) {
    if (const auto* binary = std::get_if<Binary>(&expression.kind)) {
        if (binary->lhs == nullptr || binary->rhs == nullptr) {
            reason = "Binary child is null";
            return false;
        }
        return validate_expression(*binary->lhs, reason) &&
               validate_expression(*binary->rhs, reason);
    }
    if (const auto* unary = std::get_if<Unary>(&expression.kind)) {
        if (unary->operand == nullptr) {
            reason = "Unary operand is null";
            return false;
        }
        return validate_expression(*unary->operand, reason);
    }
    return true;
}

[[nodiscard]] bool validate_plan_node(
    const PlanNode& node,
    TableId expected_table,
    bool& found_scan,
    std::string& reason) {
    if (const auto* scan = std::get_if<SeqScanNode>(&node.kind)) {
        if (scan->table_id != expected_table) {
            reason = "SeqScan TableId mismatch";
            return false;
        }
        found_scan = true;
        return true;
    }
    if (const auto* filter = std::get_if<FilterNode>(&node.kind)) {
        if (filter->child == nullptr) {
            reason = "Filter child is null";
            return false;
        }
        return validate_expression(filter->predicate, reason) &&
               validate_plan_node(*filter->child, expected_table, found_scan, reason);
    }
    const auto& project = std::get<ProjectNode>(node.kind);
    if (project.child == nullptr) {
        reason = "Project child is null";
        return false;
    }
    return validate_plan_node(*project.child, expected_table, found_scan, reason);
}

[[nodiscard]] bool validate_generated_plan(
    const GeneratedSql& generated,
    const Plan& plan,
    std::string& reason) {
    if (generated.kind == StatementKind::kCreate) {
        const auto* create = std::get_if<CreateTablePlan>(&plan.kind);
        if (create == nullptr || create->table_name != generated.table_name ||
            create->columns.size() != generated.create_columns.size()) {
            reason = "CreateTablePlan header mismatch";
            return false;
        }
        for (std::size_t index = 0; index < create->columns.size(); ++index) {
            if (create->columns[index].name != generated.create_columns[index].name ||
                create->columns[index].type != generated.create_columns[index].type) {
                reason = "CreateTablePlan column mismatch";
                return false;
            }
        }
        return true;
    }

    if (generated.kind == StatementKind::kInsert) {
        const auto* insert = std::get_if<InsertPlan>(&plan.kind);
        if (insert == nullptr || insert->table_id != generated.table_id ||
            insert->columns != generated.insert_columns ||
            insert->rows.size() != generated.insert_rows.size()) {
            reason = "InsertPlan header mismatch";
            return false;
        }
        for (std::size_t row = 0; row < insert->rows.size(); ++row) {
            if (insert->rows[row].size() != generated.insert_rows[row].size()) {
                reason = "InsertPlan row width mismatch";
                return false;
            }
            for (std::size_t column = 0; column < insert->rows[row].size(); ++column) {
                if (!values_equal(insert->rows[row][column], generated.insert_rows[row][column])) {
                    reason = "InsertPlan value order mismatch";
                    return false;
                }
            }
        }
        return true;
    }

    if (generated.kind == StatementKind::kSelect) {
        const auto* query = std::get_if<QueryPlan>(&plan.kind);
        if (query == nullptr || query->root == nullptr) {
            reason = "QueryPlan root is null or kind is wrong";
            return false;
        }
        const auto* project = std::get_if<ProjectNode>(&query->root->kind);
        if (project == nullptr || project->outputs != generated.select_outputs) {
            reason = "QueryPlan Project outputs mismatch";
            return false;
        }
        bool found_scan = false;
        if (!validate_plan_node(*query->root, generated.table_id, found_scan, reason)) {
            return false;
        }
        if (!found_scan) {
            reason = "QueryPlan has no SeqScan";
            return false;
        }
        return true;
    }

    const auto* deletion = std::get_if<DeletePlan>(&plan.kind);
    if (deletion == nullptr || deletion->table_id != generated.table_id) {
        reason = "DeletePlan kind or TableId mismatch";
        return false;
    }
    return !deletion->predicate.has_value() ||
           validate_expression(*deletion->predicate, reason);
}

[[nodiscard]] bool catalogs_equal(
    const std::vector<TableMeta>& lhs,
    const std::vector<TableMeta>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t table = 0; table < lhs.size(); ++table) {
        if (lhs[table].table_id != rhs[table].table_id ||
            lhs[table].table_name != rhs[table].table_name ||
            lhs[table].columns.size() != rhs[table].columns.size()) {
            return false;
        }
        for (std::size_t column = 0; column < lhs[table].columns.size(); ++column) {
            if (lhs[table].columns[column].name != rhs[table].columns[column].name ||
                lhs[table].columns[column].type != rhs[table].columns[column].type) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] std::string_view error_kind_name(CompileErrorKind kind) {
    switch (kind) {
        case CompileErrorKind::kLex:
            return "Lex";
        case CompileErrorKind::kSyntax:
            return "Syntax";
        case CompileErrorKind::kSemantic:
            return "Semantic";
    }
    return "Unknown";
}

void print_context(std::size_t iteration, const GeneratedSql& generated) {
    std::cerr << "seed=" << kSeed << " iteration=" << iteration
              << "\nSQL: " << generated.sql << '\n';
}

[[nodiscard]] FuzzCounts count_statements(const std::vector<GeneratedSql>& sequence) {
    FuzzCounts counts;
    for (const GeneratedSql& generated : sequence) {
        switch (generated.kind) {
            case StatementKind::kCreate:
                ++counts.create;
                break;
            case StatementKind::kInsert:
                ++counts.insert;
                break;
            case StatementKind::kSelect:
                ++counts.select;
                break;
            case StatementKind::kDelete:
                ++counts.deletion;
                break;
        }
    }
    return counts;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::vector<TableMeta> tables{
        TableMeta{7U, "student", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"name", Type::kVarchar},
            ColumnMeta{"age", Type::kInt},
        }},
        TableMeta{42U, "course", {
            ColumnMeta{"course_id", Type::kInt},
            ColumnMeta{"title", Type::kVarchar},
            ColumnMeta{"credits", Type::kInt},
        }},
        TableMeta{100U, "account", {
            ColumnMeta{"account_id", Type::kInt},
            ColumnMeta{"owner", Type::kVarchar},
            ColumnMeta{"balance", Type::kInt},
        }},
    };
    const std::vector<TableMeta> catalog_before = tables;
    const CatalogView catalog{std::span<const TableMeta>{tables}};

    const std::vector<GeneratedSql> sequence = generate_sequence(kSeed, kFuzzCount, tables);
    const std::vector<GeneratedSql> replay = generate_sequence(kSeed, 100, tables);
    for (std::size_t index = 0; index < replay.size(); ++index) {
        if (sequence[index].sql != replay[index].sql) {
            std::cerr << "reproducibility failure: seed=" << kSeed
                      << " index=" << index << '\n';
            return 1;
        }
    }

    const FuzzCounts counts = count_statements(sequence);
    if (counts.create == 0 || counts.insert == 0 || counts.select == 0 ||
        counts.deletion == 0) {
        std::cerr << "seed=" << kSeed << " did not cover all statement kinds\n";
        return 1;
    }

    for (std::size_t iteration = 0; iteration < sequence.size(); ++iteration) {
        const GeneratedSql& generated = sequence[iteration];
        const CompileResult result = compile(CompileRequest{generated.sql, catalog});
        if (const auto* error = std::get_if<CompileError>(&result.outcome)) {
            print_context(iteration, generated);
            std::cerr << "unexpected " << error_kind_name(error->kind) << " error at "
                      << error->location.line << ':' << error->location.column
                      << ": " << error->message << '\n';
            return 1;
        }

        const Plan& plan = std::get<Plan>(result.outcome);
        std::string reason;
        if (!validate_generated_plan(generated, plan, reason)) {
            print_context(iteration, generated);
            std::cerr << "invalid Plan: " << reason << "\n" << format_plan(plan);
            return 1;
        }

        if (iteration % 211 == 0) {
            const CompileResult repeated = compile(CompileRequest{generated.sql, catalog});
            if (const auto* error = std::get_if<CompileError>(&repeated.outcome)) {
                print_context(iteration, generated);
                std::cerr << "repeated compile produced " << error_kind_name(error->kind)
                          << " error at " << error->location.line << ':'
                          << error->location.column << ": " << error->message << '\n';
                return 1;
            }
            const auto* repeated_plan = std::get_if<Plan>(&repeated.outcome);
            if (repeated_plan == nullptr || format_plan(plan) != format_plan(*repeated_plan)) {
                print_context(iteration, generated);
                std::cerr << "compile determinism failure\n";
                return 1;
            }
        }
    }

    if (!catalogs_equal(tables, catalog_before)) {
        std::cerr << "seed=" << kSeed << ": CatalogView backing data changed\n";
        return 1;
    }

    if (argc == 2 && std::string_view{argv[1]} == "--stats") {
        std::cout << "seed=" << kSeed << " total=" << sequence.size()
                  << " create=" << counts.create
                  << " insert=" << counts.insert
                  << " select=" << counts.select
                  << " delete=" << counts.deletion << '\n';
    }
    return 0;
}
