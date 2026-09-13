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
constexpr std::uint32_t kBigIntOverflowSeed = 20260912U;
constexpr std::size_t kBigIntOverflowCount = 250;
constexpr std::uint32_t kDoubleInvalidSeed = 20260914U;
constexpr std::size_t kDoubleInvalidCount = 300;
constexpr std::uint32_t kBooleanInvalidSeed = 20260916U;
constexpr std::size_t kBooleanInvalidCount = 300;
constexpr std::uint32_t kNullInvalidSeed = 20260918U;
constexpr std::size_t kNullInvalidCount = 400;
constexpr std::uint32_t kUpdateInvalidSeed = 20260920U;
constexpr std::size_t kUpdateInvalidCount = 500;
constexpr std::uint32_t kOrderByInvalidSeed = 20260922U;
constexpr std::size_t kOrderByInvalidCount = 500;
constexpr std::uint32_t kJoinInvalidSeed = 20260924U;
constexpr std::size_t kJoinInvalidCount = 600;
constexpr std::uint32_t kAggregateInvalidSeed = 20260926U;
constexpr std::size_t kAggregateInvalidCount = 700;
constexpr std::uint32_t kDiagnosticsSeed = 20260927U;
constexpr std::size_t kDiagnosticsCount = 1200;

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
    std::optional<CompileStage> expected_stage;
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
                    CompileStage::kSyntax};
        case MutationKind::kIllegalCharacter:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + ';',
                    "SELECT @ " + int_name + " FROM " + table.table_name + ';',
                    CompileStage::kLex};
        case MutationKind::kUnclosedString:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        text_name + " = 'Alice';",
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        text_name + " = 'Alice;",
                    CompileStage::kLex};
        case MutationKind::kUnclosedBlockComment:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + ';',
                    "SELECT " + int_name + " FROM " + table.table_name +
                        "; /* unfinished",
                    CompileStage::kLex};
        case MutationKind::kMissingSelectFrom:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + ';',
                    "SELECT " + int_name + ' ' + table.table_name + ';',
                    CompileStage::kSyntax};
        case MutationKind::kMissingInsertValues:
            return {kind,
                    "INSERT INTO " + table.table_name + " VALUES " +
                        valid_row(engine, table) + ';',
                    "INSERT INTO " + table.table_name + ' ' + valid_row(engine, table) + ';',
                    CompileStage::kSyntax};
        case MutationKind::kMissingDeleteFrom:
            return {kind,
                    "DELETE FROM " + table.table_name + ';',
                    "DELETE " + table.table_name + ';',
                    CompileStage::kSyntax};
        case MutationKind::kMissingCreateTable:
            return {kind,
                    "CREATE TABLE " + create_name + "(a INT);",
                    "CREATE " + create_name + "(a INT);",
                    CompileStage::kSyntax};
        case MutationKind::kDuplicateDelimiter:
            return {kind,
                    "SELECT " + int_name + ',' + text_name + " FROM " +
                        table.table_name + ';',
                    "SELECT " + int_name + ",," + text_name + " FROM " +
                        table.table_name + ';',
                    CompileStage::kSyntax};
        case MutationKind::kBrokenLeftParenthesis:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE (" +
                        int_name + " > 1 AND " + int_name + " = 1);",
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        int_name + " > 1 AND " + int_name + " = 1);",
                    CompileStage::kSyntax};
        case MutationKind::kBrokenRightParenthesis:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE (" +
                        int_name + " > 1 AND " + int_name + " = 1);",
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE (" +
                        int_name + " > 1 AND " + int_name + " = 1;",
                    CompileStage::kSyntax};
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
                    CompileStage::kSemantic};
        case MutationKind::kUnknownColumn:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + ';',
                    "SELECT fuzz_unknown_column FROM " + table.table_name + ';',
                    CompileStage::kSemantic};
        case MutationKind::kTypeMismatch:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        int_name + " = 1;",
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        int_name + " = 'abc';",
                    CompileStage::kSemantic};
        case MutationKind::kVarcharOrdering:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        text_name + " = 'Tom';",
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        text_name + " > 'Tom';",
                    CompileStage::kSemantic};
        case MutationKind::kPartialInsertColumns:
            return {kind,
                    "INSERT INTO " + table.table_name + '(' +
                        table.columns[0].name + ',' + table.columns[1].name + ',' +
                        table.columns[2].name + ") VALUES " + valid_row(engine, table) + ';',
                    "INSERT INTO " + table.table_name + '(' +
                        table.columns[0].name + ',' + table.columns[1].name +
                        ") VALUES (1,'Alice');",
                    CompileStage::kSemantic};
        case MutationKind::kDuplicateInsertColumn:
            return {kind,
                    "INSERT INTO " + table.table_name + " VALUES " +
                        valid_row(engine, table) + ';',
                    "INSERT INTO " + table.table_name + '(' +
                        table.columns[0].name + ',' + table.columns[0].name + ',' +
                        table.columns[2].name + ") VALUES (1,2,3);",
                    CompileStage::kSemantic};
        case MutationKind::kTooFewInsertValues:
            return {kind,
                    "INSERT INTO " + table.table_name + " VALUES " +
                        valid_row(engine, table) + ';',
                    "INSERT INTO " + table.table_name + " VALUES (1,'Alice');",
                    CompileStage::kSemantic};
        case MutationKind::kTooManyInsertValues:
            return {kind,
                    "INSERT INTO " + table.table_name + " VALUES " +
                        valid_row(engine, table) + ';',
                    "INSERT INTO " + table.table_name + " VALUES (1,'Alice',20,100);",
                    CompileStage::kSemantic};
        case MutationKind::kWrongInsertType:
            return {kind,
                    "INSERT INTO " + table.table_name + " VALUES " +
                        valid_row(engine, table) + ';',
                    "INSERT INTO " + table.table_name + " VALUES ('wrong','Alice',20);",
                    CompileStage::kSemantic};
        case MutationKind::kNonBoolWhere:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        int_name + " = 1;",
                    "SELECT " + int_name + " FROM " + table.table_name + " WHERE " +
                        int_name + ';',
                    CompileStage::kSemantic};
        case MutationKind::kUnsupportedStatement:
            return {kind,
                    "SELECT " + int_name + " FROM " + table.table_name + ';',
                    iteration % 3 == 0
                        ? "SELECT " + int_name + " FROM " + table.table_name +
                              " LIMIT 1;"
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
    const bool valid_stage = error.stage == CompileStage::kLex ||
                             error.stage == CompileStage::kSyntax ||
                             error.stage == CompileStage::kSemantic;
    return valid_stage && error.source.begin.line >= 1 && error.source.begin.column >= 1 &&
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
    if (const auto* null_test = std::get_if<NullTest>(&expression.kind)) {
        return null_test->operand != nullptr && valid_expression(*null_test->operand);
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
        if (lhs_error == nullptr || rhs_error == nullptr ||
            lhs_error->stage != rhs_error->stage ||
            lhs_error->source.begin.line != rhs_error->source.begin.line ||
            lhs_error->source.begin.column != rhs_error->source.begin.column ||
            lhs_error->source.begin.byte_offset != rhs_error->source.begin.byte_offset ||
            lhs_error->source.end.line != rhs_error->source.end.line ||
            lhs_error->source.end.column != rhs_error->source.end.column ||
            lhs_error->source.end.byte_offset != rhs_error->source.end.byte_offset ||
            lhs_error->message != rhs_error->message ||
            lhs_error->suggestion != rhs_error->suggestion ||
            lhs_error->fix_it.has_value() != rhs_error->fix_it.has_value()) {
            return false;
        }
        if (!lhs_error->fix_it.has_value()) {
            return true;
        }
        return lhs_error->fix_it->range.begin.line == rhs_error->fix_it->range.begin.line &&
               lhs_error->fix_it->range.begin.column == rhs_error->fix_it->range.begin.column &&
               lhs_error->fix_it->range.end.line == rhs_error->fix_it->range.end.line &&
               lhs_error->fix_it->range.end.column == rhs_error->fix_it->range.end.column &&
               lhs_error->fix_it->replacement == rhs_error->fix_it->replacement;
    }
    return format_plan(std::get<Plan>(lhs.outcome)) ==
           format_plan(std::get<Plan>(rhs.outcome));
}

