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
using tinydbms::Type;
using tinydbms::compiler::CompileError;
using tinydbms::compiler::CompileErrorKind;
using tinydbms::compiler::internal::CreateTableAst;
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

const CreateTableAst* expect_create(
    TestContext& test,
    std::string_view case_name,
    const ParseResult& result) {
    const auto* statement = std::get_if<StatementAst>(&result.outcome);
    test.expect(statement != nullptr, std::string{case_name} + ": parse success");
    if (statement == nullptr) {
        return nullptr;
    }

    const auto* create = std::get_if<CreateTableAst>(&statement->kind);
    test.expect(create != nullptr, std::string{case_name} + ": create AST");
    return create;
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

}  // namespace

int main() {
    TestContext test;

    {
        const auto result = parse_sql(test, "single column", "CREATE TABLE student(id INT);");
        const auto* create = expect_create(test, "single column", result);
        if (create != nullptr) {
            test.expect(create->table_name == "student", "single column: table name");
            test.expect(create->columns.size() == 1, "single column: column count");
            if (create->columns.size() == 1) {
                test.expect(create->columns[0].name == "id", "single column: column name");
                test.expect(create->columns[0].type == Type::kInt, "single column: column type");
            }
        }
    }

    {
        const auto result = parse_sql(test, "two columns", "CREATE TABLE student(id INT, name VARCHAR);");
        const auto* create = expect_create(test, "two columns", result);
        if (create != nullptr) {
            test.expect(create->columns.size() == 2, "two columns: column count");
            if (create->columns.size() == 2) {
                test.expect(create->columns[0].name == "id", "two columns: first name");
                test.expect(create->columns[0].type == Type::kInt, "two columns: first type");
                test.expect(create->columns[1].name == "name", "two columns: second name");
                test.expect(create->columns[1].type == Type::kVarchar, "two columns: second type");
            }
        }
    }

    {
        const auto result = parse_sql(test, "case normalization", "CrEaTe TaBlE Student(ID int, Name VaRcHaR);");
        const auto* create = expect_create(test, "case normalization", result);
        if (create != nullptr && create->columns.size() == 2) {
            test.expect(create->table_name == "student", "case normalization: table");
            test.expect(create->columns[0].name == "id", "case normalization: first column");
            test.expect(create->columns[1].name == "name", "case normalization: second column");
        }
    }

    expect_create(test, "whitespace", parse_sql(test, "whitespace", "CREATE   TABLE   t ( id INT ) ;"));

    {
        const std::string sql =
            "CREATE TABLE student(\n"
            "  id INT,\n"
            "  name VARCHAR\n"
            ");";
        const auto result = parse_sql(test, "multiline locations", sql);
        const auto* create = expect_create(test, "multiline locations", result);
        if (create != nullptr && create->columns.size() == 2) {
            test.expect(create->table_location.line == 1, "multiline: table line");
            test.expect(create->table_location.column == 14, "multiline: table column");
            test.expect(create->columns[0].location.line == 2, "multiline: id line");
            test.expect(create->columns[0].location.column == 3, "multiline: id column");
            test.expect(create->columns[1].location.line == 3, "multiline: name line");
            test.expect(create->columns[1].location.column == 3, "multiline: name column");
        }
    }

    expect_create(
        test,
        "comments",
        parse_sql(test, "comments", "CREATE /*a*/ TABLE student(\nid INT, -- x\nname VARCHAR\n);"));

    {
        const auto result = parse_sql(test, "duplicate columns remain syntactic", "CREATE TABLE t(id INT, id VARCHAR);");
        const auto* create = expect_create(test, "duplicate columns remain syntactic", result);
        if (create != nullptr) {
            test.expect(create->columns.size() == 2, "duplicate columns: both retained");
        }
    }

    expect_syntax_error(test, "create only", "CREATE;", {1, 7}, "TABLE");
    expect_syntax_error(test, "missing table keyword", "CREATE student(id INT);", {1, 8}, "TABLE");
    expect_syntax_error(test, "missing table name", "CREATE TABLE;", {1, 13}, "identifier");
    expect_syntax_error(test, "missing left paren", "CREATE TABLE t;", {1, 15}, "'('");
    expect_syntax_error(test, "empty columns", "CREATE TABLE t();", {1, 16}, "identifier");
    expect_syntax_error(test, "missing type", "CREATE TABLE t(id);", {1, 18}, "INT or VARCHAR");
    expect_syntax_error(test, "unsupported type", "CREATE TABLE t(id FLOAT);", {1, 19}, "INT or VARCHAR");
    expect_syntax_error(test, "missing comma", "CREATE TABLE t(id INT name VARCHAR);", {1, 23}, "')'");
    expect_syntax_error(test, "trailing comma", "CREATE TABLE t(id INT,);", {1, 23}, "identifier");
    expect_syntax_error(test, "missing right paren", "CREATE TABLE t(id INT;", {1, 22}, "')'");
    expect_syntax_error(test, "extra right paren", "CREATE TABLE t(id INT))", {1, 23}, "';'");
    expect_syntax_error(test, "missing semicolon", "CREATE TABLE t(id INT)", {1, 23}, "';'");
    expect_syntax_error(test, "trailing token", "CREATE TABLE t(id INT); abc", {1, 25}, "end of statement");
    expect_syntax_error(test, "unsupported statement", "VALUES;", {1, 1}, "CREATE");

    {
        const ParseResult result = parse(std::vector<Token>{});
        const auto* error = std::get_if<CompileError>(&result.outcome);
        test.expect(error != nullptr, "empty token stream: error returned");
        if (error != nullptr) {
            test.expect(error->kind == CompileErrorKind::kSyntax, "empty token stream: syntax kind");
            test.expect(error->location.line == 1, "empty token stream: line");
            test.expect(error->location.column == 1, "empty token stream: column");
        }
    }

    if (test.failures() != 0) {
        std::cerr << test.failures() << " parser test assertion(s) failed\n";
        return 1;
    }

    return 0;
}
