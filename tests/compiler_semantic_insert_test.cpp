#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "bound_ast.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "semantic.hpp"

namespace {

using tinydbms::ColumnId;
using tinydbms::ColumnMeta;
using tinydbms::SourceLocation;
using tinydbms::TableMeta;
using tinydbms::Type;
using tinydbms::compiler::CatalogView;
using tinydbms::compiler::CompileError;
using tinydbms::compiler::CompileErrorKind;
using tinydbms::compiler::internal::BoundInsert;
using tinydbms::compiler::internal::BoundStatement;
using tinydbms::compiler::internal::LexResult;
using tinydbms::compiler::internal::ParseResult;
using tinydbms::compiler::internal::SemanticResult;
using tinydbms::compiler::internal::StatementAst;
using tinydbms::compiler::internal::Token;
using tinydbms::compiler::internal::analyze;
using tinydbms::compiler::internal::parse;
using tinydbms::compiler::internal::tokenize;

class TestContext {
public:
    void expect(bool condition, std::string_view message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures_;
        }
    }

    [[nodiscard]] int failures() const { return failures_; }

private:
    int failures_{0};
};

SemanticResult analyze_sql(
    TestContext& test,
    std::string_view case_name,
    std::string_view sql,
    CatalogView catalog) {
    const LexResult lex_result = tokenize(sql);
    const auto* tokens = std::get_if<std::vector<Token>>(&lex_result.outcome);
    test.expect(tokens != nullptr, std::string{case_name} + ": lexer success");
    if (tokens == nullptr) {
        return SemanticResult{std::get<CompileError>(lex_result.outcome)};
    }
    const ParseResult parse_result = parse(*tokens);
    const auto* statement = std::get_if<StatementAst>(&parse_result.outcome);
    test.expect(statement != nullptr, std::string{case_name} + ": parser success");
    if (statement == nullptr) {
        return SemanticResult{std::get<CompileError>(parse_result.outcome)};
    }
    return analyze(*statement, catalog);
}

const BoundInsert* expect_insert(
    TestContext& test,
    std::string_view case_name,
    const SemanticResult& result) {
    const auto* statement = std::get_if<BoundStatement>(&result.outcome);
    test.expect(statement != nullptr, std::string{case_name} + ": semantic success");
    if (statement == nullptr) {
        return nullptr;
    }
    const auto* insert = std::get_if<BoundInsert>(&statement->kind);
    test.expect(insert != nullptr, std::string{case_name} + ": bound INSERT");
    return insert;
}

void expect_semantic_error(
    TestContext& test,
    std::string_view case_name,
    std::string_view sql,
    CatalogView catalog,
    SourceLocation location,
    std::string_view message_part) {
    const SemanticResult result = analyze_sql(test, case_name, sql, catalog);
    const auto* error = std::get_if<CompileError>(&result.outcome);
    const std::string prefix{case_name};
    test.expect(error != nullptr, prefix + ": semantic error returned");
    if (error == nullptr) {
        return;
    }
    test.expect(error->kind == CompileErrorKind::kSemantic, prefix + ": error kind");
    test.expect(error->location.line == location.line, prefix + ": error line");
    test.expect(error->location.column == location.column, prefix + ": error column");
    test.expect(error->message.find(message_part) != std::string::npos, prefix + ": error message");
}

const std::int32_t* integer_value(const BoundInsert& insert, std::size_t row, std::size_t column) {
    return std::get_if<std::int32_t>(&insert.rows[row][column].data);
}

const std::string* string_value(const BoundInsert& insert, std::size_t row, std::size_t column) {
    return std::get_if<std::string>(&insert.rows[row][column].data);
}

}  // namespace

