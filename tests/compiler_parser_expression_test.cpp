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

const AstExpr* predicate(TestContext& test, std::string_view name, const ParseResult& result) {
    const auto* statement = std::get_if<StatementAst>(&result.outcome);
    test.expect(statement != nullptr, std::string{name} + ": parse success");
    if (statement == nullptr) {
        return nullptr;
    }
    const auto* select = std::get_if<SelectAst>(&statement->kind);
    test.expect(select != nullptr, std::string{name} + ": select AST");
    if (select == nullptr) {
        return nullptr;
    }
    test.expect(select->predicate != nullptr, std::string{name} + ": predicate exists");
    return select->predicate.get();
}

const AstBinaryExpr* binary(const AstExpr* expression) {
    return expression == nullptr ? nullptr : std::get_if<AstBinaryExpr>(&expression->kind);
}

const AstUnaryExpr* unary(const AstExpr* expression) {
    return expression == nullptr ? nullptr : std::get_if<AstUnaryExpr>(&expression->kind);
}

const AstIdentifierExpr* identifier(const AstExpr* expression) {
    return expression == nullptr ? nullptr : std::get_if<AstIdentifierExpr>(&expression->kind);
}

const AstLiteralExpr* literal(const AstExpr* expression) {
    return expression == nullptr ? nullptr : std::get_if<AstLiteralExpr>(&expression->kind);
}

bool has_compare(const AstBinaryExpr* expression, AstCompareOp expected) {
    if (expression == nullptr) {
        return false;
    }
    const auto* op = std::get_if<AstCompareOp>(&expression->op);
    return op != nullptr && *op == expected;
}

bool has_logic(const AstBinaryExpr* expression, AstLogicOp expected) {
    if (expression == nullptr) {
        return false;
    }
    const auto* op = std::get_if<AstLogicOp>(&expression->op);
    return op != nullptr && *op == expected;
}

void expect_expression_success(TestContext& test, std::string_view name, std::string_view expression) {
    const std::string sql = "SELECT * FROM t WHERE " + std::string{expression} + ";";
    const auto result = parse_sql(test, name, sql);
    predicate(test, name, result);
}

void expect_expression_error(
    TestContext& test,
    std::string_view name,
    std::string_view expression,
    SourceLocation expected) {
    const std::string sql = "SELECT * FROM t WHERE " + std::string{expression} + ";";
    const auto result = parse_sql(test, name, sql);
    const auto* error = std::get_if<CompileError>(&result.outcome);
    const std::string prefix{name};
    test.expect(error != nullptr, prefix + ": syntax error");
    if (error != nullptr) {
        test.expect(error->kind == CompileErrorKind::kSyntax, prefix + ": error kind");
        test.expect(error->location.line == expected.line, prefix + ": line");
        test.expect(error->location.column == expected.column, prefix + ": column");
    }
}

}  // namespace

