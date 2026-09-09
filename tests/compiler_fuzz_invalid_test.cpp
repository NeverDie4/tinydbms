#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
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

constexpr std::uint32_t kSeed = 20260910U;
constexpr std::size_t kGuaranteedCount = 1000;
constexpr std::size_t kGeneralCount = 3000;

enum class MutationKind {
    kMissingSemicolon,
    kIllegalCharacter,
    kUnclosedString,
    kUnclosedBlockComment,
    kMissingSelectFrom,
    kMissingInsertValues,
    kMissingDeleteFrom,
    kMissingCreateTable,
    kDuplicateDelimiter,
    kBrokenLeftParenthesis,
    kBrokenRightParenthesis,
    kDoubleEqual,
    kBareBang,
    kAngleNotEqual,
    kUnknownTable,
    kUnknownColumn,
    kTypeMismatch,
    kVarcharOrdering,
    kPartialInsertColumns,
    kDuplicateInsertColumn,
    kTooFewInsertValues,
    kTooManyInsertValues,
    kWrongInsertType,
    kNonBoolWhere,
    kUnsupportedStatement,
    kCount
};

enum class GeneralMutation {
    kDeleteCharacter,
    kInsertCharacter,
    kReplaceCharacter,
    kDuplicateCharacter,
    kDuplicateSpan,
    kDeleteSpan,
    kInsertWhitespace,
    kInsertNewline,
    kInsertCommentMarker,
    kChangeLetterCase
};

struct GuaranteedCase {
    MutationKind kind;
    std::string original;
    std::string mutated;
    std::optional<CompileErrorKind> expected_kind;
};

struct GeneralCase {
    std::string original;
    std::string mutated;
};

struct Corpus {
    std::vector<GuaranteedCase> guaranteed;
    std::vector<GeneralCase> general;
};

struct ResultCounts {
    std::size_t success{0};
    std::size_t lex{0};
    std::size_t syntax{0};
    std::size_t semantic{0};
};

struct GuaranteedStageCounts {
    std::size_t lex{0};
    std::size_t syntax{0};
    std::size_t semantic{0};
    std::size_t unspecified{0};
};

[[nodiscard]] int random_int(std::mt19937& engine, int minimum, int maximum) {
    return std::uniform_int_distribution<int>{minimum, maximum}(engine);
}

[[nodiscard]] std::size_t random_index(std::mt19937& engine, std::size_t size) {
    return std::uniform_int_distribution<std::size_t>{0, size - 1}(engine);
}

[[nodiscard]] std::int32_t random_literal(std::mt19937& engine) {
    switch (random_int(engine, 0, 3)) {
        case 0:
            return 0;
        case 1:
            return 1;
        case 2:
            return std::numeric_limits<std::int32_t>::max();
        default:
            return std::uniform_int_distribution<std::int32_t>{2, 100000}(engine);
    }
}

[[nodiscard]] std::string quote_sql(std::string_view value) {
    std::string result{"'"};
    for (const char character : value) {
        result += character == '\'' ? "''" : std::string(1, character);
    }
    result += '\'';
    return result;
}

[[nodiscard]] std::string random_string(std::mt19937& engine) {
    static const std::vector<std::string> values{
        "", "Alice", "Tom's", ";", ",", "--", "/* */", "A B"
    };
    return values[random_index(engine, values.size())];
}

[[nodiscard]] ColumnId first_column(const TableMeta& table, Type type) {
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
        if (table.columns[index].type == type) {
            return static_cast<ColumnId>(index);
        }
    }
    return 0U;
}

[[nodiscard]] std::string valid_row(std::mt19937& engine, const TableMeta& table) {
    std::string result{"("};
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
        if (index != 0) {
            result += ',';
        }
        result += table.columns[index].type == Type::kInt
            ? std::to_string(random_literal(engine))
            : quote_sql(random_string(engine));
    }
    result += ')';
    return result;
}

