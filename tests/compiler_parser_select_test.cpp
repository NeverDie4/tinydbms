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
using namespace tinydbms::compiler::internal;

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

ParseResult parse_sql(TestContext& test, std::string_view name, std::string_view sql) {
    const LexResult lexed = tokenize(sql);
    const auto* tokens = std::get_if<std::vector<Token>>(&lexed.outcome);
    test.expect(tokens != nullptr, std::string{name} + ": lexer success");
    if (tokens == nullptr) {
        return ParseResult{std::get<CompileError>(lexed.outcome)};
    }
    return parse(*tokens);
}

const SelectAst* expect_select(TestContext& test, std::string_view name, const ParseResult& result) {
    const auto* statement = std::get_if<StatementAst>(&result.outcome);
    test.expect(statement != nullptr, std::string{name} + ": parse success");
    if (statement == nullptr) {
        return nullptr;
    }
    const auto* select = std::get_if<SelectAst>(&statement->kind);
    test.expect(select != nullptr, std::string{name} + ": select AST");
    return select;
}

void expect_error(
    TestContext& test,
    std::string_view name,
    std::string_view sql,
    SourceLocation expected,
    std::string_view message_part) {
    const auto result = parse_sql(test, name, sql);
    const auto* error = std::get_if<CompileError>(&result.outcome);
    const std::string prefix{name};
    test.expect(error != nullptr, prefix + ": syntax error");
    if (error != nullptr) {
        test.expect(error->kind == CompileErrorKind::kSyntax, prefix + ": kind");
        test.expect(error->location.line == expected.line, prefix + ": line");
        test.expect(error->location.column == expected.column, prefix + ": column");
        test.expect(error->message.find(message_part) != std::string::npos, prefix + ": message");
    }
}

}  // namespace


int main() {
    TestContext test;

    {
        const auto result = parse_sql(test, "star", "SELECT * FROM student;");
        const auto* select = expect_select(test, "star", result);
        if (select != nullptr) {
            test.expect(select->select_all, "star: select all");
            test.expect(select->columns.empty(), "star: no explicit columns");
            test.expect(select->table_name == "student", "star: table");
            test.expect(select->predicate == nullptr, "star: no predicate");
        }
    }

    {
        const auto result = parse_sql(test, "one column", "SELECT id FROM student;");
        const auto* select = expect_select(test, "one column", result);
        if (select != nullptr) {
            test.expect(!select->select_all, "one column: not all");
            test.expect(select->columns.size() == 1 && select->columns[0].name == "id", "one column: projection");
        }
    }

    {
        const auto result = parse_sql(test, "three columns", "SELECT id,name,age FROM student;");
        const auto* select = expect_select(test, "three columns", result);
        if (select != nullptr && select->columns.size() == 3) {
            test.expect(select->columns[0].name == "id", "three columns: id");
            test.expect(select->columns[1].name == "name", "three columns: name");
            test.expect(select->columns[2].name == "age", "three columns: age");
            test.expect(select->columns[0].location.column == 8, "three columns: id location");
            test.expect(select->columns[1].location.column == 11, "three columns: name location");
            test.expect(select->table_location.column == 25, "three columns: table location");
        }
    }

    {
        const auto result = parse_sql(test, "case", "SeLeCt ID,Name FrOm Student;");
        const auto* select = expect_select(test, "case", result);
        if (select != nullptr && select->columns.size() == 2) {
            test.expect(select->columns[0].name == "id", "case: id");
            test.expect(select->columns[1].name == "name", "case: name");
            test.expect(select->table_name == "student", "case: table");
        }
    }

    {
        const auto result = parse_sql(test, "where", "SELECT id FROM student WHERE age >= 18;");
        const auto* select = expect_select(test, "where", result);
        test.expect(select != nullptr && select->predicate != nullptr, "where: predicate");
    }

    expect_select(
        test,
        "complex where",
        parse_sql(test, "complex where", "SELECT id,name\nFROM student\nWHERE age >= 18 AND name != 'Tom';"));
    expect_select(test, "star where", parse_sql(test, "star where", "SELECT * FROM student WHERE id = 1;"));
    expect_select(
        test,
        "comments",
        parse_sql(test, "comments", "SELECT /*a*/ id, -- first\nname FROM student /*b*/ WHERE id = 1;"));

    expect_select(test, "unknown table", parse_sql(test, "unknown table", "SELECT unknown FROM no_table;"));
    expect_select(test, "duplicate projection", parse_sql(test, "duplicate projection", "SELECT id,id FROM student;"));
    expect_select(test, "type mismatch", parse_sql(test, "type mismatch", "SELECT name FROM student WHERE age = 'abc';"));
    expect_select(test, "unknown predicate column", parse_sql(test, "unknown predicate column", "SELECT id FROM student WHERE unknown = 1;"));

    expect_error(test, "select only", "SELECT;", {1, 7}, "column");
    expect_error(test, "missing select list", "SELECT FROM t;", {1, 8}, "column");
    expect_error(test, "star missing from", "SELECT *;", {1, 9}, "FROM");
    expect_error(test, "missing from keyword", "SELECT * t;", {1, 10}, "FROM");
    expect_error(test, "missing projection comma", "SELECT id name FROM t;", {1, 11}, "FROM");
    expect_error(test, "trailing projection comma", "SELECT id, FROM t;", {1, 12}, "identifier");
    expect_error(test, "leading projection comma", "SELECT ,id FROM t;", {1, 8}, "column");
    expect_error(test, "star then column", "SELECT *,id FROM t;", {1, 9}, "FROM");
    expect_error(test, "column then star", "SELECT id,* FROM t;", {1, 11}, "identifier");
    expect_error(test, "missing table", "SELECT id FROM;", {1, 15}, "identifier");
    expect_error(test, "empty where", "SELECT id FROM t WHERE;", {1, 23}, "expression");
    expect_error(test, "where empty parens", "SELECT id FROM t WHERE ();", {1, 25}, "expression");
    expect_error(test, "comparison missing rhs", "SELECT id FROM t WHERE a = ;", {1, 28}, "expression");
    expect_error(test, "comparison missing lhs", "SELECT id FROM t WHERE = 1;", {1, 24}, "expression");
    expect_error(test, "missing close paren", "SELECT id FROM t WHERE (a = 1;", {1, 30}, "')'");
    expect_error(test, "extra close paren", "SELECT id FROM t WHERE a = 1);", {1, 29}, "';'");
    expect_error(test, "missing semicolon", "SELECT id FROM t", {1, 17}, "';'");
    expect_error(test, "trailing token", "SELECT * FROM t; abc", {1, 18}, "end of statement");

    if (test.failures() != 0) {
        std::cerr << test.failures() << " select parser assertion(s) failed\n";
        return 1;
    }
    return 0;
}
