#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ast.hpp"
#include "lexer.hpp"
#include "parser.hpp"

namespace {

using tinydbms::SourceLocation;
using tinydbms::compiler::CompileError;
using tinydbms::compiler::CompileErrorKind;
using tinydbms::compiler::internal::InsertAst;
using tinydbms::compiler::internal::LexResult;
using tinydbms::compiler::internal::ParseResult;
using tinydbms::compiler::internal::StatementAst;
using tinydbms::compiler::internal::Token;
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

    [[nodiscard]] int failures() const {
        return failures_;
    }

private:
    int failures_{0};
};

ParseResult parse_sql(TestContext& test, std::string_view case_name, std::string_view sql) {
    const LexResult lex_result = tokenize(sql);
    const auto* tokens = std::get_if<std::vector<Token>>(&lex_result.outcome);
    test.expect(tokens != nullptr, std::string{case_name} + ": lexer success");
    if (tokens == nullptr) {
        return ParseResult{std::get<CompileError>(lex_result.outcome)};
    }
    return parse(*tokens);
}

const InsertAst* expect_insert(
    TestContext& test,
    std::string_view case_name,
    const ParseResult& result) {
    const auto* statement = std::get_if<StatementAst>(&result.outcome);
    test.expect(statement != nullptr, std::string{case_name} + ": parse success");
    if (statement == nullptr) {
        return nullptr;
    }

    const auto* insert = std::get_if<InsertAst>(&statement->kind);
    test.expect(insert != nullptr, std::string{case_name} + ": insert AST");
    return insert;
}

void expect_syntax_error(
    TestContext& test,
    std::string_view case_name,
    std::string_view sql,
    SourceLocation expected_location,
    std::string_view message_part) {
    const ParseResult result = parse_sql(test, case_name, sql);
    const auto* error = std::get_if<CompileError>(&result.outcome);
    const std::string prefix{case_name};
    test.expect(error != nullptr, prefix + ": syntax error returned");
    if (error == nullptr) {
        return;
    }

    test.expect(error->kind == CompileErrorKind::kSyntax, prefix + ": error kind");
    test.expect(error->location.line == expected_location.line, prefix + ": error line");
    test.expect(error->location.column == expected_location.column, prefix + ": error column");
    test.expect(error->message.find(message_part) != std::string::npos, prefix + ": error message");
}

const std::int32_t* integer_value(const InsertAst& insert, std::size_t row, std::size_t column) {
    if (row >= insert.rows.size() || column >= insert.rows[row].size()) {
        return nullptr;
    }
    return std::get_if<std::int32_t>(&insert.rows[row][column].value.data);
}

const std::string* string_value(const InsertAst& insert, std::size_t row, std::size_t column) {
    if (row >= insert.rows.size() || column >= insert.rows[row].size()) {
        return nullptr;
    }
    return std::get_if<std::string>(&insert.rows[row][column].value.data);
}

}  // namespace