[[nodiscard]] std::string valid_sql(
    std::mt19937& engine,
    const std::vector<TableMeta>& tables,
    std::size_t iteration) {
    const TableMeta& table = tables[random_index(engine, tables.size())];
    const ColumnId int_column = first_column(table, Type::kInt);
    const ColumnId text_column = first_column(table, Type::kVarchar);
    switch (random_int(engine, 0, 3)) {
        case 0:
            return "CREATE TABLE general_t_" + std::to_string(iteration) +
                   "(a INT,b VARCHAR);";
        case 1:
            return "INSERT INTO " + table.table_name + " VALUES " +
                   valid_row(engine, table) + ';';
        case 2:
            return "SELECT " + table.columns[text_column].name + ',' +
                   table.columns[int_column].name + " FROM " + table.table_name +
                   " WHERE (" + table.columns[int_column].name + " >= " +
                   std::to_string(random_literal(engine)) + " AND " +
                   table.columns[text_column].name + " != " +
                   quote_sql(random_string(engine)) + ");";
        default:
            return "DELETE FROM " + table.table_name + " WHERE NOT (" +
                   table.columns[int_column].name + " = " +
                   std::to_string(random_literal(engine)) + ");";
    }
}

[[nodiscard]] std::string_view mutation_name(MutationKind kind) {
    switch (kind) {
        case MutationKind::kMissingSemicolon: return "MissingSemicolon";
        case MutationKind::kIllegalCharacter: return "IllegalCharacter";
        case MutationKind::kUnclosedString: return "UnclosedString";
        case MutationKind::kUnclosedBlockComment: return "UnclosedBlockComment";
        case MutationKind::kMissingSelectFrom: return "MissingSelectFrom";
        case MutationKind::kMissingInsertValues: return "MissingInsertValues";
        case MutationKind::kMissingDeleteFrom: return "MissingDeleteFrom";
        case MutationKind::kMissingCreateTable: return "MissingCreateTable";
        case MutationKind::kDuplicateDelimiter: return "DuplicateDelimiter";
        case MutationKind::kBrokenLeftParenthesis: return "BrokenLeftParenthesis";
        case MutationKind::kBrokenRightParenthesis: return "BrokenRightParenthesis";
        case MutationKind::kDoubleEqual: return "DoubleEqual";
        case MutationKind::kBareBang: return "BareBang";
        case MutationKind::kAngleNotEqual: return "AngleNotEqual";
        case MutationKind::kUnknownTable: return "UnknownTable";
        case MutationKind::kUnknownColumn: return "UnknownColumn";
        case MutationKind::kTypeMismatch: return "TypeMismatch";
        case MutationKind::kVarcharOrdering: return "VarcharOrdering";
        case MutationKind::kPartialInsertColumns: return "PartialInsertColumns";
        case MutationKind::kDuplicateInsertColumn: return "DuplicateInsertColumn";
        case MutationKind::kTooFewInsertValues: return "TooFewInsertValues";
        case MutationKind::kTooManyInsertValues: return "TooManyInsertValues";
        case MutationKind::kWrongInsertType: return "WrongInsertType";
        case MutationKind::kNonBoolWhere: return "NonBoolWhere";
        case MutationKind::kUnsupportedStatement: return "UnsupportedStatement";
        case MutationKind::kCount: break;
    }
    return "UnknownMutation";
}

