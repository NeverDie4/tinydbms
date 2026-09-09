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

const DeleteAst* expect_delete(TestContext& test, std::string_view name, const ParseResult& result) {
    const auto* statement = std::get_if<StatementAst>(&result.outcome);
    test.expect(statement != nullptr, std::string{name} + ": parse success");
    if (statement == nullptr) {
        return nullptr;
    }
    const auto* deletion = std::get_if<DeleteAst>(&statement->kind);
    test.expect(deletion != nullptr, std::string{name} + ": delete AST");
    return deletion;
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
        const auto result = parse_sql(test, "delete all", "DELETE FROM student;");
        const auto* deletion = expect_delete(test, "delete all", result);
        if (deletion != nullptr) {
            test.expect(deletion->table_name == "student", "delete all: table");
            test.expect(deletion->table_location.column == 13, "delete all: table location");
            test.expect(deletion->predicate == nullptr, "delete all: no predicate");
        }
    }

    {
        const auto result = parse_sql(test, "where", "DELETE FROM student WHERE id = 1;");
        const auto* deletion = expect_delete(test, "where", result);
        test.expect(deletion != nullptr && deletion->predicate != nullptr, "where: predicate");
    }

    expect_delete(
        test,
        "complex where",
        parse_sql(test, "complex where", "DELETE FROM student WHERE age < 18 OR name = 'Tom';"));
    {
        const auto result = parse_sql(test, "case", "DeLeTe FrOm Student WhErE ID = 1;");
        const auto* deletion = expect_delete(test, "case", result);
        if (deletion != nullptr) {
            test.expect(deletion->table_name == "student", "case: normalized table");
        }
    }
    expect_delete(
        test,
        "comments",
        parse_sql(test, "comments", "DELETE /*a*/ FROM student\nWHERE /*b*/ id = 1;"));
    expect_delete(test, "unknown table", parse_sql(test, "unknown table", "DELETE FROM unknown;"));
    expect_delete(test, "unknown column", parse_sql(test, "unknown column", "DELETE FROM t WHERE unknown = 1;"));
    expect_delete(test, "type mismatch", parse_sql(test, "type mismatch", "DELETE FROM t WHERE name > 'a';"));

    expect_error(test, "delete only", "DELETE;", {1, 7}, "FROM");
    expect_error(test, "missing from", "DELETE t;", {1, 8}, "FROM");
    expect_error(test, "missing table", "DELETE FROM;", {1, 12}, "identifier");
    expect_error(test, "empty where", "DELETE FROM t WHERE;", {1, 20}, "expression");
    expect_error(test, "missing rhs", "DELETE FROM t WHERE a = ;", {1, 25}, "expression");
    expect_error(test, "missing lhs", "DELETE FROM t WHERE = 1;", {1, 21}, "expression");
    expect_error(test, "missing close paren", "DELETE FROM t WHERE (a = 1;", {1, 27}, "')'");
    expect_error(test, "extra close paren", "DELETE FROM t WHERE a = 1)", {1, 26}, "';'");
    expect_error(test, "missing semicolon", "DELETE FROM t WHERE a = 1", {1, 26}, "';'");
    expect_error(test, "trailing token", "DELETE FROM t; abc", {1, 16}, "end of statement");

    if (test.failures() != 0) {
        std::cerr << test.failures() << " delete parser assertion(s) failed\n";
        return 1;
    }
    return 0;
}