[[nodiscard]] std::size_t source_offset(
    std::string_view source,
    SourceLocation location) {
    if (location.byte_offset <= source.size()) {
        return location.byte_offset;
    }
    int line = 1;
    int column = 1;
    for (std::size_t offset = 0; offset < source.size(); ++offset) {
        if (line == location.line && column == location.column) {
            return offset;
        }
        if (source[offset] == '\n') {
            ++line;
            column = 1;
        } else if (source[offset] != '\r') {
            ++column;
        }
    }
    return source.size();
}

[[nodiscard]] std::string apply_fix_it(std::string_view source, const FixIt& fix_it) {
    const std::size_t begin = source_offset(source, fix_it.range.begin);
    const std::size_t end = source_offset(source, fix_it.range.end);
    return std::string{source.substr(0, begin)} + fix_it.replacement +
        std::string{source.substr(end)};
}

void add_result(ResultCounts& counts, const CompileResult& result) {
    const auto* error = std::get_if<CompileError>(&result.outcome);
    if (error == nullptr) {
        ++counts.success;
    } else if (error->stage == CompileStage::kLex) {
        ++counts.lex;
    } else if (error->stage == CompileStage::kSyntax) {
        ++counts.syntax;
    } else {
        ++counts.semantic;
    }
}

[[nodiscard]] GuaranteedStageCounts count_expected_stages(
    const std::vector<GuaranteedCase>& cases) {
    GuaranteedStageCounts counts;
    for (const GuaranteedCase& item : cases) {
        if (!item.expected_stage.has_value()) {
            ++counts.unspecified;
        } else if (*item.expected_stage == CompileStage::kLex) {
            ++counts.lex;
        } else if (*item.expected_stage == CompileStage::kSyntax) {
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

[[nodiscard]] std::string generate_overflow_literal(std::mt19937& engine) {
    const int length = random_int(engine, 20, 40);
    std::string literal;
    literal.reserve(static_cast<std::size_t>(length));
    literal += static_cast<char>('1' + random_int(engine, 0, 8));
    for (int index = 1; index < length; ++index) {
        literal += static_cast<char>('0' + random_int(engine, 0, 9));
    }
    return literal;
}

[[nodiscard]] std::string generate_double_range_literal(std::mt19937& engine) {
    const int length = random_int(engine, 310, 360);
    std::string literal;
    literal.reserve(static_cast<std::size_t>(length) + 2U);
    literal += static_cast<char>('1' + random_int(engine, 0, 8));
    for (int index = 1; index < length; ++index) {
        literal += static_cast<char>('0' + random_int(engine, 0, 9));
    }
    literal += ".0";
    return literal;
}

[[nodiscard]] std::string generate_invalid_double_syntax(
    std::mt19937& engine,
    std::size_t iteration) {
    const std::string digits = std::to_string(random_int(engine, 1, 999999));
    switch (iteration % 7U) {
        case 0:
            return '.' + digits;
        case 1:
            return digits + '.';
        case 2:
            return digits + "e3";
        case 3:
            return digits + ".0e3";
        case 4:
            return digits + "..0";
        case 5:
            return '-' + digits + ".5";
        default:
            return '+' + digits + ".5";
    }
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
        if (item.expected_stage.has_value() && error->stage != *item.expected_stage) {
            print_guaranteed_context(iteration, item);
            std::cerr << "error stage mismatch: expected="
                      << static_cast<int>(*item.expected_stage)
                      << " actual=" << static_cast<int>(error->stage)
                      << " location=" << error->source.begin.line << ':'
                      << error->source.begin.column << " message=" << error->message << '\n';
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

    std::mt19937 overflow_engine{kBigIntOverflowSeed};
    for (std::size_t iteration = 0; iteration < kBigIntOverflowCount; ++iteration) {
        const std::string literal = generate_overflow_literal(overflow_engine);
        const CompileResult result = compile(CompileRequest{
            "INSERT INTO student VALUES (" + literal + ",'Alice',20);",
            catalog
        });
        const auto* error = std::get_if<CompileError>(&result.outcome);
        if (error == nullptr || error->stage != CompileStage::kLex) {
            std::cerr << "BIGINT overflow seed=" << kBigIntOverflowSeed
                      << " iteration=" << iteration << " literal=" << literal
                      << " did not produce Lex Error\n";
            return 1;
        }
    }


    std::mt19937 double_engine{kDoubleInvalidSeed};
    std::size_t double_syntax_count = 0;
    std::size_t double_range_count = 0;
    for (std::size_t iteration = 0; iteration < kDoubleInvalidCount; ++iteration) {
        const bool range_case = iteration % 5U == 0U;
        const std::string literal = range_case
            ? generate_double_range_literal(double_engine)
            : generate_invalid_double_syntax(double_engine, iteration);
        const CompileResult result = compile(CompileRequest{
            "SELECT id FROM student WHERE id = " + literal + ";",
            catalog
        });
        const auto* error = std::get_if<CompileError>(&result.outcome);
        if (error == nullptr ||
            (range_case &&
             (error->stage != CompileStage::kLex ||
              error->message != "floating literal out of range"))) {
            std::cerr << "DOUBLE invalid seed=" << kDoubleInvalidSeed
                      << " iteration=" << iteration << " literal=" << literal
                      << " did not produce the expected error\n";
            return 1;
        }
        if (range_case) {
            ++double_range_count;
        } else {
            ++double_syntax_count;
        }
    }

    const std::vector<TableMeta> boolean_tables{
        TableMeta{702U, "boolean_values", {
            ColumnMeta{"flag", Type::kBoolean},
            ColumnMeta{"small", Type::kInt},
            ColumnMeta{"large", Type::kBigInt},
            ColumnMeta{"measure", Type::kDouble},
            ColumnMeta{"label", Type::kVarchar},
        }}
    };
    const CatalogView boolean_catalog{std::span<const TableMeta>{boolean_tables}};
    static constexpr std::array<std::string_view, 15> boolean_invalid_sql{
        "SELECT flag FROM boolean_values WHERE flag < TRUE;",
        "SELECT flag FROM boolean_values WHERE flag <= FALSE;",
        "SELECT flag FROM boolean_values WHERE TRUE > flag;",
        "SELECT flag FROM boolean_values WHERE FALSE >= flag;",
        "SELECT flag FROM boolean_values WHERE flag = 1;",
        "SELECT flag FROM boolean_values WHERE flag = 2147483648;",
        "SELECT flag FROM boolean_values WHERE flag = 1.5;",
        "SELECT flag FROM boolean_values WHERE flag = 'true';",
        "INSERT INTO boolean_values VALUES (1,1,1,1.0,'x');",
        "INSERT INTO boolean_values VALUES ('true',1,1,1.0,'x');",
        "INSERT INTO boolean_values VALUES (TRUE,TRUE,1,1.0,'x');",
        "INSERT INTO boolean_values VALUES (TRUE,1,TRUE,1.0,'x');",
        "INSERT INTO boolean_values VALUES (TRUE,1,1,TRUE,'x');",
        "INSERT INTO boolean_values VALUES (TRUE,1,1,1.0,TRUE);",
        "SELECT flag FROM boolean_values WHERE 1 OR FALSE;",
    };
    std::mt19937 boolean_engine{kBooleanInvalidSeed};
    std::size_t boolean_semantic_count = 0;
    for (std::size_t iteration = 0; iteration < kBooleanInvalidCount; ++iteration) {
        const std::size_t offset = random_index(boolean_engine, boolean_invalid_sql.size());
        const std::string_view sql = boolean_invalid_sql[
            (iteration + offset) % boolean_invalid_sql.size()];
        const CompileResult result = compile(CompileRequest{std::string{sql}, boolean_catalog});
        const auto* error = std::get_if<CompileError>(&result.outcome);
        if (error == nullptr || error->stage != CompileStage::kSemantic) {
            std::cerr << "BOOLEAN invalid seed=" << kBooleanInvalidSeed
                      << " iteration=" << iteration << "\nSQL: " << sql
                      << "\nexpected Semantic Error\n";
            return 1;
        }
        ++boolean_semantic_count;
    }

    const std::vector<TableMeta> null_tables{
        TableMeta{703U, "nullable_values", {
            ColumnMeta{"id", Type::kInt, false},
            ColumnMeta{"note", Type::kVarchar, true},
        }}
    };
    const CatalogView null_catalog{std::span<const TableMeta>{null_tables}};
    static constexpr std::array<std::string_view, 11> null_invalid_sql{
        "CREATE TABLE bad(id INT NOT);",
        "CREATE TABLE bad(id INT NULL NULL);",
        "CREATE TABLE bad(id INT NOT NULL NULL);",
        "CREATE TABLE bad(id INT NULL NOT NULL);",
        "CREATE TABLE bad(NULL INT);",
        "CREATE TABLE bad(IS INT);",
        "SELECT id FROM nullable_values WHERE note IS;",
        "SELECT id FROM nullable_values WHERE note IS NOT;",
        "SELECT id FROM nullable_values WHERE note IS TRUE;",
        "SELECT id FROM nullable_values WHERE note IS NOT FALSE;",
        "INSERT INTO nullable_values VALUES (NULL,'bad');",
    };
    std::mt19937 null_engine{kNullInvalidSeed};
    for (std::size_t iteration = 0; iteration < kNullInvalidCount; ++iteration) {
        const std::string_view sql =
            null_invalid_sql[random_index(null_engine, null_invalid_sql.size())];
        const CompileResult result = compile(CompileRequest{std::string{sql}, null_catalog});
        const auto* error = std::get_if<CompileError>(&result.outcome);
        if (error == nullptr || !valid_error(*error)) {
            std::cerr << "NULL invalid seed=" << kNullInvalidSeed
                      << " iteration=" << iteration << "\nSQL: " << sql
                      << "\nexpected compile error\n";
            return 1;
        }
        const CompileResult repeated = compile(CompileRequest{std::string{sql}, null_catalog});
        if (!same_result(result, repeated)) {
            std::cerr << "NULL invalid determinism failure: seed=" << kNullInvalidSeed
                      << " iteration=" << iteration << '\n';
            return 1;
        }
    }

    const std::vector<TableMeta> update_tables{
        TableMeta{704U,"update_values",{
            {"id",Type::kInt,false},{"big_id",Type::kBigInt,false},
            {"measure",Type::kDouble,false},{"active",Type::kBoolean,true},
            {"note",Type::kVarchar,true}}}};
    const CatalogView update_catalog{std::span<const TableMeta>{update_tables}};
    static constexpr std::array<std::string_view,22> update_invalid_sql{
        "UPDATE;",
        "UPDATE update_values;",
        "UPDATE update_values SET;",
        "UPDATE update_values SET id;",
        "UPDATE update_values SET id=;",
        "UPDATE update_values SET id=1,;",
        "UPDATE update_values SET id=1,,note='x';",
        "UPDATE update_values SET id=1 WHERE;",
        "UPDATE update_values SET id=big_id;",
        "UPDATE update_values SET id=1,note=;",
        "UPDATE missing SET id=1;",
        "UPDATE update_values SET missing=1;",
        "UPDATE update_values SET id=1,id=2;",
        "UPDATE update_values SET id='x';",
        "UPDATE update_values SET id=NULL;",
        "UPDATE update_values SET id=2147483648;",
        "UPDATE update_values SET big_id=1.5;",
        "UPDATE update_values SET big_id=TRUE;",
        "UPDATE update_values SET active=1;",
        "UPDATE update_values SET note=TRUE;",
        "UPDATE update_values SET id=1 WHERE id;",
        "UPDATE update_values SET id=1 WHERE missing=1;"};
    std::mt19937 update_engine{kUpdateInvalidSeed};
    for(std::size_t iteration=0;iteration<kUpdateInvalidCount;++iteration) {
        const std::string_view sql=update_invalid_sql[
            random_index(update_engine,update_invalid_sql.size())];
        const CompileResult result=compile(CompileRequest{std::string{sql},update_catalog});
        const auto* error=std::get_if<CompileError>(&result.outcome);
        if(error==nullptr || !valid_error(*error)) {
            std::cerr<<"UPDATE invalid seed="<<kUpdateInvalidSeed
                     <<" iteration="<<iteration<<"\nSQL: "<<sql
                     <<"\nexpected compile error\n";return 1;
        }
        const CompileResult repeated=compile(CompileRequest{std::string{sql},update_catalog});
        if(!same_result(result,repeated)) {
            std::cerr<<"UPDATE invalid determinism failure: seed="<<kUpdateInvalidSeed
                     <<" iteration="<<iteration<<'\n';return 1;
        }
    }

    static constexpr std::array<std::string_view, 20> order_by_invalid_sql{
        "SELECT id FROM student ORDER;",
        "SELECT id FROM student ORDER id;",
        "SELECT id FROM student ORDER BY;",
        "SELECT id FROM student ORDER BY id,;",
        "SELECT id FROM student ORDER BY ,id;",
        "SELECT id FROM student ORDER BY id ASC DESC;",
        "SELECT id FROM student ORDER BY id DESC ASC;",
        "SELECT id FROM student ORDER BY 1;",
        "SELECT id FROM student ORDER BY TRUE;",
        "SELECT id FROM student ORDER BY NULL;",
        "SELECT id FROM student ORDER BY 1.5;",
        "SELECT id FROM student ORDER BY *;",
        "SELECT id FROM student ORDER BY id = 1;",
        "SELECT id FROM student ORDER BY student.*;",
        "SELECT id FROM student ORDER BY id NULLS FIRST;",
        "SELECT id FROM student ORDER BY id COLLATE binary;",
        "SELECT id FROM student ORDER BY missing;",
        "SELECT id FROM student ORDER BY id WHERE id=1;",
        "DELETE FROM student ORDER BY id;",
        "UPDATE student SET age=1 ORDER BY id;"};
    std::mt19937 order_invalid_engine{kOrderByInvalidSeed};
    for (std::size_t iteration = 0; iteration < kOrderByInvalidCount; ++iteration) {
        const std::string_view sql = order_by_invalid_sql[
            random_index(order_invalid_engine, order_by_invalid_sql.size())];
        const CompileResult result = compile(CompileRequest{std::string{sql}, catalog});
        const auto* error = std::get_if<CompileError>(&result.outcome);
        if (error == nullptr || !valid_error(*error)) {
            std::cerr << "ORDER BY invalid seed=" << kOrderByInvalidSeed
                      << " iteration=" << iteration << "\nSQL: " << sql
                      << "\nexpected compile error\n";
            return 1;
        }
        const CompileResult repeated = compile(CompileRequest{std::string{sql}, catalog});
        if (!same_result(result, repeated)) {
            std::cerr << "ORDER BY invalid determinism failure: seed="
                      << kOrderByInvalidSeed << " iteration=" << iteration << '\n';
            return 1;
        }
    }

    const std::vector<TableMeta> join_tables{
        TableMeta{800U, "lhs_values", {
            {"id", Type::kInt, false}, {"shared", Type::kVarchar, true},
            {"left_only", Type::kInt, false}}},
        TableMeta{801U, "rhs_values", {
            {"id", Type::kInt, true}, {"shared", Type::kVarchar, false},
            {"right_only", Type::kInt, false}}},
        TableMeta{802U, "third_values", {{"id", Type::kInt, false}}}};
    const CatalogView join_catalog{std::span<const TableMeta>{join_tables}};
    static constexpr std::array<std::string_view, 25> join_invalid_sql{
        "SELECT lhs_values.id FROM lhs_values JOIN;",
        "SELECT lhs_values.id FROM lhs_values JOIN rhs_values;",
        "SELECT lhs_values.id FROM lhs_values INNER rhs_values ON TRUE;",
        "SELECT lhs_values.id FROM lhs_values LEFT JOIN rhs_values ON TRUE;",
        "SELECT lhs_values.id FROM lhs_values RIGHT JOIN rhs_values ON TRUE;",
        "SELECT lhs_values.id FROM lhs_values FULL JOIN rhs_values ON TRUE;",
        "SELECT lhs_values.id FROM lhs_values CROSS JOIN rhs_values ON TRUE;",
        "SELECT lhs_values.id FROM lhs_values NATURAL JOIN rhs_values;",
        "SELECT lhs_values.id FROM lhs_values JOIN rhs_values USING(id);",
        "SELECT lhs_values.id FROM lhs_values,rhs_values;",
        "SELECT lhs_values.* FROM lhs_values JOIN rhs_values ON TRUE;",
        "SELECT l.id FROM lhs_values l JOIN rhs_values ON TRUE;",
        "SELECT lhs_values.id FROM lhs_values JOIN rhs_values r ON TRUE;",
        "SELECT missing.id FROM lhs_values JOIN rhs_values ON TRUE;",
        "SELECT rhs_values.missing FROM lhs_values JOIN rhs_values ON TRUE;",
        "SELECT id FROM lhs_values JOIN rhs_values ON lhs_values.id=rhs_values.id;",
        "SELECT lhs_values.id FROM lhs_values JOIN rhs_values ON id=id;",
        "SELECT lhs_values.id FROM lhs_values JOIN rhs_values ON TRUE WHERE shared='x';",
        "SELECT lhs_values.id FROM lhs_values JOIN rhs_values ON TRUE ORDER BY shared;",
        "SELECT lhs_values.id FROM lhs_values JOIN lhs_values ON TRUE;",
        "SELECT lhs_values.id FROM lhs_values JOIN rhs_values ON rhs_values.right_only;",
        "SELECT lhs_values.id FROM lhs_values JOIN missing ON TRUE;",
        "SELECT lhs_values.id FROM lhs_values JOIN rhs_values ON third_values.id=rhs_values.id JOIN third_values ON TRUE;",
        "SELECT lhs_values.id FROM lhs_values JOIN rhs_values ON lhs_values.=rhs_values.id;",
        "SELECT lhs_values.id FROM lhs_values JOIN rhs_values ON lhs_values.id=rhs_values.;"};
    std::mt19937 join_invalid_engine{kJoinInvalidSeed};
    for (std::size_t iteration = 0; iteration < kJoinInvalidCount; ++iteration) {
        const std::string_view sql = join_invalid_sql[
            random_index(join_invalid_engine, join_invalid_sql.size())];
        const CompileResult result = compile(CompileRequest{std::string{sql}, join_catalog});
        const auto* error = std::get_if<CompileError>(&result.outcome);
        if (error == nullptr || !valid_error(*error)) {
            std::cerr << "JOIN invalid seed=" << kJoinInvalidSeed
                      << " iteration=" << iteration << "\nSQL: " << sql
                      << "\nexpected compile error\n";
            return 1;
        }
        const CompileResult repeated = compile(CompileRequest{std::string{sql}, join_catalog});
        if (!same_result(result, repeated)) {
            std::cerr << "JOIN invalid determinism failure: seed=" << kJoinInvalidSeed
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
            {"id", Type::kInt, false}, {"amount", Type::kInt, true}}}};
    const CatalogView aggregate_catalog{std::span<const TableMeta>{aggregate_tables}};
    static constexpr std::array<std::string_view, 38> aggregate_invalid_sql{
        "SELECT SUM(*) FROM aggregate_values;",
        "SELECT AVG(*) FROM aggregate_values;",
        "SELECT COUNT() FROM aggregate_values;",
        "SELECT COUNT(1) FROM aggregate_values;",
        "SELECT SUM(id,id) FROM aggregate_values;",
        "SELECT COUNT(DISTINCT id) FROM aggregate_values;",
        "SELECT SUM(id+id) FROM aggregate_values;",
        "SELECT SUM(COUNT(id)) FROM aggregate_values;",
        "SELECT COUNT(*) total FROM aggregate_values;",
        "SELECT COUNT(*) AS total FROM aggregate_values;",
        "SELECT COUNT(*) FROM aggregate_values HAVING COUNT(*)>0;",
        "SELECT COUNT(*) OVER () FROM aggregate_values;",
        "SELECT note,COUNT(*) FROM aggregate_values;",
        "SELECT note,id,COUNT(*) FROM aggregate_values GROUP BY note;",
        "SELECT * FROM aggregate_values GROUP BY note;",
        "SELECT * FROM aggregate_values GROUP BY note,active;",
        "SELECT note,COUNT(*) FROM aggregate_values GROUP BY active;",
        "SELECT note,COUNT(*) FROM aggregate_values GROUP BY note ORDER BY id;",
        "SELECT COUNT(*) FROM aggregate_values GROUP BY missing;",
        "SELECT COUNT(*) FROM aggregate_values JOIN aggregate_rhs "
        "ON aggregate_values.id=aggregate_rhs.id GROUP BY id;",
        "SELECT SUM(active) FROM aggregate_values;",
        "SELECT SUM(note) FROM aggregate_values;",
        "SELECT AVG(active) FROM aggregate_values;",
        "SELECT AVG(note) FROM aggregate_values;",
        "SELECT MIN(active) FROM aggregate_values;",
        "SELECT MAX(active) FROM aggregate_values;",
        "SELECT SUM(missing) FROM aggregate_values;",
        "SELECT COUNT(missing.id) FROM aggregate_values;",
        "SELECT COUNT(aggregate_rhs.amount) FROM aggregate_values;",
        "SELECT id FROM aggregate_values WHERE COUNT(*)=1;",
        "SELECT id FROM aggregate_values JOIN aggregate_rhs ON COUNT(*)=1;",
        "DELETE FROM aggregate_values WHERE COUNT(*)=1;",
        "UPDATE aggregate_values SET id=COUNT(*);",
        "SELECT COUNT(*) FROM aggregate_values GROUP BY COUNT(*);",
        "SELECT COUNT(*) FROM aggregate_values GROUP BY 1;",
        "SELECT COUNT(*) FROM aggregate_values GROUP id;",
        "SELECT COUNT(*) FROM aggregate_values GROUP BY id,;",
        "SELECT COUNT(*) FROM aggregate_values GROUP BY;"};
    std::mt19937 aggregate_invalid_engine{kAggregateInvalidSeed};
    for (std::size_t iteration = 0; iteration < kAggregateInvalidCount; ++iteration) {
        const std::string_view sql = aggregate_invalid_sql[
            random_index(aggregate_invalid_engine, aggregate_invalid_sql.size())];
        const CompileResult result = compile(CompileRequest{std::string{sql}, aggregate_catalog});
        const auto* error = std::get_if<CompileError>(&result.outcome);
        if (error == nullptr || !valid_error(*error)) {
            std::cerr << "Aggregate invalid seed=" << kAggregateInvalidSeed
                      << " iteration=" << iteration << "\nSQL: " << sql
                      << "\nexpected compile error\n";
            return 1;
        }
        const CompileResult repeated =
            compile(CompileRequest{std::string{sql}, aggregate_catalog});
        if (!same_result(result, repeated)) {
            std::cerr << "Aggregate invalid determinism failure: seed="
                      << kAggregateInvalidSeed << " iteration=" << iteration << '\n';
            return 1;
        }
    }

    struct DiagnosticCase {
        std::string_view category;
        std::string_view sql;
        bool expects_suggestion;
        bool expects_fix_it;
    };
    static constexpr std::array diagnostic_cases{
        DiagnosticCase{"keyword", "SELETC id FROM student;", true, true},
        DiagnosticCase{"comma", "SELECT id name FROM student;", false, true},
        DiagnosticCase{"right_parenthesis", "SELECT COUNT(* FROM student;", false, true},
        DiagnosticCase{"semicolon", "SELECT id FROM student", false, true},
        DiagnosticCase{"table", "SELECT * FROM studnet;", true, false},
        DiagnosticCase{"column", "SELECT naem FROM student;", true, false},
        DiagnosticCase{"qualified", "SELECT student.naem FROM student;", true, false},
        DiagnosticCase{"far", "SELECT * FROM completely_unrelated;", false, false},
    };
    std::array<std::size_t, diagnostic_cases.size()> diagnostic_counts{};
    std::mt19937 diagnostics_engine{kDiagnosticsSeed};
    for (std::size_t iteration = 0; iteration < kDiagnosticsCount; ++iteration) {
        const std::size_t category =
            (iteration + random_index(diagnostics_engine, diagnostic_cases.size())) %
            diagnostic_cases.size();
        ++diagnostic_counts[category];
        const DiagnosticCase& item = diagnostic_cases[category];
        const CompileResult result = compile(CompileRequest{std::string{item.sql}, catalog});
        const CompileResult repeated = compile(CompileRequest{std::string{item.sql}, catalog});
        const auto* error = std::get_if<CompileError>(&result.outcome);
        if (error == nullptr || !valid_error(*error) || !same_result(result, repeated) ||
            error->suggestion.has_value() != item.expects_suggestion ||
            error->fix_it.has_value() != item.expects_fix_it) {
            std::cerr << "Diagnostics fuzz seed=" << kDiagnosticsSeed
                      << " iteration=" << iteration << " category=" << item.category
                      << "\nSQL: " << item.sql << "\nunexpected diagnostic\n";
            return 1;
        }
        if (error->fix_it.has_value()) {
            const std::size_t begin = source_offset(item.sql, error->fix_it->range.begin);
            const std::size_t end = source_offset(item.sql, error->fix_it->range.end);
            if (begin > end || end > item.sql.size()) {
                std::cerr << "Diagnostics fuzz invalid FixIt range: seed="
                          << kDiagnosticsSeed << " iteration=" << iteration << '\n';
                return 1;
            }
            const std::string corrected = apply_fix_it(item.sql, *error->fix_it);
            const CompileResult fixed = compile(CompileRequest{corrected, catalog});
            if (!std::holds_alternative<Plan>(fixed.outcome)) {
                std::cerr << "Diagnostics fuzz FixIt did not compile: seed="
                          << kDiagnosticsSeed << " iteration=" << iteration
                          << "\nSQL: " << item.sql << "\nfixed: " << corrected << '\n';
                return 1;
            }
        }
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
        std::cout << "bigint_overflow_seed=" << kBigIntOverflowSeed
                  << " bigint_overflow_total=" << kBigIntOverflowCount
                  << "\ndouble_invalid_seed=" << kDoubleInvalidSeed
                  << " double_invalid_total=" << kDoubleInvalidCount
                  << " syntax_invalid=" << double_syntax_count
                  << " range_invalid=" << double_range_count
                  << "\nboolean_invalid_seed=" << kBooleanInvalidSeed
                  << " boolean_invalid_total=" << kBooleanInvalidCount
                  << " semantic_invalid=" << boolean_semantic_count
                  << "\nnull_invalid_seed=" << kNullInvalidSeed
                  << " null_invalid_total=" << kNullInvalidCount
                  << "\nupdate_invalid_seed=" << kUpdateInvalidSeed
                  << " update_invalid_total=" << kUpdateInvalidCount
                  << "\norder_by_invalid_seed=" << kOrderByInvalidSeed
                  << " order_by_invalid_total=" << kOrderByInvalidCount
                  << "\njoin_invalid_seed=" << kJoinInvalidSeed
                  << " join_invalid_total=" << kJoinInvalidCount
                  << "\naggregate_invalid_seed=" << kAggregateInvalidSeed
                  << " aggregate_invalid_total=" << kAggregateInvalidCount
                  << "\ndiagnostics_seed=" << kDiagnosticsSeed
                  << " diagnostics_total=" << kDiagnosticsCount;
        for (std::size_t index = 0; index < diagnostic_cases.size(); ++index) {
            std::cout << ' ' << diagnostic_cases[index].category << '='
                      << diagnostic_counts[index];
        }
        std::cout << '\n';
    }
    return 0;
}