[[nodiscard]] GuaranteedCase generate_guaranteed_case(
    std::mt19937& engine,
    const std::vector<TableMeta>& tables,
    std::size_t iteration) {
    const std::size_t kind_count = static_cast<std::size_t>(MutationKind::kCount);
    const MutationKind kind = static_cast<MutationKind>(iteration % kind_count);
    const TableMeta& table = tables[random_index(engine, tables.size())];
    const ColumnId int_column = first_column(table, Type::kInt);
    const ColumnId text_column = first_column(table, Type::kVarchar);
    const std::string& int_name = table.columns[int_column].name;
    const std::string& text_name = table.columns[text_column].name;
    const std::string value = std::to_string(random_literal(engine));
    const std::string create_name = "invalid_origin_" + std::to_string(iteration);

    switch (kind) {
        case MutationKind::kMissingSemicolon:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + ';',
                    "SELECT " + int_name + " FROM " + table.table_name,
                    CompileErrorKind::kSyntax};
        case MutationKind::kIllegalCharacter:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + ';',
                    "SELECT @ " + int_name + " FROM " + table.table_name + ';',
                    CompileErrorKind::kLex};
        case MutationKind::kUnclosedString:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        text_name + " = 'Alice';",
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        text_name + " = 'Alice;",
                    CompileErrorKind::kLex};
        case MutationKind::kUnclosedBlockComment:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + ';',
                    "SELECT " + int_name + " FROM " + table.table_name +
                        "; /* unfinished",
                    CompileErrorKind::kLex};
        case MutationKind::kMissingSelectFrom:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + ';',
                    "SELECT " + int_name + ' ' + table.table_name + ';',
                    CompileErrorKind::kSyntax};
        case MutationKind::kMissingInsertValues:
            return {kind,
                    "INSERT INTO " + table.table_name + " VALUES " +
                        valid_row(engine, table) + ';',
                    "INSERT INTO " + table.table_name + ' ' + valid_row(engine, table) + ';',
                    CompileErrorKind::kSyntax};
        case MutationKind::kMissingDeleteFrom:
            return {kind,
                    "DELETE FROM " + table.table_name + ';',
                    "DELETE " + table.table_name + ';',
                    CompileErrorKind::kSyntax};
        case MutationKind::kMissingCreateTable:
            return {kind,
                    "CREATE TABLE " + create_name + "(a INT);",
                    "CREATE " + create_name + "(a INT);",
                    CompileErrorKind::kSyntax};
        case MutationKind::kDuplicateDelimiter:
            return {kind,
                    "SELECT " + int_name + ',' + text_name + " FROM " +
                        table.table_name + ';',
                    "SELECT " + int_name + ",," + text_name + " FROM " +
                        table.table_name + ';',
                    CompileErrorKind::kSyntax};
        case MutationKind::kBrokenLeftParenthesis:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE (" +
                        int_name + " > 1 AND " + int_name + " = 1);",
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        int_name + " > 1 AND " + int_name + " = 1);",
                    CompileErrorKind::kSyntax};
        case MutationKind::kBrokenRightParenthesis:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE (" +
                        int_name + " > 1 AND " + int_name + " = 1);",
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE (" +
                        int_name + " > 1 AND " + int_name + " = 1;",
                    CompileErrorKind::kSyntax};
        case MutationKind::kDoubleEqual:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        int_name + " = " + value + ';',
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        int_name + " == " + value + ';',
                    std::nullopt};
        case MutationKind::kBareBang:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        int_name + " != " + value + ';',
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        int_name + " ! " + value + ';',
                    std::nullopt};
        case MutationKind::kAngleNotEqual:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        int_name + " != " + value + ';',
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        int_name + " <> " + value + ';',
                    std::nullopt};
        case MutationKind::kUnknownTable:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + ';',
                    "SELECT " + int_name + " FROM fuzz_unknown_table;",
                    CompileErrorKind::kSemantic};
        case MutationKind::kUnknownColumn:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + ';',
                    "SELECT fuzz_unknown_column FROM " + table.table_name + ';',
                    CompileErrorKind::kSemantic};
        case MutationKind::kTypeMismatch:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        int_name + " = 1;",
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        int_name + " = 'abc';",
                    CompileErrorKind::kSemantic};
        case MutationKind::kVarcharOrdering:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        text_name + " = 'Tom';",
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        text_name + " > 'Tom';",
                    CompileErrorKind::kSemantic};
        case MutationKind::kPartialInsertColumns:
            return {kind,
                    "INSERT INTO " + table.table_name + '(' +
                        table.columns[0].name + ',' + table.columns[1].name + ',' +
                        table.columns[2].name + ") VALUES " + valid_row(engine, table) + ';',
                    "INSERT INTO " + table.table_name + '(' +
                        table.columns[0].name + ',' + table.columns[1].name +
                        ") VALUES (1,'Alice');",
                    CompileErrorKind::kSemantic};
        case MutationKind::kDuplicateInsertColumn:
            return {kind,
                    "INSERT INTO " + table.table_name + " VALUES " +
                        valid_row(engine, table) + ';',
                    "INSERT INTO " + table.table_name + '(' +
                        table.columns[0].name + ',' + table.columns[0].name + ',' +
                        table.columns[2].name + ") VALUES (1,2,3);",
                    CompileErrorKind::kSemantic};
        case MutationKind::kTooFewInsertValues:
            return {kind,
                    "INSERT INTO " + table.table_name + " VALUES " +
                        valid_row(engine, table) + ';',
                    "INSERT INTO " + table.table_name + " VALUES (1,'Alice');",
                    CompileErrorKind::kSemantic};
        case MutationKind::kTooManyInsertValues:
            return {kind,
                    "INSERT INTO " + table.table_name + " VALUES " +
                        valid_row(engine, table) + ';',
                    "INSERT INTO " + table.table_name + " VALUES (1,'Alice',20,100);",
                    CompileErrorKind::kSemantic};
        case MutationKind::kWrongInsertType:
            return {kind,
                    "INSERT INTO " + table.table_name + " VALUES " +
                        valid_row(engine, table) + ';',
                    "INSERT INTO " + table.table_name + " VALUES ('wrong','Alice',20);",
                    CompileErrorKind::kSemantic};
        case MutationKind::kNonBoolWhere:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        int_name + " = 1;",
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        int_name + ';',
                    CompileErrorKind::kSemantic};
        case MutationKind::kUnsupportedStatement:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + ';',
                    iteration % 3 == 0
                        ? "UPDATE " + table.table_name + " SET " + int_name + " = 1;"
                        : (iteration % 3 == 1
                            ? "DROP TABLE " + table.table_name + ';'
                            : "ALTER TABLE " + table.table_name + " ADD x INT;"),
                    std::nullopt};
        case MutationKind::kCount:
            break;
    }
    return {kind, {}, {}, std::nullopt};
}

