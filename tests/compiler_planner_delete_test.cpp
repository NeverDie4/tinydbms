#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "bound_ast.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "planner.hpp"
#include "semantic.hpp"

namespace {

using namespace tinydbms;
using namespace tinydbms::compiler;
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

SemanticResult analyze_sql(
    TestContext& test,
    std::string_view name,
    std::string_view sql,
    CatalogView catalog) {
    const LexResult lexed = tokenize(sql);
    const auto* tokens = std::get_if<std::vector<Token>>(&lexed.outcome);
    test.expect(tokens != nullptr, std::string{name} + ": lexer success");
    if (tokens == nullptr) {
        return SemanticResult{std::get<CompileError>(lexed.outcome)};
    }
    const ParseResult parsed = parse(*tokens);
    const auto* statement = std::get_if<StatementAst>(&parsed.outcome);
    test.expect(statement != nullptr, std::string{name} + ": parser success");
    if (statement == nullptr) {
        return SemanticResult{std::get<CompileError>(parsed.outcome)};
    }
    return analyze(*statement, catalog);
}

const DeletePlan* generate_delete(
    TestContext& test,
    std::string_view name,
    SemanticResult& result,
    Plan& plan) {
    auto* statement = std::get_if<BoundStatement>(&result.outcome);
    test.expect(statement != nullptr, std::string{name} + ": semantic success");
    if (statement == nullptr) {
        return nullptr;
    }
    auto* deletion = std::get_if<BoundDelete>(&statement->kind);
    test.expect(deletion != nullptr, std::string{name} + ": bound DELETE");
    if (deletion == nullptr) {
        return nullptr;
    }
    plan = generate_plan(std::move(*deletion));
    const auto* output = std::get_if<DeletePlan>(&plan.kind);
    test.expect(output != nullptr, std::string{name} + ": public DeletePlan");
    return output;
}

const Binary* binary(const Expr* expression) {
    return expression == nullptr ? nullptr : std::get_if<Binary>(&expression->kind);
}

const Unary* unary(const Expr* expression) {
    return expression == nullptr ? nullptr : std::get_if<Unary>(&expression->kind);
}

bool has_compare(const Binary* expression, CmpOp expected) {
    if (expression == nullptr) {
        return false;
    }
    const auto* op = std::get_if<CmpOp>(&expression->op);
    return op != nullptr && *op == expected;
}

bool has_logic(const Binary* expression, LogicOp expected) {
    if (expression == nullptr) {
        return false;
    }
    const auto* op = std::get_if<LogicOp>(&expression->op);
    return op != nullptr && *op == expected;
}

void expect_column(
    TestContext& test,
    std::string_view name,
    const Expr* expression,
    ColumnId expected) {
    const auto* column = expression == nullptr
        ? nullptr
        : std::get_if<ColumnRef>(&expression->kind);
    test.expect(
        column != nullptr && column->column_id == expected,
        std::string{name} + ": ColumnId");
}

void expect_integer(
    TestContext& test,
    std::string_view name,
    const Expr* expression,
    std::int32_t expected) {
    const auto* literal = expression == nullptr
        ? nullptr
        : std::get_if<Literal>(&expression->kind);
    const auto* value = literal == nullptr
        ? nullptr
        : std::get_if<std::int32_t>(&literal->value.data);
    test.expect(value != nullptr && *value == expected, std::string{name} + ": integer literal");
}

void expect_string(
    TestContext& test,
    std::string_view name,
    const Expr* expression,
    std::string_view expected) {
    const auto* literal = expression == nullptr
        ? nullptr
        : std::get_if<Literal>(&expression->kind);
    const auto* value = literal == nullptr
        ? nullptr
        : std::get_if<std::string>(&literal->value.data);
    test.expect(value != nullptr && *value == expected, std::string{name} + ": string literal");
}

}  // namespace