int main() {
    TestContext test;

    {
        const auto result = parse_sql(test, "equal", "SELECT * FROM t WHERE a = 1;");
        const AstExpr* root = predicate(test, "equal", result);
        const auto* comparison = binary(root);
        test.expect(has_compare(comparison, AstCompareOp::kEq), "equal: comparison op");
        if (comparison != nullptr) {
            const auto* lhs = identifier(comparison->lhs.get());
            const auto* rhs = literal(comparison->rhs.get());
            test.expect(lhs != nullptr && lhs->name == "a", "equal: lhs identifier");
            test.expect(rhs != nullptr, "equal: rhs literal");
            if (rhs != nullptr) {
                const auto* value = std::get_if<std::int32_t>(&rhs->value.data);
                test.expect(value != nullptr && *value == 1, "equal: integer value");
                test.expect(rhs->location.column == 27, "equal: literal location");
            }
            test.expect(lhs != nullptr && lhs->location.column == 23, "equal: identifier location");
            test.expect(comparison->location.column == 25, "equal: operator location");
        }
    }

    {
        const auto result = parse_sql(test, "not equal", "SELECT * FROM t WHERE a != 1;");
        test.expect(
            has_compare(binary(predicate(test, "not equal", result)), AstCompareOp::kNe),
            "not equal: comparison op");
    }

    {
        const auto result = parse_sql(test, "less", "SELECT * FROM t WHERE a < 1;");
        test.expect(has_compare(binary(predicate(test, "less", result)), AstCompareOp::kLt), "less: comparison op");
    }

    {
        const auto result = parse_sql(test, "less equal", "SELECT * FROM t WHERE a <= 1;");
        test.expect(
            has_compare(binary(predicate(test, "less equal", result)), AstCompareOp::kLe),
            "less equal: comparison op");
    }

    {
        const auto result = parse_sql(test, "greater", "SELECT * FROM t WHERE a > 1;");
        test.expect(
            has_compare(binary(predicate(test, "greater", result)), AstCompareOp::kGt),
            "greater: comparison op");
    }

    {
        const auto result = parse_sql(test, "greater equal", "SELECT * FROM t WHERE a >= 1;");
        test.expect(
            has_compare(binary(predicate(test, "greater equal", result)), AstCompareOp::kGe),
            "greater equal: comparison op");
    }

    {
        const auto result = parse_sql(test, "and", "SELECT * FROM t WHERE a = 1 AND b = 2;");
        const auto* root = binary(predicate(test, "and", result));
        test.expect(has_logic(root, AstLogicOp::kAnd), "and: root");
        test.expect(root != nullptr && root->location.column == 29, "and: operator location");
    }

    {
        const auto result = parse_sql(test, "or", "SELECT * FROM t WHERE a = 1 OR b = 2;");
        test.expect(has_logic(binary(predicate(test, "or", result)), AstLogicOp::kOr), "or: root");
    }

    {
        const auto result = parse_sql(
            test,
            "and before or",
            "SELECT * FROM t WHERE a = 1 OR b = 2 AND c = 3;");
        const auto* root = binary(predicate(test, "and before or", result));
        test.expect(has_logic(root, AstLogicOp::kOr), "and before or: root OR");
        test.expect(root != nullptr && has_logic(binary(root->rhs.get()), AstLogicOp::kAnd), "and before or: right AND");
    }

    {
        const auto result = parse_sql(
            test,
            "parentheses",
            "SELECT * FROM t WHERE (a = 1 OR b = 2) AND c = 3;");
        const auto* root = binary(predicate(test, "parentheses", result));
        test.expect(has_logic(root, AstLogicOp::kAnd), "parentheses: root AND");
        test.expect(root != nullptr && has_logic(binary(root->lhs.get()), AstLogicOp::kOr), "parentheses: left OR");
    }

    {
        const auto result = parse_sql(test, "not", "SELECT * FROM t WHERE NOT a = 1;");
        const auto* root = unary(predicate(test, "not", result));
        test.expect(root != nullptr && root->op == AstUnaryOp::kNot, "not: root");
        test.expect(root != nullptr && root->location.column == 23, "not: operator location");
        test.expect(root != nullptr && has_compare(binary(root->operand.get()), AstCompareOp::kEq), "not: comparison operand");
    }

    {
        const auto result = parse_sql(test, "double not", "SELECT * FROM t WHERE NOT NOT a = 1;");
        const auto* first = unary(predicate(test, "double not", result));
        test.expect(first != nullptr && unary(first->operand.get()) != nullptr, "double not: two unary nodes");
    }

    {
        const auto result = parse_sql(
            test,
            "full precedence",
            "SELECT * FROM t WHERE NOT a = 1 AND b = 2 OR c = 3;");
        const auto* root = binary(predicate(test, "full precedence", result));
        const auto* left = root == nullptr ? nullptr : binary(root->lhs.get());
        test.expect(has_logic(root, AstLogicOp::kOr), "full precedence: root OR");
        test.expect(has_logic(left, AstLogicOp::kAnd), "full precedence: left AND");
        test.expect(left != nullptr && unary(left->lhs.get()) != nullptr, "full precedence: NOT under AND");
    }

    {
        const auto result = parse_sql(test, "string literal", "SELECT * FROM t WHERE name = 'Alice';");
        const auto* root = binary(predicate(test, "string literal", result));
        const auto* rhs = root == nullptr ? nullptr : literal(root->rhs.get());
        const auto* value = rhs == nullptr ? nullptr : std::get_if<std::string>(&rhs->value.data);
        test.expect(value != nullptr && *value == "Alice", "string literal: value");
    }

    expect_expression_success(test, "type mismatch comparison", "age = 'abc'");
    expect_expression_success(test, "varchar ordering", "name > 'Alice'");
    expect_expression_success(test, "non boolean and", "1 AND 2");
    expect_expression_success(test, "not integer", "NOT 123");
    expect_expression_success(test, "identifier comparison", "id = name");
    expect_expression_success(test, "unknown identifier", "unknown_column = 1");

    expect_expression_error(test, "leading and", "AND a = 1", {1, 23});
    expect_expression_error(test, "trailing and", "a = 1 AND", {1, 32});
    expect_expression_error(test, "leading or", "OR a = 1", {1, 23});
    expect_expression_error(test, "bare not", "NOT", {1, 26});
    expect_expression_error(test, "empty parens", "()", {1, 24});
    expect_expression_error(test, "spaced empty parens", "( )", {1, 25});
    expect_expression_error(test, "double compare", "a = = 1", {1, 27});
    expect_expression_error(test, "missing compare rhs", "a !=", {1, 27});
    expect_expression_error(test, "double or", "a = 1 OR OR b = 2", {1, 32});

    if (test.failures() != 0) {
        std::cerr << test.failures() << " expression parser assertion(s) failed\n";
        return 1;
    }
    return 0;
}