void apply_general_mutation(std::mt19937& engine, std::string& sql) {
    const GeneralMutation kind = static_cast<GeneralMutation>(random_int(engine, 0, 9));
    const std::size_t position = random_index(engine, sql.size());
    constexpr std::string_view characters = "abcXYZ019 @'(),;=!<>/*-";
    switch (kind) {
        case GeneralMutation::kDeleteCharacter:
            sql.erase(position, 1);
            break;
        case GeneralMutation::kInsertCharacter:
            sql.insert(position, 1, characters[random_index(engine, characters.size())]);
            break;
        case GeneralMutation::kReplaceCharacter:
            sql[position] = characters[random_index(engine, characters.size())];
            break;
        case GeneralMutation::kDuplicateCharacter:
            sql.insert(position, 1, sql[position]);
            break;
        case GeneralMutation::kDuplicateSpan: {
            const std::size_t maximum = std::min<std::size_t>(5, sql.size() - position);
            const std::size_t length =
                std::uniform_int_distribution<std::size_t>{1, maximum}(engine);
            sql.insert(position, sql.substr(position, length));
            break;
        }
        case GeneralMutation::kDeleteSpan: {
            const std::size_t maximum = std::min<std::size_t>(5, sql.size() - position);
            const std::size_t length =
                std::uniform_int_distribution<std::size_t>{1, maximum}(engine);
            sql.erase(position, length);
            break;
        }
        case GeneralMutation::kInsertWhitespace:
            sql.insert(position, random_int(engine, 0, 1) == 0 ? " " : "  ");
            break;
        case GeneralMutation::kInsertNewline:
            sql.insert(position, random_int(engine, 0, 1) == 0 ? "\n" : "\r\n");
            break;
        case GeneralMutation::kInsertCommentMarker: {
            static const std::vector<std::string_view> markers{"--", "/*", "*/", "/**/"};
            sql.insert(position, markers[random_index(engine, markers.size())]);
            break;
        }
        case GeneralMutation::kChangeLetterCase:
            for (std::size_t offset = 0; offset < sql.size(); ++offset) {
                const std::size_t index = (position + offset) % sql.size();
                const unsigned char character = static_cast<unsigned char>(sql[index]);
                if (std::isalpha(character) != 0) {
                    sql[index] = std::islower(character) != 0
                        ? static_cast<char>(std::toupper(character))
                        : static_cast<char>(std::tolower(character));
                    break;
                }
            }
            break;
    }
}

