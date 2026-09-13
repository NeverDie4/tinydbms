#include <algorithm>
#include <array>
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
constexpr std::uint32_t kBigIntSeed = 20260911U;
constexpr std::size_t kBigIntFuzzCount = 500;
constexpr std::uint32_t kDoubleSeed = 20260913U;
constexpr std::size_t kDoubleFuzzCount = 500;
constexpr std::uint32_t kBooleanSeed = 20260915U;
constexpr std::size_t kBooleanFuzzCount = 500;
constexpr std::uint32_t kNullSeed = 20260917U;
constexpr std::size_t kNullFuzzCount = 600;
constexpr std::uint32_t kUpdateSeed = 20260919U;
constexpr std::size_t kUpdateFuzzCount = 700;
constexpr std::uint32_t kOrderBySeed = 20260921U;
constexpr std::size_t kOrderByFuzzCount = 800;
constexpr std::uint32_t kJoinSeed = 20260923U;
constexpr std::size_t kJoinFuzzCount = 900;
constexpr std::uint32_t kAggregateSeed = 20260925U;
constexpr std::size_t kAggregateFuzzCount = 1000;
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

[[nodiscard]] std::int64_t generate_bigint_literal(std::mt19937& engine) {
    static constexpr std::array<std::int64_t, 8> boundaries{
        0,
        1,
        std::numeric_limits<std::int32_t>::max() - 1LL,
        std::numeric_limits<std::int32_t>::max(),
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1LL,
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 2LL,
        std::numeric_limits<std::int64_t>::max() - 1LL,
        std::numeric_limits<std::int64_t>::max(),
    };
    if (random_chance(engine, 60)) {
        return boundaries[random_index(engine, boundaries.size())];
    }
    return std::uniform_int_distribution<std::int64_t>{
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1LL,
        std::numeric_limits<std::int64_t>::max()}(engine);
}

[[nodiscard]] std::string generate_bigint_sql(
    std::mt19937& engine,
    std::size_t iteration) {
    const std::string literal = std::to_string(generate_bigint_literal(engine));
    switch (iteration % 5U) {
        case 0:
            return "INSERT INTO bigint_values VALUES (" + literal + ",1);";
        case 1:
            return "SELECT big_id FROM bigint_values WHERE big_id >= " + literal + ";";
        case 2:
            return "SELECT small_id FROM bigint_values WHERE small_id < " + literal + ";";
        case 3:
            return "DELETE FROM bigint_values WHERE " + literal + " != big_id;";
        default:
            return "SELECT big_id,small_id FROM bigint_values;";
    }
}

[[nodiscard]] GeneratedSql generate_double_sql(
    std::mt19937& engine,
    std::size_t iteration) {
    static constexpr std::array<std::pair<std::string_view, double>, 6> literals{
        std::pair{"0.0", 0.0},
        std::pair{"1.0", 1.0},
        std::pair{"12.5", 12.5},
        std::pair{"2147483647.0", 2147483647.0},
        std::pair{"2147483648.0", 2147483648.0},
        std::pair{"9007199254740992.0", 9007199254740992.0},
    };
    const auto [literal, value] = literals[random_index(engine, literals.size())];
    switch (iteration % 8U) {
        case 0: {
            GeneratedSql generated{StatementKind::kCreate};
            generated.table_name = "double_fuzz_" + std::to_string(iteration);
            generated.create_columns = {{"value", Type::kDouble, true}};
            generated.sql = "CREATE TABLE " + generated.table_name + "(value DOUBLE);";
            return generated;
        }
        case 1: {
            GeneratedSql generated{StatementKind::kInsert};
            generated.table_id = 701U;
            generated.insert_rows = {{Value{std::int32_t{1}},
                                      Value{std::int64_t{2147483648LL}}, Value{value}}};
            generated.sql = "INSERT INTO double_values VALUES (1,2147483648," +
                std::string{literal} + ");";
            return generated;
        }
        case 2: {
            GeneratedSql generated{StatementKind::kInsert};
            generated.table_id = 701U;
            generated.insert_rows = {{Value{std::int32_t{1}},
                                      Value{std::int64_t{2147483648LL}}, Value{1.0}}};
            generated.sql = "INSERT INTO double_values VALUES (1,2147483648,1);";
            return generated;
        }
        case 3: {
            GeneratedSql generated{StatementKind::kInsert};
            generated.table_id = 701U;
            generated.insert_rows = {{Value{std::int32_t{1}},
                                      Value{std::int64_t{2147483648LL}},
                                      Value{2147483648.0}}};
            generated.sql =
                "INSERT INTO double_values VALUES (1,2147483648,2147483648);";
            return generated;
        }
        case 4:
        case 5:
        case 6: {
            static constexpr std::array<std::string_view, 3> names{
                "small_id", "big_id", "measure"};
            GeneratedSql generated{StatementKind::kSelect};
            generated.table_id = 701U;
            const ColumnId output = static_cast<ColumnId>(iteration % 3U);
            generated.select_outputs = {output};
            const std::string name{names[static_cast<std::size_t>(output)]};
            generated.sql = "SELECT " + name + " FROM double_values WHERE " + name +
                " >= " + std::string{literal} + ";";
            return generated;
        }
        default: {
            GeneratedSql generated{StatementKind::kDelete};
            generated.table_id = 701U;
            generated.sql = "DELETE FROM double_values WHERE " + std::string{literal} +
                " != measure;";
            return generated;
        }
    }
}

[[nodiscard]] std::string boolean_literal(std::mt19937& engine, bool value) {
    static constexpr std::array<std::string_view, 3> true_spellings{
        "TRUE", "true", "TrUe"};
    static constexpr std::array<std::string_view, 3> false_spellings{
        "FALSE", "false", "FaLsE"};
    const auto& spellings = value ? true_spellings : false_spellings;
    return std::string{spellings[random_index(engine, spellings.size())]};
}

