#include <cstddef>
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
    CatalogView catalog,
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
    PlannerResult planned = generate_plan(std::move(*deletion), catalog);
    auto* generated = std::get_if<Plan>(&planned.outcome);
    test.expect(generated != nullptr, std::string{name} + ": planner success");
    if (generated == nullptr) {
        return nullptr;
    }
    plan = std::move(*generated);
    const auto* output = std::get_if<DeletePlan>(&plan.kind);
    test.expect(output != nullptr, std::string{name} + ": public DeletePlan");
    return output;
}

const UpdatePlan* generate_update(
    TestContext& test,
    std::string_view name,
    SemanticResult& result,
    CatalogView catalog,
    Plan& plan) {
    auto* statement = std::get_if<BoundStatement>(&result.outcome);
    test.expect(statement != nullptr, std::string{name} + ": semantic success");
    auto* update = statement == nullptr ? nullptr : std::get_if<BoundUpdate>(&statement->kind);
    test.expect(update != nullptr, std::string{name} + ": bound UPDATE");
    if (update == nullptr) {
        return nullptr;
    }
    PlannerResult planned = generate_plan(std::move(*update), catalog);
    auto* generated = std::get_if<Plan>(&planned.outcome);
    test.expect(generated != nullptr, std::string{name} + ": planner success");
    if (generated == nullptr) {
        return nullptr;
    }
    plan = std::move(*generated);
    const auto* output = std::get_if<UpdatePlan>(&plan.kind);
    test.expect(output != nullptr, std::string{name} + ": public UpdatePlan");
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

bool slot_is_mapped(const std::vector<ScanColumn>& columns, SlotId slot_id) {
    for (const ScanColumn& column : columns) {
        if (column.output_slot == slot_id) {
            return true;
        }
    }
    return false;
}

bool expression_slots_are_mapped(
    const Expr& expression,
    const std::vector<ScanColumn>& columns) {
    if (const auto* column = std::get_if<ColumnRef>(&expression.kind)) {
        return slot_is_mapped(columns, column->slot_id);
    }
    if (const auto* binary_expression = std::get_if<Binary>(&expression.kind)) {
        return binary_expression->lhs != nullptr && binary_expression->rhs != nullptr &&
            expression_slots_are_mapped(*binary_expression->lhs, columns) &&
            expression_slots_are_mapped(*binary_expression->rhs, columns);
    }
    if (const auto* unary_expression = std::get_if<Unary>(&expression.kind)) {
        return unary_expression->operand != nullptr &&
            expression_slots_are_mapped(*unary_expression->operand, columns);
    }
    if (const auto* null_test = std::get_if<NullTest>(&expression.kind)) {
        return null_test->operand != nullptr &&
            expression_slots_are_mapped(*null_test->operand, columns);
    }
    return true;
}

void expect_input_mapping(
    TestContext& test,
    std::string_view name,
    const DeletePlan& deletion) {
    test.expect(deletion.input_columns.size() == 3U,
                std::string{name} + ": all input columns");
    for (std::size_t index = 0; index < deletion.input_columns.size(); ++index) {
        test.expect(
            deletion.input_columns[index].column_id == static_cast<ColumnId>(index) &&
                deletion.input_columns[index].output_slot == static_cast<SlotId>(index),
            std::string{name} + ": deterministic input mapping");
    }
}

void expect_column(
    TestContext& test,
    std::string_view name,
    const Expr* expression,
    SlotId expected) {
    const auto* column = expression == nullptr
        ? nullptr
        : std::get_if<ColumnRef>(&expression->kind);
    test.expect(
        column != nullptr && column->slot_id == expected,
        std::string{name} + ": SlotId");
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
        Plan plan{DeletePlan{0U, {}, std::nullopt}};
        const DeletePlan* deletion = generate_delete(
            test, "without WHERE", result, catalog, plan);
        if (deletion != nullptr) {
            test.expect(deletion->table_id == 7U, "without WHERE: TableId");
            test.expect(!deletion->predicate.has_value(), "without WHERE: nullopt predicate");
            expect_input_mapping(test, "without WHERE", *deletion);
        }
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "UPDATE slot boundary",
            "UPDATE student SET name='Alice',age=21 WHERE id=1;",
            catalog);
        Plan plan{UpdatePlan{0U, {}, {}, std::nullopt}};
        const UpdatePlan* update = generate_update(
            test, "UPDATE slot boundary", result, catalog, plan);
        test.expect(update != nullptr && update->input_columns.size() == 3U,
                    "UPDATE slot boundary: complete input mapping");
        test.expect(update != nullptr && update->assignments.size() == 2U &&
                        update->assignments[0].column_id == 1U &&
                        update->assignments[1].column_id == 2U,
                    "UPDATE slot boundary: assignments retain ColumnId and order");
        test.expect(update != nullptr && update->predicate.has_value() &&
                        expression_slots_are_mapped(
                            *update->predicate, update->input_columns),
                    "UPDATE slot boundary: predicate uses mapped SlotId");
        if (update != nullptr && update->predicate.has_value()) {
            const Binary* comparison = binary(&*update->predicate);
            if (comparison != nullptr) {
                expect_column(test, "UPDATE predicate", comparison->lhs.get(), 0U);
            }
        }
    }
    {
        SemanticResult result = analyze_sql(
            test, "UPDATE all", "UPDATE student SET age=20;", catalog);
        Plan plan{UpdatePlan{0U, {}, {}, std::nullopt}};
        const UpdatePlan* update = generate_update(test, "UPDATE all", result, catalog, plan);
        test.expect(update != nullptr && !update->predicate.has_value(),
                    "UPDATE all: absent predicate");
        test.expect(update != nullptr && update->input_columns.size() == 3U,
                    "UPDATE all: input mapping retained");
    }

    {
        SemanticResult result = analyze_sql(
            test, "IS NOT NULL", "DELETE FROM student WHERE name IS NOT NULL;", catalog);
        Plan plan{DeletePlan{0U, {}, std::nullopt}};
        const DeletePlan* deletion = generate_delete(
            test, "IS NOT NULL", result, catalog, plan);
        const auto* null_test = deletion == nullptr || !deletion->predicate.has_value()
            ? nullptr
            : std::get_if<NullTest>(&deletion->predicate->kind);
        test.expect(
            null_test != nullptr && null_test->op == NullTestOp::kIsNotNull,
            "IS NOT NULL: dedicated public node");
        test.expect(
            deletion != nullptr && null_test != nullptr &&
                expression_slots_are_mapped(*deletion->predicate, deletion->input_columns),
            "IS NOT NULL: operand slot mapped by delete input");
    }

    {
        SemanticResult result = analyze_sql(
            test, "simple WHERE", "DELETE FROM student WHERE id = 1;", catalog);
        Plan plan{DeletePlan{0U, {}, std::nullopt}};
        const DeletePlan* deletion = generate_delete(
            test, "simple WHERE", result, catalog, plan);
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
        if (deletion != nullptr) {
            expect_input_mapping(test, "simple WHERE", *deletion);
            test.expect(
                !deletion->predicate.has_value() ||
                    expression_slots_are_mapped(
                        *deletion->predicate,
                        deletion->input_columns),
                "simple WHERE: predicate slots are mapped");
        }
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "complex WHERE",
            "DELETE FROM student WHERE age < 18 OR name = 'Tom';",
            catalog);
        Plan plan{DeletePlan{0U, {}, std::nullopt}};
        const DeletePlan* deletion = generate_delete(
            test, "complex WHERE", result, catalog, plan);
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
        if (deletion != nullptr) {
            expect_input_mapping(test, "complex WHERE", *deletion);
            test.expect(
                !deletion->predicate.has_value() ||
                    expression_slots_are_mapped(
                        *deletion->predicate,
                        deletion->input_columns),
                "complex WHERE: predicate slots are mapped");
        }
    }

    {
        SemanticResult result = analyze_sql(
            test, "NOT", "DELETE FROM student WHERE NOT id = 1;", catalog);
        Plan plan{DeletePlan{0U, {}, std::nullopt}};
        const DeletePlan* deletion = generate_delete(test, "NOT", result, catalog, plan);
        const Unary* negation = deletion == nullptr || !deletion->predicate.has_value()
            ? nullptr
            : unary(&*deletion->predicate);
        test.expect(negation != nullptr && negation->op == UnaryOp::kNot, "NOT: Unary root");
        if (negation != nullptr) {
            test.expect(negation->operand != nullptr, "NOT: non-null operand");
            const Binary* comparison = binary(negation->operand.get());
            test.expect(has_compare(comparison, CmpOp::kEq), "NOT: equality operand");
        }
        if (deletion != nullptr) {
            expect_input_mapping(test, "NOT", *deletion);
            test.expect(
                !deletion->predicate.has_value() ||
                    expression_slots_are_mapped(
                        *deletion->predicate,
                        deletion->input_columns),
                "NOT: predicate slots are mapped");
        }
    }

    if (test.failures() != 0) {
        std::cerr << test.failures() << " DELETE planner test assertion(s) failed\n";
        return 1;
    }
    return 0;
}