int main() {
    TestContext test;
    const std::vector<TableMeta> tables{
        TableMeta{
            7U,
            "student",
            {
                ColumnMeta{"id", Type::kInt},
                ColumnMeta{"name", Type::kVarchar},
                ColumnMeta{"age", Type::kInt},
            }}
    };
    const CatalogView catalog{tables};

    {
        const auto result = analyze_sql(test, "omitted columns", "INSERT INTO student VALUES (1,'Alice',20);", catalog);
        const auto* insert = expect_insert(test, "omitted columns", result);
        if (insert != nullptr) {
            test.expect(insert->table_id == 7U, "omitted columns: table id");
            test.expect(insert->columns.empty(), "omitted columns: empty bound columns");
            test.expect(insert->rows.size() == 1 && insert->rows[0].size() == 3, "omitted columns: row shape");
            if (insert->rows.size() == 1 && insert->rows[0].size() == 3) {
                const auto* id = integer_value(*insert, 0, 0);
                const auto* name = string_value(*insert, 0, 1);
                const auto* age = integer_value(*insert, 0, 2);
                test.expect(id != nullptr && *id == 1, "omitted columns: id");
                test.expect(name != nullptr && *name == "Alice", "omitted columns: name");
                test.expect(age != nullptr && *age == 20, "omitted columns: age");
            }
        }
    }

    {
        const auto result = analyze_sql(
            test, "schema order", "INSERT INTO student(id,name,age) VALUES (1,'Alice',20);", catalog);
        const auto* insert = expect_insert(test, "schema order", result);
        if (insert != nullptr) {
            test.expect(insert->columns == std::vector<ColumnId>{0U, 1U, 2U}, "schema order: ids");
        }
    }

    {
        const auto result = analyze_sql(
            test, "explicit reorder", "INSERT INTO student(age,id,name) VALUES (20,1,'Alice');", catalog);
        const auto* insert = expect_insert(test, "explicit reorder", result);
        if (insert != nullptr) {
            test.expect(insert->columns == std::vector<ColumnId>{2U, 0U, 1U}, "explicit reorder: ids");
            const auto* first = integer_value(*insert, 0, 0);
            const auto* last = string_value(*insert, 0, 2);
            test.expect(first != nullptr && *first == 20, "explicit reorder: row order");
            test.expect(last != nullptr && *last == "Alice", "explicit reorder: string order");
        }
    }

    expect_insert(
        test,
        "multiple rows",
        analyze_sql(test, "multiple rows", "INSERT INTO student VALUES (1,'Alice',20),(2,'Bob',21);", catalog));
    expect_insert(
        test,
        "reordered multiple rows",
        analyze_sql(
            test,
            "reordered multiple rows",
            "INSERT INTO student(name,age,id) VALUES ('Alice',20,1),('Bob',21,2);",
            catalog));

    expect_semantic_error(test, "unknown table", "INSERT INTO unknown VALUES (1);", catalog, {1, 13}, "does not exist");
    expect_semantic_error(
        test, "unknown column", "INSERT INTO student(id,score,age) VALUES (1,'A',20);", catalog, {1, 24}, "column 'score' does not exist");
    expect_semantic_error(
        test, "duplicate column", "INSERT INTO student(id,id,age) VALUES (1,2,20);", catalog, {1, 24}, "duplicate column 'id'");
    expect_semantic_error(
        test, "partial columns", "INSERT INTO student(id,name) VALUES (1,'Alice');", catalog, {1, 13}, "must contain all columns");
    expect_semantic_error(
        test, "single partial column", "INSERT INTO student(id) VALUES (1);", catalog, {1, 13}, "missing column 'name'");

    expect_semantic_error(
        test, "too few values", "INSERT INTO student VALUES (1,'Alice');", catalog, {1, 29}, "value count");
    expect_semantic_error(
        test, "too many values", "INSERT INTO student VALUES (1,'Alice',20,999);", catalog, {1, 42}, "value count");
    expect_semantic_error(
        test,
        "second row too short",
        "INSERT INTO student VALUES\n(1,'Alice',20),\n(2,'Bob');",
        catalog,
        {3, 2},
        "value count");
    expect_semantic_error(
        test,
        "reordered values too short",
        "INSERT INTO student(age,id,name) VALUES (20,1);",
        catalog,
        {1, 42},
        "value count");

    expect_semantic_error(
        test,
        "INT receives VARCHAR",
        "INSERT INTO student VALUES ('Alice','Bob',20);",
        catalog,
        {1, 29},
        "column 'id' expects INT, but VARCHAR found");
    expect_semantic_error(
        test,
        "VARCHAR receives INT",
        "INSERT INTO student VALUES (1,123,20);",
        catalog,
        {1, 31},
        "column 'name' expects VARCHAR, but INT found");
    expect_insert(
        test,
        "reordered types valid",
        analyze_sql(test, "reordered types valid", "INSERT INTO student(name,age,id) VALUES ('Alice',20,1);", catalog));
    expect_semantic_error(
        test,
        "reordered first type invalid",
        "INSERT INTO student(name,age,id) VALUES (1,20,'Alice');",
        catalog,
        {1, 42},
        "column 'name' expects VARCHAR, but INT found");
    expect_semantic_error(
        test,
        "second row type invalid",
        "INSERT INTO student VALUES\n(1,'Alice',20),\n(2,123,21);",
        catalog,
        {3, 4},
        "column 'name' expects VARCHAR, but INT found");

    expect_semantic_error(
        test,
        "error priority",
        "INSERT INTO student(unknown,id,id) VALUES ('x');",
        catalog,
        {1, 21},
        "column 'unknown' does not exist");

    if (test.failures() != 0) {
        std::cerr << test.failures() << " INSERT semantic test assertion(s) failed\n";
        return 1;
    }
    return 0;
}