[[nodiscard]] GeneratedSql generate_boolean_sql(
    std::mt19937& engine,
    std::size_t iteration) {
    const bool value = random_chance(engine, 50);
    const std::string literal = boolean_literal(engine, value);
    switch (iteration % 12U) {
        case 0: {
            GeneratedSql generated{StatementKind::kCreate};
            generated.table_name = "boolean_fuzz_" + std::to_string(iteration);
            generated.create_columns = {{"active", Type::kBoolean, true}};
            generated.sql = "CREATE TABLE " + generated.table_name + "(active Boolean);";
            return generated;
        }
        case 1: {
            GeneratedSql generated{StatementKind::kInsert};
            generated.table_id = 702U;
            generated.insert_rows = {{Value{std::int32_t{1}}, Value{value}}};
            generated.sql = "INSERT INTO boolean_values VALUES (1," + literal + ");";
            return generated;
        }
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
        case 9: {
            static constexpr std::array<std::string_view, 8> predicates{
                "active", "NOT active", "TRUE", "FALSE",
                "active = TRUE", "active != FALSE",
                "active AND TRUE", "active OR FALSE"};
            GeneratedSql generated{StatementKind::kSelect};
            generated.table_id = 702U;
            generated.select_outputs = {1U};
            generated.sql = "SELECT active FROM boolean_values WHERE " +
                std::string{predicates[(iteration - 2U) % predicates.size()]} + ";";
            return generated;
        }
        case 10: {
            GeneratedSql generated{StatementKind::kDelete};
            generated.table_id = 702U;
            generated.sql = "DELETE FROM boolean_values WHERE active;";
            return generated;
        }
        default: {
            GeneratedSql generated{StatementKind::kDelete};
            generated.table_id = 702U;
            generated.sql = "DELETE FROM boolean_values WHERE NOT active;";
            return generated;
        }
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
        generated.create_columns.push_back(ColumnMeta{name, type, true});
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

[[nodiscard]] const ScanColumn* scan_column_for_slot(
    const std::vector<ScanColumn>& columns,
    SlotId slot_id) {
    for (const ScanColumn& column : columns) {
        if (column.output_slot == slot_id) {
            return &column;
        }
    }
    return nullptr;
}

[[nodiscard]] const ScanColumn* scan_column_for_column(
    const std::vector<ScanColumn>& columns,
    ColumnId column_id) {
    for (const ScanColumn& column : columns) {
        if (column.column_id == column_id) {
            return &column;
        }
    }
    return nullptr;
}

[[nodiscard]] const TableMeta* table_by_id(
    const std::vector<TableMeta>& tables,
    TableId table_id) {
    for (const TableMeta& table : tables) {
        if (table.table_id == table_id) {
            return &table;
        }
    }
    return nullptr;
}

[[nodiscard]] bool validate_scan_columns(
    const std::vector<ScanColumn>& columns,
    const TableMeta& table,
    std::string& reason) {
    if (columns.size() != table.columns.size()) {
        reason = "scan mapping does not cover the full schema";
        return false;
    }
    for (std::size_t index = 0; index < columns.size(); ++index) {
        const ScanColumn& column = columns[index];
        if (static_cast<std::size_t>(column.column_id) >= table.columns.size()) {
            reason = "scan mapping ColumnId is outside the table schema";
            return false;
        }
        if (column.column_id != static_cast<ColumnId>(index) ||
            column.output_slot != static_cast<SlotId>(index)) {
            reason = "scan mapping is not deterministic schema order";
            return false;
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (columns[previous].column_id == column.column_id ||
                columns[previous].output_slot == column.output_slot) {
                reason = "scan mapping contains a duplicate ColumnId or SlotId";
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool validate_expression(
    const Expr& expression,
    const std::vector<ScanColumn>& columns,
    std::string& reason) {
    if (const auto* column = std::get_if<ColumnRef>(&expression.kind)) {
        if (scan_column_for_slot(columns, column->slot_id) == nullptr) {
            reason = "ColumnRef SlotId is not provided by the scan mapping";
            return false;
        }
        return true;
    }
    if (const auto* binary = std::get_if<Binary>(&expression.kind)) {
        if (binary->lhs == nullptr || binary->rhs == nullptr) {
            reason = "Binary child is null";
            return false;
        }
        return validate_expression(*binary->lhs, columns, reason) &&
               validate_expression(*binary->rhs, columns, reason);
    }
    if (const auto* unary = std::get_if<Unary>(&expression.kind)) {
        if (unary->operand == nullptr) {
            reason = "Unary operand is null";
            return false;
        }
        return validate_expression(*unary->operand, columns, reason);
    }
    if (const auto* null_test = std::get_if<NullTest>(&expression.kind)) {
        if (null_test->operand == nullptr) {
            reason = "NullTest operand is null";
            return false;
        }
        return validate_expression(*null_test->operand, columns, reason);
    }
    return true;
}

[[nodiscard]] bool value_matches(const Value& value,const ColumnMeta& column) {
    if(std::holds_alternative<std::monostate>(value.data))return column.nullable;
    switch(column.type) {
        case Type::kInt:return std::holds_alternative<std::int32_t>(value.data);
        case Type::kBigInt:return std::holds_alternative<std::int64_t>(value.data);
        case Type::kDouble:return std::holds_alternative<double>(value.data);
        case Type::kBoolean:return std::holds_alternative<bool>(value.data);
        case Type::kVarchar:return std::holds_alternative<std::string>(value.data);
    }
    return false;
}

[[nodiscard]] bool validate_update_plan(
    const Plan& plan,const TableMeta& table,std::string& reason) {
    const auto* update=std::get_if<UpdatePlan>(&plan.kind);
    if(update==nullptr || update->table_id!=table.table_id || update->assignments.empty()) {
        reason="UpdatePlan header mismatch";return false;
    }
    if(!validate_scan_columns(update->input_columns,table,reason))return false;
    if(update->predicate.has_value() &&
       !validate_expression(*update->predicate,update->input_columns,reason))return false;
    for(std::size_t index=0;index<update->assignments.size();++index) {
        const UpdateAssignment& assignment=update->assignments[index];
        const std::size_t column=static_cast<std::size_t>(assignment.column_id);
        if(column>=table.columns.size() || !value_matches(assignment.value,table.columns[column])) {
            reason="UPDATE assignment type or ColumnId mismatch";return false;
        }
        for(std::size_t previous=0;previous<index;++previous) {
            if(update->assignments[previous].column_id==assignment.column_id) {
                reason="UPDATE assignment target is duplicated";return false;
            }
        }
    }
    return true;
}

[[nodiscard]] std::string generate_update_sql(std::mt19937& engine,std::size_t iteration) {
    const std::string integer=std::to_string(generate_integer(engine));
    switch(iteration%12U) {
        case 0:return "UPDATE update_values SET id="+integer+";";
        case 1:return "UPDATE update_values SET big_id=1,measure=2;";
        case 2:return "UPDATE update_values SET big_id=2147483648 WHERE id>="+integer+";";
        case 3:return "UPDATE update_values SET measure=2147483648 WHERE active;";
        case 4:return "UPDATE update_values SET active=TRUE WHERE note IS NULL;";
        case 5:return "UPDATE update_values SET active=FALSE WHERE note IS NOT NULL;";
        case 6:return "UPDATE update_values SET active=NULL;";
        case 7:return "UPDATE update_values SET note=NULL WHERE active OR NULL;";
        case 8:return "UPDATE update_values SET note='text';";
        case 9:return "UPDATE update_values SET measure=12.5 WHERE big_id>1;";
        case 10:return "UPDATE update_values SET note='',active=FALSE WHERE id=1;";
        default:return "UPDATE update_values SET id=1,big_id=2,measure=3.0,active=TRUE,note='all';";
    }
}

[[nodiscard]] const SeqScanNode* find_scan(const PlanNode& node) {
    if (const auto* scan = std::get_if<SeqScanNode>(&node.kind)) {
        return scan;
    }
    if (const auto* filter = std::get_if<FilterNode>(&node.kind)) {
        return filter->child == nullptr ? nullptr : find_scan(*filter->child);
    }
    if (const auto* sort = std::get_if<SortNode>(&node.kind)) {
        return sort->child == nullptr ? nullptr : find_scan(*sort->child);
    }
    const auto& project = std::get<ProjectNode>(node.kind);
    return project.child == nullptr ? nullptr : find_scan(*project.child);
}

[[nodiscard]] bool validate_plan_expressions(
    const PlanNode& node,
    const std::vector<ScanColumn>& columns,
    std::string& reason) {
    if (std::holds_alternative<SeqScanNode>(node.kind)) {
        return true;
    }
    if (const auto* filter = std::get_if<FilterNode>(&node.kind)) {
        if (filter->child == nullptr) {
            reason = "Filter child is null";
            return false;
        }
        return validate_expression(filter->predicate, columns, reason) &&
               validate_plan_expressions(*filter->child, columns, reason);
    }
    if (const auto* sort = std::get_if<SortNode>(&node.kind)) {
        if (sort->child == nullptr || sort->keys.empty()) {
            reason = "Sort child is null or keys are empty";
            return false;
        }
        for (const SortKey& key : sort->keys) {
            if (scan_column_for_slot(columns, key.slot_id) == nullptr) {
                reason = "Sort key SlotId is not produced by scan";
                return false;
            }
        }
        return validate_plan_expressions(*sort->child, columns, reason);
    }
    const auto& project = std::get<ProjectNode>(node.kind);
    if (project.child == nullptr) {
        reason = "Project child is null";
        return false;
    }
    return validate_plan_expressions(*project.child, columns, reason);
}

[[nodiscard]] const SortNode* find_sort(const PlanNode& node) {
    if (const auto* sort = std::get_if<SortNode>(&node.kind)) {
        return sort;
    }
    if (const auto* filter = std::get_if<FilterNode>(&node.kind)) {
        return filter->child == nullptr ? nullptr : find_sort(*filter->child);
    }
    if (const auto* project = std::get_if<ProjectNode>(&node.kind)) {
        return project->child == nullptr ? nullptr : find_sort(*project->child);
    }
    return nullptr;
}

[[nodiscard]] std::string generate_order_by_sql(
    std::mt19937& engine,
    std::size_t iteration) {
    static constexpr std::array<std::string_view, 5> columns{
        "id", "big_id", "measure", "active", "note"};
    const std::string_view output = columns[random_index(engine, columns.size())];
    const std::string_view first = columns[random_index(engine, columns.size())];
    const std::string_view second = iteration % 7U == 0U
        ? first
        : columns[random_index(engine, columns.size())];
    const std::string direction = iteration % 3U == 0U
        ? " DESC"
        : (iteration % 3U == 1U ? " ASC" : "");
    std::string sql = "SELECT " + std::string{output} + " FROM order_values";
    if (iteration % 2U == 0U) {
        sql += " WHERE id >= 0";
    }
    sql += " ORDER BY " + std::string{first} + direction;
    if (iteration % 4U != 0U) {
        sql += ',' + std::string{second} +
            (iteration % 5U == 0U ? " DESC" : " ASC");
    }
    sql += ';';
    return sql;
}

[[nodiscard]] bool validate_generated_plan(
    const GeneratedSql& generated,
    const Plan& plan,
    const std::vector<TableMeta>& tables,
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
                create->columns[index].type != generated.create_columns[index].type ||
                create->columns[index].nullable != generated.create_columns[index].nullable) {
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
        const TableMeta* table = table_by_id(tables, generated.table_id);
        const auto* query = std::get_if<QueryPlan>(&plan.kind);
        if (table == nullptr || query == nullptr || query->root == nullptr) {
            reason = "QueryPlan root is null or kind is wrong";
            return false;
        }
        const auto* project = std::get_if<ProjectNode>(&query->root->kind);
        const SeqScanNode* scan = find_scan(*query->root);
        if (project == nullptr || scan == nullptr || scan->table_id != generated.table_id) {
            reason = "QueryPlan topology or SeqScan TableId mismatch";
            return false;
        }
        if (!validate_scan_columns(scan->columns, *table, reason) ||
            !validate_plan_expressions(*query->root, scan->columns, reason)) {
            return false;
        }
        if (project->outputs.size() != generated.select_outputs.size() ||
            query->outputs.size() != project->outputs.size()) {
            reason = "Project or QueryOutput count mismatch";
            return false;
        }
        for (std::size_t index = 0; index < project->outputs.size(); ++index) {
            const ScanColumn* expected = scan_column_for_column(
                scan->columns,
                generated.select_outputs[index]);
            const ScanColumn* source = scan_column_for_slot(
                scan->columns,
                project->outputs[index]);
            if (expected == nullptr || source == nullptr ||
                project->outputs[index] != expected->output_slot ||
                query->outputs[index].slot_id != project->outputs[index]) {
                reason = "Project or QueryOutput SlotId mismatch";
                return false;
            }
            const ColumnMeta& metadata =
                table->columns[static_cast<std::size_t>(source->column_id)];
            if (query->outputs[index].name.empty() ||
                query->outputs[index].name != metadata.name ||
                query->outputs[index].type != metadata.type ||
                query->outputs[index].nullable != metadata.nullable) {
                reason = "QueryOutput source metadata mismatch";
                return false;
            }
        }
        return true;
    }

    const TableMeta* table = table_by_id(tables, generated.table_id);
    const auto* deletion = std::get_if<DeletePlan>(&plan.kind);
    if (table == nullptr || deletion == nullptr || deletion->table_id != generated.table_id) {
        reason = "DeletePlan kind or TableId mismatch";
        return false;
    }
    if (!validate_scan_columns(deletion->input_columns, *table, reason)) {
        return false;
    }
    return !deletion->predicate.has_value() ||
           validate_expression(*deletion->predicate, deletion->input_columns, reason);
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
                lhs[table].columns[column].type != rhs[table].columns[column].type ||
                lhs[table].columns[column].nullable != rhs[table].columns[column].nullable) {
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

struct JoinSlotSource {
    SlotId slot_id;
    const ColumnMeta* column;
};

[[nodiscard]] bool validate_join_tree(
    const PlanNode& node,
    const std::vector<TableMeta>& tables,
    std::vector<ScanColumn>& mappings,
    std::vector<JoinSlotSource>& sources,
    std::size_t& next_slot,
    std::size_t& join_count,
    std::string& reason) {
    if (const auto* scan = std::get_if<SeqScanNode>(&node.kind)) {
        const TableMeta* table = table_by_id(tables, scan->table_id);
        if (table == nullptr || scan->columns.size() != table->columns.size()) {
            reason = "JOIN scan table or schema width mismatch";
            return false;
        }
        for (std::size_t index = 0; index < scan->columns.size(); ++index) {
            const ScanColumn& mapping = scan->columns[index];
            if (mapping.column_id != static_cast<ColumnId>(index) ||
                mapping.output_slot != static_cast<SlotId>(next_slot)) {
                reason = "JOIN slots are not deterministic and globally contiguous";
                return false;
            }
            mappings.push_back(mapping);
            sources.push_back(JoinSlotSource{mapping.output_slot, &table->columns[index]});
            ++next_slot;
        }
        return true;
    }
    if (const auto* join = std::get_if<JoinNode>(&node.kind)) {
        if (!join->left || !join->right || join->kind != JoinKind::kInner ||
            !validate_join_tree(
                *join->left, tables, mappings, sources, next_slot, join_count, reason) ||
            !validate_join_tree(
                *join->right, tables, mappings, sources, next_slot, join_count, reason) ||
            !validate_expression(join->condition, mappings, reason)) {
            return false;
        }
        ++join_count;
        return true;
    }
    if (const auto* filter = std::get_if<FilterNode>(&node.kind)) {
        return filter->child != nullptr &&
            validate_join_tree(
                *filter->child, tables, mappings, sources, next_slot, join_count, reason) &&
            validate_expression(filter->predicate, mappings, reason);
    }
    if (const auto* sort = std::get_if<SortNode>(&node.kind)) {
        if (sort->child == nullptr || sort->keys.empty() ||
            !validate_join_tree(
                *sort->child, tables, mappings, sources, next_slot, join_count, reason)) {
            return false;
        }
        for (const SortKey& key : sort->keys) {
            if (scan_column_for_slot(mappings, key.slot_id) == nullptr) {
                reason = "JOIN Sort key is not provided by an input";
                return false;
            }
        }
        return true;
    }
    if (const auto* project = std::get_if<ProjectNode>(&node.kind)) {
        if (project->child == nullptr ||
            !validate_join_tree(
                *project->child, tables, mappings, sources, next_slot, join_count, reason)) {
            return false;
        }
        for (SlotId slot : project->outputs) {
            if (scan_column_for_slot(mappings, slot) == nullptr) {
                reason = "JOIN Project slot is not provided by an input";
                return false;
            }
        }
        return true;
    }
    reason = "unknown JOIN plan node";
    return false;
}

[[nodiscard]] const JoinSlotSource* aggregate_source(
    const std::vector<JoinSlotSource>& sources,
    SlotId slot) {
    const auto found = std::find_if(
        sources.begin(), sources.end(), [slot](const JoinSlotSource& source) {
            return source.slot_id == slot;
        });
    return found == sources.end() ? nullptr : &*found;
}

[[nodiscard]] bool validate_aggregate_plan(
    const Plan& plan,
    const std::vector<TableMeta>& tables,
    std::string& reason) {
    const auto* query = std::get_if<QueryPlan>(&plan.kind);
    const auto* project = query == nullptr || query->root == nullptr
        ? nullptr
        : std::get_if<ProjectNode>(&query->root->kind);
    if (project == nullptr || project->child == nullptr ||
        project->outputs.size() != query->outputs.size()) {
        reason = "aggregate Project/QueryOutput shape mismatch";
        return false;
    }
    const PlanNode* below_project = project->child.get();
    const SortNode* sort = std::get_if<SortNode>(&below_project->kind);
    const PlanNode* aggregate_node = sort == nullptr ? below_project : sort->child.get();
    const auto* aggregate = aggregate_node == nullptr
        ? nullptr
        : std::get_if<AggregateNode>(&aggregate_node->kind);
    if (aggregate == nullptr || aggregate->child == nullptr) {
        reason = "AggregateNode missing below Project/Sort";
        return false;
    }

    std::vector<ScanColumn> mappings;
    std::vector<JoinSlotSource> sources;
    std::size_t next_slot = 0;
    std::size_t join_count = 0;
    if (!validate_join_tree(
            *aggregate->child, tables, mappings, sources,
            next_slot, join_count, reason)) {
        return false;
    }
    std::vector<SlotId> produced;
    for (SlotId slot : aggregate->group_keys) {
        if (aggregate_source(sources, slot) == nullptr ||
            std::find(produced.begin(), produced.end(), slot) != produced.end()) {
            reason = "aggregate group slot is missing or duplicated";
            return false;
        }
        produced.push_back(slot);
    }
    for (std::size_t index = 0; index < aggregate->aggregates.size(); ++index) {
        const AggregateCall& call = aggregate->aggregates[index];
        if (call.output_slot != static_cast<SlotId>(next_slot + index) ||
            aggregate_source(sources, call.output_slot) != nullptr ||
            std::find(produced.begin(), produced.end(), call.output_slot) != produced.end() ||
            (call.input_slot.has_value() &&
             aggregate_source(sources, *call.input_slot) == nullptr) ||
            (!call.input_slot.has_value() && call.kind != AggregateKind::kCount)) {
            reason = "aggregate input/output SlotId invariant failed";
            return false;
        }
        produced.push_back(call.output_slot);
    }
    if (aggregate->group_keys.empty() && aggregate->aggregates.empty()) {
        reason = "aggregate has neither group keys nor calls";
        return false;
    }
    if (sort != nullptr) {
        if (sort->keys.empty()) {
            reason = "aggregate Sort has no keys";
            return false;
        }
        for (const SortKey& key : sort->keys) {
            if (std::find(produced.begin(), produced.end(), key.slot_id) == produced.end()) {
                reason = "aggregate Sort references an unproduced slot";
                return false;
            }
        }
    }
    for (std::size_t index = 0; index < project->outputs.size(); ++index) {
        const SlotId slot = project->outputs[index];
        if (query->outputs[index].slot_id != slot || query->outputs[index].name.empty() ||
            std::find(produced.begin(), produced.end(), slot) == produced.end()) {
            reason = "aggregate Project/QueryOutput slot invariant failed";
            return false;
        }
        const auto call = std::find_if(
            aggregate->aggregates.begin(), aggregate->aggregates.end(),
            [slot](const AggregateCall& candidate) { return candidate.output_slot == slot; });
        if (call != aggregate->aggregates.end()) {
            if (query->outputs[index].type != call->output_type ||
                query->outputs[index].nullable != call->nullable) {
                reason = "aggregate QueryOutput metadata differs from AggregateCall";
                return false;
            }
        } else {
            const JoinSlotSource* source = aggregate_source(sources, slot);
            if (source == nullptr || query->outputs[index].type != source->column->type ||
                query->outputs[index].nullable != source->column->nullable) {
                reason = "group-key QueryOutput metadata differs from source";
                return false;
            }
        }
    }
    return true;
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
        if (!validate_generated_plan(generated, plan, tables, reason)) {
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

    std::vector<TableMeta> bigint_tables{
        TableMeta{700U, "bigint_values", {
            ColumnMeta{"big_id", Type::kBigInt},
            ColumnMeta{"small_id", Type::kInt},
        }}
    };
    const std::vector<TableMeta> bigint_catalog_before = bigint_tables;
    const CatalogView bigint_catalog{std::span<const TableMeta>{bigint_tables}};
    std::mt19937 bigint_engine{kBigIntSeed};
    for (std::size_t iteration = 0; iteration < kBigIntFuzzCount; ++iteration) {
        const std::string sql = generate_bigint_sql(bigint_engine, iteration);
        const CompileResult result = compile(CompileRequest{sql, bigint_catalog});
        const auto* plan = std::get_if<Plan>(&result.outcome);
        if (plan == nullptr) {
            const auto& error = std::get<CompileError>(result.outcome);
            std::cerr << "BIGINT seed=" << kBigIntSeed << " iteration=" << iteration
                      << "\nSQL: " << sql << "\nerror=" << error.message << '\n';
            return 1;
        }
        const CompileResult repeated = compile(CompileRequest{sql, bigint_catalog});
        const auto* repeated_plan = std::get_if<Plan>(&repeated.outcome);
        if (repeated_plan == nullptr || format_plan(*plan) != format_plan(*repeated_plan)) {
            std::cerr << "BIGINT determinism failure: seed=" << kBigIntSeed
                      << " iteration=" << iteration << '\n';
            return 1;
        }
    }
    if (!catalogs_equal(bigint_tables, bigint_catalog_before)) {
        std::cerr << "BIGINT seed=" << kBigIntSeed << ": CatalogView backing data changed\n";
        return 1;
    }

    std::vector<TableMeta> double_tables{
        TableMeta{701U, "double_values", {
            ColumnMeta{"small_id", Type::kInt},
            ColumnMeta{"big_id", Type::kBigInt},
            ColumnMeta{"measure", Type::kDouble},
        }}
    };
    const std::vector<TableMeta> double_catalog_before = double_tables;
    const CatalogView double_catalog{std::span<const TableMeta>{double_tables}};
    std::mt19937 double_engine{kDoubleSeed};
    for (std::size_t iteration = 0; iteration < kDoubleFuzzCount; ++iteration) {
        const GeneratedSql generated = generate_double_sql(double_engine, iteration);
        const CompileResult result = compile(CompileRequest{generated.sql, double_catalog});
        const auto* plan = std::get_if<Plan>(&result.outcome);
        std::string reason;
        if (plan == nullptr ||
            !validate_generated_plan(generated, *plan, double_tables, reason)) {
            std::cerr << "DOUBLE seed=" << kDoubleSeed << " iteration=" << iteration
                      << "\nSQL: " << generated.sql << "\nreason=" << reason << '\n';
            return 1;
        }
        const CompileResult repeated = compile(CompileRequest{generated.sql, double_catalog});
        const auto* repeated_plan = std::get_if<Plan>(&repeated.outcome);
        if (repeated_plan == nullptr || format_plan(*plan) != format_plan(*repeated_plan)) {
            std::cerr << "DOUBLE determinism failure: seed=" << kDoubleSeed
                      << " iteration=" << iteration << '\n';
            return 1;
        }
    }
    if (!catalogs_equal(double_tables, double_catalog_before)) {
        std::cerr << "DOUBLE seed=" << kDoubleSeed << ": CatalogView backing data changed\n";
        return 1;
    }

    std::vector<TableMeta> boolean_tables{
        TableMeta{702U, "boolean_values", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"active", Type::kBoolean},
        }}
    };
    const std::vector<TableMeta> boolean_catalog_before = boolean_tables;
    const CatalogView boolean_catalog{std::span<const TableMeta>{boolean_tables}};
    std::mt19937 boolean_engine{kBooleanSeed};
    for (std::size_t iteration = 0; iteration < kBooleanFuzzCount; ++iteration) {
        const GeneratedSql generated = generate_boolean_sql(boolean_engine, iteration);
        const CompileResult result = compile(CompileRequest{generated.sql, boolean_catalog});
        const auto* plan = std::get_if<Plan>(&result.outcome);
        std::string reason;
        if (plan == nullptr ||
            !validate_generated_plan(generated, *plan, boolean_tables, reason)) {
            std::cerr << "BOOLEAN seed=" << kBooleanSeed << " iteration=" << iteration
                      << "\nSQL: " << generated.sql << "\nreason=" << reason << '\n';
            return 1;
        }
        const CompileResult repeated = compile(CompileRequest{generated.sql, boolean_catalog});
        const auto* repeated_plan = std::get_if<Plan>(&repeated.outcome);
        if (repeated_plan == nullptr || format_plan(*plan) != format_plan(*repeated_plan)) {
            std::cerr << "BOOLEAN determinism failure: seed=" << kBooleanSeed
                      << " iteration=" << iteration << '\n';
            return 1;
        }
    }
    if (!catalogs_equal(boolean_tables, boolean_catalog_before)) {
        std::cerr << "BOOLEAN seed=" << kBooleanSeed << ": CatalogView backing data changed\n";
        return 1;
    }

    const std::vector<TableMeta> null_tables{
        TableMeta{703U, "nullable_values", {
            ColumnMeta{"id", Type::kInt, false},
            ColumnMeta{"note", Type::kVarchar, true},
            ColumnMeta{"active", Type::kBoolean, true},
        }}
    };
    const CatalogView null_catalog{std::span<const TableMeta>{null_tables}};
    static constexpr std::array<std::string_view, 17> null_sql{
        "CREATE TABLE nullable_default(value INT);",
        "CREATE TABLE nullable_explicit(value INT NULL);",
        "CREATE TABLE nullable_fuzz(id INT NOT NULL,note VARCHAR NULL);",
        "INSERT INTO nullable_values VALUES (1,NULL,NULL);",
        "SELECT id FROM nullable_values WHERE note IS NULL;",
        "SELECT id FROM nullable_values WHERE note IS NOT NULL;",
        "SELECT id FROM nullable_values WHERE id = NULL;",
        "SELECT id FROM nullable_values WHERE id < NULL;",
        "SELECT id FROM nullable_values WHERE note = NULL;",
        "SELECT id FROM nullable_values WHERE active = NULL;",
        "SELECT id FROM nullable_values WHERE NULL = NULL;",
        "SELECT id FROM nullable_values WHERE TRUE AND NULL;",
        "SELECT id FROM nullable_values WHERE FALSE OR NULL;",
        "SELECT id FROM nullable_values WHERE NOT NULL;",
        "SELECT id FROM nullable_values WHERE active;",
        "DELETE FROM nullable_values WHERE active IS NULL;",
        "DELETE FROM nullable_values WHERE active OR NULL;",
    };
    std::mt19937 null_engine{kNullSeed};
    for (std::size_t iteration = 0; iteration < kNullFuzzCount; ++iteration) {
        const std::size_t sql_index = iteration < null_sql.size()
            ? iteration
            : random_index(null_engine, null_sql.size());
        const std::string_view sql = null_sql[sql_index];
        const CompileResult result = compile(CompileRequest{std::string{sql}, null_catalog});
        const auto* plan = std::get_if<Plan>(&result.outcome);
        if (plan == nullptr) {
            const auto& error = std::get<CompileError>(result.outcome);
            std::cerr << "NULL seed=" << kNullSeed << " iteration=" << iteration
                      << "\nSQL: " << sql << "\nerror=" << error.message << '\n';
            return 1;
        }
        const CompileResult repeated = compile(CompileRequest{std::string{sql}, null_catalog});
        const auto* repeated_plan = std::get_if<Plan>(&repeated.outcome);
        if (repeated_plan == nullptr || format_plan(*plan) != format_plan(*repeated_plan)) {
            std::cerr << "NULL determinism failure: seed=" << kNullSeed
                      << " iteration=" << iteration << '\n';
            return 1;
        }
    }

    const TableMeta update_table{704U,"update_values",{
        {"id",Type::kInt,false},{"big_id",Type::kBigInt,false},
        {"measure",Type::kDouble,false},{"active",Type::kBoolean,true},
        {"note",Type::kVarchar,true}}};
    const std::vector<TableMeta> update_tables{update_table};
    const CatalogView update_catalog{std::span<const TableMeta>{update_tables}};
    std::mt19937 update_engine{kUpdateSeed};
    for(std::size_t iteration=0;iteration<kUpdateFuzzCount;++iteration) {
        const std::string sql=generate_update_sql(update_engine,iteration);
        const CompileResult result=compile(CompileRequest{sql,update_catalog});
        const auto* plan=std::get_if<Plan>(&result.outcome);
        std::string reason;
        if(plan==nullptr || !validate_update_plan(*plan,update_table,reason)) {
            std::cerr<<"UPDATE seed="<<kUpdateSeed<<" iteration="<<iteration
                     <<"\nSQL: "<<sql<<"\nreason="<<reason<<'\n';return 1;
        }
        const CompileResult repeated=compile(CompileRequest{sql,update_catalog});
        const auto* repeated_plan=std::get_if<Plan>(&repeated.outcome);
        if(repeated_plan==nullptr || format_plan(*plan)!=format_plan(*repeated_plan)) {
            std::cerr<<"UPDATE determinism failure: seed="<<kUpdateSeed
                     <<" iteration="<<iteration<<'\n';return 1;
        }
    }

    const std::vector<TableMeta> order_tables{
        TableMeta{705U, "order_values", {
            {"id", Type::kInt, false},
            {"big_id", Type::kBigInt, true},
            {"measure", Type::kDouble, true},
            {"active", Type::kBoolean, true},
            {"note", Type::kVarchar, true}}}};
    const CatalogView order_catalog{std::span<const TableMeta>{order_tables}};
    std::mt19937 order_engine{kOrderBySeed};
    for (std::size_t iteration = 0; iteration < kOrderByFuzzCount; ++iteration) {
        const std::string sql = generate_order_by_sql(order_engine, iteration);
        const CompileResult result = compile(CompileRequest{sql, order_catalog});
        const auto* plan = std::get_if<Plan>(&result.outcome);
        const auto* query = plan == nullptr ? nullptr : std::get_if<QueryPlan>(&plan->kind);
        const auto* project = query == nullptr || query->root == nullptr
            ? nullptr
            : std::get_if<ProjectNode>(&query->root->kind);
        const SeqScanNode* scan = query == nullptr || query->root == nullptr
            ? nullptr
            : find_scan(*query->root);
        const SortNode* sort = query == nullptr || query->root == nullptr
            ? nullptr
            : find_sort(*query->root);
        std::string reason;
        if (project == nullptr || scan == nullptr || sort == nullptr || sort->keys.empty() ||
            !validate_scan_columns(scan->columns, order_tables[0], reason) ||
            !validate_plan_expressions(*query->root, scan->columns, reason) ||
            query->outputs.size() != project->outputs.size()) {
            std::cerr << "ORDER BY valid seed=" << kOrderBySeed
                      << " iteration=" << iteration << "\nSQL: " << sql
                      << "\nreason=" << reason << '\n';
            return 1;
        }
        for (std::size_t index = 0; index < project->outputs.size(); ++index) {
            if (project->outputs[index] != query->outputs[index].slot_id) {
                std::cerr << "ORDER BY output invariant failure: seed=" << kOrderBySeed
                          << " iteration=" << iteration << '\n';
                return 1;
            }
        }
        const CompileResult repeated = compile(CompileRequest{sql, order_catalog});
        const auto* repeated_plan = std::get_if<Plan>(&repeated.outcome);
        if (repeated_plan == nullptr || format_plan(*plan) != format_plan(*repeated_plan)) {
            std::cerr << "ORDER BY determinism failure: seed=" << kOrderBySeed
                      << " iteration=" << iteration << '\n';
            return 1;
        }
    }

    const std::vector<TableMeta> join_tables{
        TableMeta{800U, "lhs_values", {
            {"id", Type::kInt, false}, {"shared", Type::kVarchar, true},
            {"l_big", Type::kBigInt, false}, {"active", Type::kBoolean, false},
            {"measure", Type::kDouble, true}}},
        TableMeta{801U, "rhs_values", {
            {"id", Type::kBigInt, true}, {"shared", Type::kVarchar, false},
            {"r_only", Type::kInt, false}, {"active", Type::kBoolean, true},
            {"measure", Type::kDouble, false}}},
        TableMeta{802U, "third_values", {
            {"id", Type::kInt, false}, {"rhs_id", Type::kBigInt, false}}}};
    const CatalogView join_catalog{std::span<const TableMeta>{join_tables}};
    static constexpr std::array<std::string_view, 12> join_sql{
        "SELECT lhs_values.id,rhs_values.shared FROM lhs_values JOIN rhs_values ON lhs_values.id=rhs_values.id;",
        "SELECT l_big,r_only FROM lhs_values INNER JOIN rhs_values ON lhs_values.id=rhs_values.id;",
        "SELECT * FROM lhs_values JOIN rhs_values ON lhs_values.id=rhs_values.id;",
        "SELECT lhs_values.id,lhs_values.id FROM lhs_values JOIN rhs_values ON TRUE;",
        "SELECT lhs_values.shared FROM lhs_values JOIN rhs_values ON lhs_values.active=rhs_values.active;",
        "SELECT rhs_values.r_only FROM lhs_values JOIN rhs_values ON NULL;",
        "SELECT lhs_values.id FROM lhs_values JOIN rhs_values ON lhs_values.id=rhs_values.id WHERE rhs_values.active;",
        "SELECT lhs_values.shared FROM lhs_values JOIN rhs_values ON lhs_values.id=rhs_values.id ORDER BY rhs_values.measure DESC;",
        "SELECT r_only FROM lhs_values JOIN rhs_values ON lhs_values.measure=rhs_values.measure ORDER BY l_big;",
        "SELECT lhs_values.id,rhs_values.id FROM lhs_values JOIN rhs_values ON lhs_values.shared=rhs_values.shared;",
        "SELECT third_values.id FROM lhs_values JOIN rhs_values ON lhs_values.id=rhs_values.id JOIN third_values ON rhs_values.id=third_values.rhs_id;",
        "SELECT lhs_values.active,rhs_values.active FROM lhs_values JOIN rhs_values ON lhs_values.id=rhs_values.id WHERE rhs_values.shared IS NOT NULL;"};
    std::mt19937 join_engine{kJoinSeed};
    for (std::size_t iteration = 0; iteration < kJoinFuzzCount; ++iteration) {
        const std::string_view sql = join_sql[random_index(join_engine, join_sql.size())];
        const CompileResult result = compile(CompileRequest{std::string{sql}, join_catalog});
        const auto* plan = std::get_if<Plan>(&result.outcome);
        const auto* query = plan == nullptr ? nullptr : std::get_if<QueryPlan>(&plan->kind);
        const auto* project = query == nullptr || query->root == nullptr
            ? nullptr
            : std::get_if<ProjectNode>(&query->root->kind);
        std::vector<ScanColumn> mappings;
        std::vector<JoinSlotSource> sources;
        std::size_t next_slot = 0;
        std::size_t join_count = 0;
        std::string reason;
        if (project == nullptr ||
            !validate_join_tree(
                *query->root, join_tables, mappings, sources,
                next_slot, join_count, reason) ||
            join_count == 0U || query->outputs.size() != project->outputs.size()) {
            std::cerr << "JOIN valid seed=" << kJoinSeed << " iteration=" << iteration
                      << "\nSQL: " << sql << "\nreason=" << reason << '\n';
            return 1;
        }
        for (std::size_t index = 0; index < project->outputs.size(); ++index) {
            const SlotId slot = project->outputs[index];
            const auto source = std::find_if(
                sources.begin(), sources.end(), [slot](const JoinSlotSource& candidate) {
                    return candidate.slot_id == slot;
                });
            if (query->outputs[index].slot_id != slot || source == sources.end() ||
                query->outputs[index].name != source->column->name ||
                query->outputs[index].type != source->column->type ||
                query->outputs[index].nullable != source->column->nullable) {
                std::cerr << "JOIN output invariant failure: seed=" << kJoinSeed
                          << " iteration=" << iteration << '\n';
                return 1;
            }
        }
        const CompileResult repeated = compile(CompileRequest{std::string{sql}, join_catalog});
        const auto* repeated_plan = std::get_if<Plan>(&repeated.outcome);
        if (repeated_plan == nullptr || format_plan(*plan) != format_plan(*repeated_plan)) {
            std::cerr << "JOIN determinism failure: seed=" << kJoinSeed
                      << " iteration=" << iteration << '\n';
            return 1;
        }
    }

    const std::vector<TableMeta> aggregate_tables{
        TableMeta{900U, "aggregate_values", {
            {"id", Type::kInt, false}, {"big_id", Type::kBigInt, true},
            {"measure", Type::kDouble, true}, {"active", Type::kBoolean, true},
            {"note", Type::kVarchar, true}}},
        TableMeta{901U, "aggregate_rhs", {
            {"value_id", Type::kInt, false}, {"amount", Type::kInt, true}}}};
    const CatalogView aggregate_catalog{std::span<const TableMeta>{aggregate_tables}};
    static constexpr std::array<std::string_view, 16> aggregate_sql{
        "SELECT COUNT(*) FROM aggregate_values;",
        "SELECT COUNT(id),COUNT(big_id),COUNT(measure),COUNT(active),COUNT(note) FROM aggregate_values;",
        "SELECT SUM(id),SUM(big_id),SUM(measure) FROM aggregate_values;",
        "SELECT AVG(id),AVG(big_id),AVG(measure) FROM aggregate_values;",
        "SELECT MIN(id),MAX(big_id),MIN(measure),MAX(note) FROM aggregate_values;",
        "SELECT note,COUNT(*) FROM aggregate_values GROUP BY note ORDER BY note;",
        "SELECT active,COUNT(id),SUM(big_id) FROM aggregate_values GROUP BY active ORDER BY active DESC;",
        "SELECT note,active,COUNT(*) FROM aggregate_values GROUP BY note,active ORDER BY note,active;",
        "SELECT COUNT(*) FROM aggregate_values GROUP BY note ORDER BY note;",
        "SELECT note FROM aggregate_values GROUP BY note ORDER BY note;",
        "SELECT note,note,COUNT(*),COUNT(*) FROM aggregate_values GROUP BY note;",
        "SELECT note,SUM(id) FROM aggregate_values WHERE active GROUP BY note ORDER BY note;",
        "SELECT aggregate_values.note,COUNT(aggregate_rhs.amount) FROM aggregate_values "
        "JOIN aggregate_rhs ON aggregate_values.id=aggregate_rhs.value_id "
        "GROUP BY aggregate_values.note ORDER BY aggregate_values.note;",
        "SELECT COUNT(*),SUM(aggregate_rhs.amount) FROM aggregate_values JOIN aggregate_rhs "
        "ON aggregate_values.id=aggregate_rhs.value_id WHERE aggregate_values.active;",
        "SELECT aggregate_values.active,AVG(aggregate_rhs.amount),MIN(aggregate_values.note) "
        "FROM aggregate_values JOIN aggregate_rhs ON aggregate_values.id=aggregate_rhs.value_id "
        "GROUP BY aggregate_values.active ORDER BY aggregate_values.active;",
        "SELECT big_id,MIN(big_id),MAX(big_id) FROM aggregate_values GROUP BY big_id ORDER BY big_id;"};
    std::mt19937 aggregate_engine{kAggregateSeed};
    for (std::size_t iteration = 0; iteration < kAggregateFuzzCount; ++iteration) {
        const std::string_view sql =
            aggregate_sql[random_index(aggregate_engine, aggregate_sql.size())];
        const CompileResult result = compile(CompileRequest{std::string{sql}, aggregate_catalog});
        const auto* plan = std::get_if<Plan>(&result.outcome);
        std::string reason;
        if (plan == nullptr || !validate_aggregate_plan(*plan, aggregate_tables, reason)) {
            std::cerr << "Aggregate valid seed=" << kAggregateSeed
                      << " iteration=" << iteration << "\nSQL: " << sql
                      << "\nreason=" << reason << '\n';
            return 1;
        }
        const CompileResult repeated =
            compile(CompileRequest{std::string{sql}, aggregate_catalog});
        const auto* repeated_plan = std::get_if<Plan>(&repeated.outcome);
        if (repeated_plan == nullptr || format_plan(*plan) != format_plan(*repeated_plan)) {
            std::cerr << "Aggregate determinism failure: seed=" << kAggregateSeed
                      << " iteration=" << iteration << '\n';
            return 1;
        }
    }

    if (argc == 2 && std::string_view{argv[1]} == "--stats") {
        std::cout << "seed=" << kSeed << " total=" << sequence.size()
                  << " create=" << counts.create
                  << " insert=" << counts.insert
                  << " select=" << counts.select
                  << " delete=" << counts.deletion
                  << "\nbigint_seed=" << kBigIntSeed
                  << " bigint_total=" << kBigIntFuzzCount
                  << "\ndouble_seed=" << kDoubleSeed
                  << " double_total=" << kDoubleFuzzCount
                  << "\nboolean_seed=" << kBooleanSeed
                  << " boolean_total=" << kBooleanFuzzCount
                  << "\nnull_seed=" << kNullSeed
                  << " null_total=" << kNullFuzzCount
                  << "\nupdate_seed=" << kUpdateSeed
                  << " update_total=" << kUpdateFuzzCount
                  << "\norder_by_seed=" << kOrderBySeed
                  << " order_by_total=" << kOrderByFuzzCount
                  << "\njoin_seed=" << kJoinSeed
                  << " join_total=" << kJoinFuzzCount
                  << "\naggregate_seed=" << kAggregateSeed
                  << " aggregate_total=" << kAggregateFuzzCount << '\n';
    }
    return 0;
}