[[nodiscard]] Corpus generate_corpus(const std::vector<TableMeta>& tables) {
    std::mt19937 engine{kSeed};
    Corpus corpus;
    corpus.guaranteed.reserve(kGuaranteedCount);
    corpus.general.reserve(kGeneralCount);
    for (std::size_t iteration = 0; iteration < kGuaranteedCount; ++iteration) {
        corpus.guaranteed.push_back(generate_guaranteed_case(engine, tables, iteration));
    }
    for (std::size_t iteration = 0; iteration < kGeneralCount; ++iteration) {
        std::string original = valid_sql(engine, tables, iteration);
        std::string mutated = original;
        const int mutation_count = random_int(engine, 1, 3);
        for (int mutation = 0; mutation < mutation_count; ++mutation) {
            apply_general_mutation(engine, mutated);
        }
        corpus.general.push_back(GeneralCase{std::move(original), std::move(mutated)});
    }
    return corpus;
}

[[nodiscard]] bool valid_error(const CompileError& error) {
    const bool valid_kind = error.kind == CompileErrorKind::kLex ||
                            error.kind == CompileErrorKind::kSyntax ||
                            error.kind == CompileErrorKind::kSemantic;
    return valid_kind && error.location.line >= 1 && error.location.column >= 1 &&
           !error.message.empty();
}

[[nodiscard]] bool valid_expression(const Expr& expression) {
    if (const auto* binary = std::get_if<Binary>(&expression.kind)) {
        return binary->lhs != nullptr && binary->rhs != nullptr &&
               valid_expression(*binary->lhs) && valid_expression(*binary->rhs);
    }
    if (const auto* unary = std::get_if<Unary>(&expression.kind)) {
        return unary->operand != nullptr && valid_expression(*unary->operand);
    }
    return true;
}

[[nodiscard]] bool valid_plan_node(const PlanNode& node) {
    if (std::holds_alternative<SeqScanNode>(node.kind)) {
        return true;
    }
    if (const auto* filter = std::get_if<FilterNode>(&node.kind)) {
        return filter->child != nullptr && valid_expression(filter->predicate) &&
               valid_plan_node(*filter->child);
    }
    const auto& project = std::get<ProjectNode>(node.kind);
    return project.child != nullptr && valid_plan_node(*project.child);
}

[[nodiscard]] bool valid_plan(const Plan& plan) {
    if (const auto* create = std::get_if<CreateTablePlan>(&plan.kind)) {
        return !create->table_name.empty() && !create->columns.empty();
    }
    if (const auto* insert = std::get_if<InsertPlan>(&plan.kind)) {
        if (insert->rows.empty()) {
            return false;
        }
        const std::size_t width = insert->rows.front().size();
        return width != 0 && std::all_of(
            insert->rows.begin(),
            insert->rows.end(),
            [width](const std::vector<Value>& row) { return row.size() == width; });
    }
    if (const auto* deletion = std::get_if<DeletePlan>(&plan.kind)) {
        return !deletion->predicate.has_value() || valid_expression(*deletion->predicate);
    }
    const auto& query = std::get<QueryPlan>(plan.kind);
    return query.root != nullptr &&
           std::holds_alternative<ProjectNode>(query.root->kind) &&
           valid_plan_node(*query.root);
}