int main() {
    TestContext test;
    const std::vector<TableMeta> tables{
        TableMeta{7U, "student", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"name", Type::kVarchar},
            ColumnMeta{"age", Type::kInt},
        }}
    };
    const CatalogView catalog{tables};

    {
        SemanticResult result = analyze_sql(
            test, "without WHERE", "DELETE FROM student;", catalog);
        Plan plan{DeletePlan{0U, std::nullopt}};
        const DeletePlan* deletion = generate_delete(test, "without WHERE", result, plan);
        if (deletion != nullptr) {
            test.expect(deletion->table_id == 7U, "without WHERE: TableId");
            test.expect(!deletion->predicate.has_value(), "without WHERE: nullopt predicate");
        }
    }

    {
        SemanticResult result = analyze_sql(
            test, "simple WHERE", "DELETE FROM student WHERE id = 1;", catalog);
        Plan plan{DeletePlan{0U, std::nullopt}};
        const DeletePlan* deletion = generate_delete(test, "simple WHERE", result, plan);
        test.expect(
            deletion != nullptr && deletion->predicate.has_value(),
            "simple WHERE: predicate exists");
        const Binary* comparison = deletion == nullptr || !deletion->predicate.has_value()
            ? nullptr
            : binary(&*deletion->predicate);
        test.expect(has_compare(comparison, CmpOp::kEq), "simple WHERE: equality root");
        if (comparison != nullptr) {
            test.expect(comparison->lhs != nullptr && comparison->rhs != nullptr, "simple WHERE: non-null operands");
            expect_column(test, "simple WHERE lhs", comparison->lhs.get(), 0U);
            expect_integer(test, "simple WHERE rhs", comparison->rhs.get(), 1);
        }
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "complex WHERE",
            "DELETE FROM student WHERE age < 18 OR name = 'Tom';",
            catalog);
        Plan plan{DeletePlan{0U, std::nullopt}};
        const DeletePlan* deletion = generate_delete(test, "complex WHERE", result, plan);
        const Binary* disjunction = deletion == nullptr || !deletion->predicate.has_value()
            ? nullptr
            : binary(&*deletion->predicate);
        test.expect(has_logic(disjunction, LogicOp::kOr), "complex WHERE: OR root");
        if (disjunction != nullptr) {
            const Binary* lhs = binary(disjunction->lhs.get());
            const Binary* rhs = binary(disjunction->rhs.get());
            test.expect(has_compare(lhs, CmpOp::kLt), "complex WHERE: lhs <");
            test.expect(has_compare(rhs, CmpOp::kEq), "complex WHERE: rhs =");
            if (lhs != nullptr && rhs != nullptr) {
                expect_column(test, "complex WHERE age", lhs->lhs.get(), 2U);
                expect_integer(test, "complex WHERE age value", lhs->rhs.get(), 18);
                expect_column(test, "complex WHERE name", rhs->lhs.get(), 1U);
                expect_string(test, "complex WHERE name value", rhs->rhs.get(), "Tom");
            }
        }
    }

    {
        SemanticResult result = analyze_sql(
            test, "NOT", "DELETE FROM student WHERE NOT id = 1;", catalog);
        Plan plan{DeletePlan{0U, std::nullopt}};
        const DeletePlan* deletion = generate_delete(test, "NOT", result, plan);
        const Unary* negation = deletion == nullptr || !deletion->predicate.has_value()
            ? nullptr
            : unary(&*deletion->predicate);
        test.expect(negation != nullptr && negation->op == UnaryOp::kNot, "NOT: Unary root");
        if (negation != nullptr) {
            test.expect(negation->operand != nullptr, "NOT: non-null operand");
            const Binary* comparison = binary(negation->operand.get());
            test.expect(has_compare(comparison, CmpOp::kEq), "NOT: equality operand");
        }
    }

    if (test.failures() != 0) {
        std::cerr << test.failures() << " DELETE planner test assertion(s) failed\n";
        return 1;
    }
    return 0;
}