int main() {
    TestContext test;

    {
        const auto result = parse_sql(test, "omitted columns", "INSERT INTO student VALUES (1);");
        const auto* insert = expect_insert(test, "omitted columns", result);
        if (insert != nullptr) {
            test.expect(insert->table_name == "student", "omitted columns: table");
            test.expect(insert->columns.empty(), "omitted columns: empty column list");
            test.expect(insert->rows.size() == 1, "omitted columns: row count");
            if (insert->rows.size() == 1 && insert->rows[0].size() == 1) {
                const auto* value = integer_value(*insert, 0, 0);
                test.expect(value != nullptr && *value == 1, "omitted columns: integer value");
            }
        }
    }

    {
        const auto result = parse_sql(
            test,
            "explicit columns",
            "INSERT INTO student(id,name) VALUES (1,'Alice');");
        const auto* insert = expect_insert(test, "explicit columns", result);
        if (insert != nullptr && insert->columns.size() == 2 && insert->rows.size() == 1) {
            test.expect(insert->columns[0].name == "id", "explicit columns: first column");
            test.expect(insert->columns[1].name == "name", "explicit columns: second column");
            const auto* id = integer_value(*insert, 0, 0);
            const auto* name = string_value(*insert, 0, 1);
            test.expect(id != nullptr && *id == 1, "explicit columns: integer");
            test.expect(name != nullptr && *name == "Alice", "explicit columns: string");
        }
    }

    {
        const auto result = parse_sql(
            test,
            "case normalization",
            "InSeRt InTo Student(ID,Name) VaLuEs (1,'Alice');");
        const auto* insert = expect_insert(test, "case normalization", result);
        if (insert != nullptr && insert->columns.size() == 2) {
            test.expect(insert->table_name == "student", "case normalization: table");
            test.expect(insert->columns[0].name == "id", "case normalization: first column");
            test.expect(insert->columns[1].name == "name", "case normalization: second column");
        }
    }

    {
        const auto result = parse_sql(
            test,
            "multiple rows",
            "INSERT INTO student(id,name) VALUES (1,'Alice'),(2,'Bob'),(3,'Carol');");
        const auto* insert = expect_insert(test, "multiple rows", result);
        if (insert != nullptr && insert->rows.size() == 3) {
            const auto* first = integer_value(*insert, 0, 0);
            const auto* second = string_value(*insert, 1, 1);
            const auto* third = string_value(*insert, 2, 1);
            test.expect(first != nullptr && *first == 1, "multiple rows: first integer");
            test.expect(second != nullptr && *second == "Bob", "multiple rows: second string");
            test.expect(third != nullptr && *third == "Carol", "multiple rows: third string");
        }
    }

    {
        const auto result = parse_sql(test, "escaped string", "INSERT INTO t(name) VALUES ('Tom''s book');");
        const auto* insert = expect_insert(test, "escaped string", result);
        if (insert != nullptr) {
            const auto* value = string_value(*insert, 0, 0);
            test.expect(value != nullptr && *value == "Tom's book", "escaped string: decoded value");
        }
    }

    {
        const auto result = parse_sql(test, "empty string", "INSERT INTO t(name) VALUES ('');");
        const auto* insert = expect_insert(test, "empty string", result);
        if (insert != nullptr) {
            const auto* value = string_value(*insert, 0, 0);
            test.expect(value != nullptr && value->empty(), "empty string: empty value");
        }
    }

    expect_insert(
        test,
        "comments and formatting",
        parse_sql(
            test,
            "comments and formatting",
            "INSERT /*a*/ INTO student(\n"
            "  id,\n"
            "  name\n"
            ")\n"
            "VALUES\n"
            "  (1, 'Alice'), -- first\n"
            "  (2, 'Bob');"));

    {
        const std::string sql =
            "INSERT INTO student(\n"
            "  id,\n"
            "  name\n"
            ")\n"
            "VALUES (\n"
            "  1,\n"
            "  'Alice'\n"
            ");";
        const auto result = parse_sql(test, "locations", sql);
        const auto* insert = expect_insert(test, "locations", result);
        if (insert != nullptr && insert->columns.size() == 2 && insert->rows.size() == 1) {
            test.expect(insert->table_location.line == 1, "locations: table line");
            test.expect(insert->table_location.column == 13, "locations: table column");
            test.expect(insert->columns[0].location.line == 2, "locations: id line");
            test.expect(insert->columns[0].location.column == 3, "locations: id column");
            test.expect(insert->columns[1].location.line == 3, "locations: name line");
            test.expect(insert->rows[0][0].location.line == 6, "locations: integer line");
            test.expect(insert->rows[0][0].location.column == 3, "locations: integer column");
            test.expect(insert->rows[0][1].location.line == 7, "locations: string line");
            test.expect(insert->rows[0][1].location.column == 3, "locations: string column");
        }
    }

    expect_insert(test, "partial columns", parse_sql(test, "partial columns", "INSERT INTO t(id) VALUES (1);"));
    expect_insert(test, "duplicate columns", parse_sql(test, "duplicate columns", "INSERT INTO t(id,id) VALUES (1,2);"));
    expect_insert(test, "column value mismatch", parse_sql(test, "column value mismatch", "INSERT INTO t(id,name) VALUES (1);"));
    expect_insert(test, "different row lengths", parse_sql(test, "different row lengths", "INSERT INTO t(id,name) VALUES (1,'a'),(2);"));
    expect_insert(test, "potential type mismatch", parse_sql(test, "potential type mismatch", "INSERT INTO t(id,name) VALUES ('abc',123);"));
    expect_insert(test, "unknown table", parse_sql(test, "unknown table", "INSERT INTO unknown VALUES (1);"));

    expect_syntax_error(test, "insert only", "INSERT;", {1, 7}, "INTO");
    expect_syntax_error(test, "missing into", "INSERT student VALUES (1);", {1, 8}, "INTO");
    expect_syntax_error(test, "missing table", "INSERT INTO;", {1, 12}, "identifier");
    expect_syntax_error(test, "missing values", "INSERT INTO t;", {1, 14}, "VALUES");
    expect_syntax_error(test, "empty column list", "INSERT INTO t();", {1, 15}, "identifier");
    expect_syntax_error(test, "trailing column comma", "INSERT INTO t(id,) VALUES (1);", {1, 18}, "identifier");
    expect_syntax_error(test, "missing column comma", "INSERT INTO t(id name) VALUES (1,2);", {1, 18}, "')'");
    expect_syntax_error(test, "missing values keyword", "INSERT INTO t(id) (1);", {1, 19}, "VALUES");
    expect_syntax_error(test, "missing row", "INSERT INTO t(id) VALUES;", {1, 25}, "'('");
    expect_syntax_error(test, "empty row", "INSERT INTO t(id) VALUES ();", {1, 27}, "literal");
    expect_syntax_error(test, "trailing value comma", "INSERT INTO t(id) VALUES (1,);", {1, 29}, "literal");
    expect_syntax_error(test, "leading value comma", "INSERT INTO t(id) VALUES (,1);", {1, 27}, "literal");
    expect_syntax_error(test, "identifier value", "INSERT INTO t(id) VALUES (abc);", {1, 27}, "literal");
    expect_syntax_error(test, "missing row close", "INSERT INTO t(id) VALUES (1;", {1, 28}, "')'");
    expect_syntax_error(test, "row lacks paren", "INSERT INTO t(id) VALUES 1;", {1, 26}, "'('");
    expect_syntax_error(test, "trailing row comma", "INSERT INTO t(id) VALUES (1),;", {1, 30}, "'('");
    expect_syntax_error(test, "missing row comma", "INSERT INTO t(id) VALUES (1) (2);", {1, 30}, "';'");
    expect_syntax_error(test, "missing semicolon", "INSERT INTO t(id) VALUES (1)", {1, 29}, "';'");
    expect_syntax_error(test, "trailing token", "INSERT INTO t VALUES (1); abc", {1, 27}, "end of statement");

    if (test.failures() != 0) {
        std::cerr << test.failures() << " insert parser test assertion(s) failed\n";
        return 1;
    }

    return 0;
}