[[nodiscard]] bool same_result(const CompileResult& lhs, const CompileResult& rhs) {
    const auto* lhs_error = std::get_if<CompileError>(&lhs.outcome);
    const auto* rhs_error = std::get_if<CompileError>(&rhs.outcome);
    if (lhs_error != nullptr || rhs_error != nullptr) {
        return lhs_error != nullptr && rhs_error != nullptr &&
               lhs_error->kind == rhs_error->kind &&
               lhs_error->location.line == rhs_error->location.line &&
               lhs_error->location.column == rhs_error->location.column &&
               lhs_error->message == rhs_error->message;
    }
    return format_plan(std::get<Plan>(lhs.outcome)) ==
           format_plan(std::get<Plan>(rhs.outcome));
}

void add_result(ResultCounts& counts, const CompileResult& result) {
    const auto* error = std::get_if<CompileError>(&result.outcome);
    if (error == nullptr) {
        ++counts.success;
    } else if (error->kind == CompileErrorKind::kLex) {
        ++counts.lex;
    } else if (error->kind == CompileErrorKind::kSyntax) {
        ++counts.syntax;
    } else {
        ++counts.semantic;
    }
}

[[nodiscard]] GuaranteedStageCounts count_expected_stages(
    const std::vector<GuaranteedCase>& cases) {
    GuaranteedStageCounts counts;
    for (const GuaranteedCase& item : cases) {
        if (!item.expected_kind.has_value()) {
            ++counts.unspecified;
        } else if (*item.expected_kind == CompileErrorKind::kLex) {
            ++counts.lex;
        } else if (*item.expected_kind == CompileErrorKind::kSyntax) {
            ++counts.syntax;
        } else {
            ++counts.semantic;
        }
    }
    return counts;
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

void print_guaranteed_context(
    std::size_t iteration,
    const GuaranteedCase& item) {
    std::cerr << "seed=" << kSeed << " iteration=" << iteration
              << " mutation=" << mutation_name(item.kind)
              << "\noriginal: " << item.original
              << "\nmutated: " << item.mutated << '\n';
}

void print_general_context(std::size_t iteration, const GeneralCase& item) {
    std::cerr << "seed=" << kSeed << " iteration=" << iteration
              << "\noriginal: " << item.original
              << "\nmutated: " << item.mutated << '\n';
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
    const Corpus corpus = generate_corpus(tables);
    const Corpus replay = generate_corpus(tables);

    for (std::size_t index = 0; index < 100; ++index) {
        const GuaranteedCase& first = corpus.guaranteed[index];
        const GuaranteedCase& second = replay.guaranteed[index];
        if (first.kind != second.kind || first.original != second.original ||
            first.mutated != second.mutated) {
            std::cerr << "guaranteed reproducibility failure: seed=" << kSeed
                      << " index=" << index << '\n';
            return 1;
        }
        if (corpus.general[index].original != replay.general[index].original ||
            corpus.general[index].mutated != replay.general[index].mutated) {
            std::cerr << "general reproducibility failure: seed=" << kSeed
                      << " index=" << index << '\n';
            return 1;
        }
    }

    ResultCounts guaranteed_results;
    ResultCounts general_results;
    const GuaranteedStageCounts expected_stages = count_expected_stages(corpus.guaranteed);

    for (std::size_t iteration = 0; iteration < corpus.guaranteed.size(); ++iteration) {
        const GuaranteedCase& item = corpus.guaranteed[iteration];
        const CompileResult original = compile(CompileRequest{item.original, catalog});
        const auto* original_plan = std::get_if<Plan>(&original.outcome);
        if (original_plan == nullptr || !valid_plan(*original_plan)) {
            print_guaranteed_context(iteration, item);
            std::cerr << "generator produced an invalid original SQL\n";
            return 1;
        }

        const CompileResult result = compile(CompileRequest{item.mutated, catalog});
        add_result(guaranteed_results, result);
        const auto* error = std::get_if<CompileError>(&result.outcome);
        if (error == nullptr) {
            print_guaranteed_context(iteration, item);
            std::cerr << "Wrong Accept:\n" << format_plan(std::get<Plan>(result.outcome));
            return 1;
        }
        if (!valid_error(*error)) {
            print_guaranteed_context(iteration, item);
            std::cerr << "invalid CompileError\n";
            return 1;
        }
        if (item.expected_kind.has_value() && error->kind != *item.expected_kind) {
            print_guaranteed_context(iteration, item);
            std::cerr << "error stage mismatch: expected="
                      << static_cast<int>(*item.expected_kind)
                      << " actual=" << static_cast<int>(error->kind)
                      << " location=" << error->location.line << ':'
                      << error->location.column << " message=" << error->message << '\n';
            return 1;
        }
        if (iteration % 113 == 0) {
            const CompileResult repeated = compile(CompileRequest{item.mutated, catalog});
            if (!same_result(result, repeated)) {
                print_guaranteed_context(iteration, item);
                std::cerr << "guaranteed result is not deterministic\n";
                return 1;
            }
        }
        if (iteration % 73 == 0) {
            const CompileResult recovery = compile(CompileRequest{
                "SELECT id FROM student;",
                catalog
            });
            const auto* recovery_plan = std::get_if<Plan>(&recovery.outcome);
            if (recovery_plan == nullptr || !valid_plan(*recovery_plan)) {
                print_guaranteed_context(iteration, item);
                std::cerr << "valid compile failed after invalid input\n";
                return 1;
            }
        }
    }

    for (std::size_t iteration = 0; iteration < corpus.general.size(); ++iteration) {
        const GeneralCase& item = corpus.general[iteration];
        const CompileResult result = compile(CompileRequest{item.mutated, catalog});
        add_result(general_results, result);
        if (const auto* error = std::get_if<CompileError>(&result.outcome)) {
            if (!valid_error(*error)) {
                print_general_context(iteration, item);
                std::cerr << "invalid CompileError\n";
                return 1;
            }
        } else {
            const Plan& plan = std::get<Plan>(result.outcome);
            if (!valid_plan(plan)) {
                print_general_context(iteration, item);
                std::cerr << "invalid successful Plan:\n" << format_plan(plan);
                return 1;
            }
        }
        if (iteration % 197 == 0) {
            const CompileResult repeated = compile(CompileRequest{item.mutated, catalog});
            if (!same_result(result, repeated)) {
                print_general_context(iteration, item);
                std::cerr << "general result is not deterministic\n";
                return 1;
            }
        }
        if (iteration % 211 == 0) {
            const CompileResult recovery = compile(CompileRequest{
                "SELECT id FROM student;",
                catalog
            });
            const auto* recovery_plan = std::get_if<Plan>(&recovery.outcome);
            if (recovery_plan == nullptr || !valid_plan(*recovery_plan)) {
                print_general_context(iteration, item);
                std::cerr << "valid compile failed during general mutation sequence\n";
                return 1;
            }
        }
    }

    if (!catalogs_equal(tables, catalog_before)) {
        std::cerr << "seed=" << kSeed << ": CatalogView backing data changed\n";
        return 1;
    }

    if (argc == 2 && std::string_view{argv[1]} == "--stats") {
        std::cout << "seed=" << kSeed
                  << " guaranteed_total=" << corpus.guaranteed.size()
                  << " expected_lex=" << expected_stages.lex
                  << " expected_syntax=" << expected_stages.syntax
                  << " expected_semantic=" << expected_stages.semantic
                  << " expected_unspecified=" << expected_stages.unspecified
                  << " actual_success=" << guaranteed_results.success
                  << " actual_lex=" << guaranteed_results.lex
                  << " actual_syntax=" << guaranteed_results.syntax
                  << " actual_semantic=" << guaranteed_results.semantic
                  << "\ngeneral_total=" << corpus.general.size()
                  << " success=" << general_results.success
                  << " lex=" << general_results.lex
                  << " syntax=" << general_results.syntax
                  << " semantic=" << general_results.semantic << '\n';
    }
    return 0;
}
