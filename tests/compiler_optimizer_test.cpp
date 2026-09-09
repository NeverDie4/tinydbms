#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "lexer.hpp"
#include "optimizer.hpp"
#include "parser.hpp"
#include "plan_formatter.hpp"
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

BoundStatement analyze_sql(std::string_view sql, CatalogView catalog) {
    LexResult lexed = tokenize(sql);
    auto tokens = std::get<std::vector<Token>>(std::move(lexed.outcome));
    ParseResult parsed = parse(tokens);
    const auto& statement = std::get<StatementAst>(parsed.outcome);
    SemanticResult analyzed = analyze(statement, catalog);
    return std::get<BoundStatement>(std::move(analyzed.outcome));
}

BoundStatement optimize_sql(std::string_view sql, CatalogView catalog) {
    return optimize(analyze_sql(sql, catalog));
}

OptimizationResult optimize_with_trace_sql(std::string_view sql, CatalogView catalog) {
    return optimize_with_trace(analyze_sql(sql, catalog));
}

BoundStatement optimize_twice_sql(std::string_view sql, CatalogView catalog) {
    return optimize(optimize_sql(sql, catalog));
}

std::string format_bound_plan(BoundStatement statement) {
    const Plan plan = std::visit(
        [](auto&& value) { return generate_plan(std::move(value)); },
        std::move(statement.kind));
    return format_plan(plan);
}

void expect_trace(
    TestContext& test,
    const OptimizationTrace& trace,
    const std::vector<OptimizationRule>& expected,
    std::string_view message) {
    test.expect(trace.size() == expected.size(), message);
    const std::size_t count = trace.size() < expected.size() ? trace.size() : expected.size();
    for (std::size_t index = 0; index < count; ++index) {
        test.expect(trace[index].rule == expected[index], message);
    }
}

const BoundSelect* select_of(TestContext& test, const BoundStatement& statement) {
    const auto* select = std::get_if<BoundSelect>(&statement.kind);
    test.expect(select != nullptr, "expected BoundSelect");
    return select;
}

const BoundDelete* delete_of(TestContext& test, const BoundStatement& statement) {
    const auto* deletion = std::get_if<BoundDelete>(&statement.kind);
    test.expect(deletion != nullptr, "expected BoundDelete");
    return deletion;
}

const BoundBinaryExpr* binary_of(const BoundExpr* expression) {
    return expression == nullptr ? nullptr : std::get_if<BoundBinaryExpr>(&expression->kind);
}

const BoundUnaryExpr* unary_of(const BoundExpr* expression) {
    return expression == nullptr ? nullptr : std::get_if<BoundUnaryExpr>(&expression->kind);
}

bool has_compare(const BoundBinaryExpr* expression, CmpOp expected) {
    const auto* op = expression == nullptr ? nullptr : std::get_if<CmpOp>(&expression->op);
    return op != nullptr && *op == expected;
}

bool has_logic(const BoundBinaryExpr* expression, LogicOp expected) {
    const auto* op = expression == nullptr ? nullptr : std::get_if<LogicOp>(&expression->op);
    return op != nullptr && *op == expected;
}

bool has_valid_children(const BoundExpr* expression) {
    if (expression == nullptr) {
        return false;
    }
    if (const auto* binary = std::get_if<BoundBinaryExpr>(&expression->kind)) {
        return binary->lhs != nullptr && binary->rhs != nullptr &&
               has_valid_children(binary->lhs.get()) && has_valid_children(binary->rhs.get());
    }
    if (const auto* unary = std::get_if<BoundUnaryExpr>(&expression->kind)) {
        return unary->operand != nullptr && has_valid_children(unary->operand.get());
    }
    return true;
}

const BoundExpr* skip_unary_not(const BoundExpr* expression, std::size_t& count) {
    while (const auto* unary = unary_of(expression)) {
        ++count;
        expression = unary->operand.get();
    }
    return expression;
}

void expect_column_int_comparison(
    TestContext& test,
    const BoundExpr* expression,
    CmpOp expected_op,
    ColumnId expected_column,
    std::int32_t expected_value) {
    const auto* comparison = binary_of(expression);
    test.expect(has_compare(comparison, expected_op), "comparison operator");
    if (comparison == nullptr) {
        return;
    }
    test.expect(comparison->lhs != nullptr, "comparison lhs");
    test.expect(comparison->rhs != nullptr, "comparison rhs");
    const auto* column = comparison->lhs == nullptr
        ? nullptr
        : std::get_if<BoundColumnRef>(&comparison->lhs->kind);
    const auto* literal = comparison->rhs == nullptr
        ? nullptr
        : std::get_if<BoundLiteral>(&comparison->rhs->kind);
    const auto* value = literal == nullptr
        ? nullptr
        : std::get_if<std::int32_t>(&literal->value.data);
    test.expect(column != nullptr && column->column_id == expected_column, "comparison ColumnId");
    test.expect(value != nullptr && *value == expected_value, "comparison literal");
}

void expect_column_string_comparison(
    TestContext& test,
    const BoundExpr* expression,
    CmpOp expected_op,
    ColumnId expected_column,
    std::string_view expected_value) {
    const auto* comparison = binary_of(expression);
    test.expect(has_compare(comparison, expected_op), "string comparison operator");
    if (comparison == nullptr) {
        return;
    }
    const auto* column = comparison->lhs == nullptr
        ? nullptr
        : std::get_if<BoundColumnRef>(&comparison->lhs->kind);
    const auto* literal = comparison->rhs == nullptr
        ? nullptr
        : std::get_if<BoundLiteral>(&comparison->rhs->kind);
    const auto* value = literal == nullptr
        ? nullptr
        : std::get_if<std::string>(&literal->value.data);
    test.expect(column != nullptr && column->column_id == expected_column, "string comparison ColumnId");
    test.expect(value != nullptr && *value == expected_value, "string comparison literal");
}

void test_constant_comparisons(TestContext& test, CatalogView catalog) {
    const std::vector<std::string_view> true_predicates{
        "1 = 1",
        "1 != 2",
        "1 < 2",
        "1 <= 1",
        "2 > 1",
        "2 >= 2",
        "'A' = 'A'",
        "'A' != 'B'",
    };
    for (const std::string_view predicate : true_predicates) {
        const std::string sql = "SELECT id FROM student WHERE " + std::string{predicate} + ";";
        const BoundStatement statement = optimize_sql(sql, catalog);
        const BoundSelect* select = select_of(test, statement);
        test.expect(select != nullptr && select->predicate == nullptr, "constant TRUE removed");
    }

    const BoundStatement not_equal = optimize_sql(
        "SELECT id FROM student WHERE 1 != 1;",
        catalog);
    const BoundSelect* select = select_of(test, not_equal);
    test.expect(select != nullptr && select->predicate != nullptr, "constant FALSE retained");
    test.expect(
        select != nullptr && has_compare(binary_of(select->predicate.get()), CmpOp::kNe),
        "constant FALSE keeps original comparison");
}

void test_and_rules(TestContext& test, CatalogView catalog) {
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE 1 = 1 AND age > 18;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        expect_column_int_comparison(test, select == nullptr ? nullptr : select->predicate.get(), CmpOp::kGt, 2U, 18);
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE age > 18 AND 1 = 1;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        expect_column_int_comparison(test, select == nullptr ? nullptr : select->predicate.get(), CmpOp::kGt, 2U, 18);
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE 1 = 2 AND age > 18;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        test.expect(select != nullptr && select->predicate != nullptr, "FALSE AND X retained");
        test.expect(
            select != nullptr && has_compare(binary_of(select->predicate.get()), CmpOp::kEq),
            "FALSE AND X becomes false comparison");
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE age > 18 AND 1 = 2;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        test.expect(select != nullptr && select->predicate != nullptr, "X AND FALSE retained");
        test.expect(
            select != nullptr && has_compare(binary_of(select->predicate.get()), CmpOp::kEq),
            "X AND FALSE becomes false comparison");
    }
}

void test_or_rules(TestContext& test, CatalogView catalog) {
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE 1 = 2 OR age > 18;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        expect_column_int_comparison(test, select == nullptr ? nullptr : select->predicate.get(), CmpOp::kGt, 2U, 18);
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE age > 18 OR 1 = 2;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        expect_column_int_comparison(test, select == nullptr ? nullptr : select->predicate.get(), CmpOp::kGt, 2U, 18);
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE 1 = 1 OR age > 18;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        test.expect(select != nullptr && select->predicate == nullptr, "TRUE OR X removed");
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE age > 18 OR 1 = 1;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        test.expect(select != nullptr && select->predicate == nullptr, "X OR TRUE removed");
    }
}

void test_statement_roots(TestContext& test, CatalogView catalog) {
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE 1 = 1;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        test.expect(select != nullptr && select->predicate == nullptr, "SELECT TRUE removed");
    }
    {
        const BoundStatement statement = optimize_sql(
            "DELETE FROM student WHERE 1 = 1;",
            catalog);
        const BoundDelete* deletion = delete_of(test, statement);
        test.expect(deletion != nullptr && deletion->predicate == nullptr, "DELETE TRUE removed");
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE 1 = 2;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        test.expect(select != nullptr && select->predicate != nullptr, "SELECT FALSE retained");
    }
    {
        const BoundStatement statement = optimize_sql(
            "DELETE FROM student WHERE 1 = 2;",
            catalog);
        const BoundDelete* deletion = delete_of(test, statement);
        test.expect(deletion != nullptr && deletion->predicate != nullptr, "DELETE FALSE retained");
    }
}

void test_recursive_rules(TestContext& test, CatalogView catalog) {
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE (1 = 1 AND age > 18) AND name != 'Tom';",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        const auto* conjunction = select == nullptr ? nullptr : binary_of(select->predicate.get());
        test.expect(has_logic(conjunction, LogicOp::kAnd), "nested O13 keeps AND root");
        if (conjunction != nullptr) {
            expect_column_int_comparison(test, conjunction->lhs.get(), CmpOp::kGt, 2U, 18);
            expect_column_string_comparison(test, conjunction->rhs.get(), CmpOp::kNe, 1U, "Tom");
        }
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE (1 = 2 OR age > 18) AND (name = 'Tom' OR 1 = 2);",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        const auto* conjunction = select == nullptr ? nullptr : binary_of(select->predicate.get());
        test.expect(has_logic(conjunction, LogicOp::kAnd), "nested O14 keeps AND root");
        if (conjunction != nullptr) {
            expect_column_int_comparison(test, conjunction->lhs.get(), CmpOp::kGt, 2U, 18);
            expect_column_string_comparison(test, conjunction->rhs.get(), CmpOp::kEq, 1U, "Tom");
        }
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE NOT (1 = 1) OR age > 18;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        expect_column_int_comparison(test, select == nullptr ? nullptr : select->predicate.get(), CmpOp::kGt, 2U, 18);
    }
}

void test_double_not_rules(TestContext& test, CatalogView catalog) {
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE NOT NOT id = 1;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        expect_column_int_comparison(
            test, select == nullptr ? nullptr : select->predicate.get(), CmpOp::kEq, 0U, 1);
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE NOT NOT age > 18;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        expect_column_int_comparison(
            test, select == nullptr ? nullptr : select->predicate.get(), CmpOp::kGt, 2U, 18);
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE NOT NOT (1 = 1);",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        test.expect(select != nullptr && select->predicate == nullptr, "D3 double NOT TRUE removed");
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE NOT NOT (1 = 2);",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        test.expect(select != nullptr && select->predicate != nullptr, "D4 double NOT FALSE retained");
        test.expect(
            select != nullptr && has_compare(binary_of(select->predicate.get()), CmpOp::kEq),
            "D4 false comparison retained");
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE NOT NOT NOT id = 1;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        std::size_t count = 0;
        const BoundExpr* leaf = select == nullptr
            ? nullptr
            : skip_unary_not(select->predicate.get(), count);
        test.expect(count == 1, "D5 triple NOT becomes one NOT");
        expect_column_int_comparison(test, leaf, CmpOp::kEq, 0U, 1);
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE NOT NOT NOT NOT id = 1;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        expect_column_int_comparison(
            test, select == nullptr ? nullptr : select->predicate.get(), CmpOp::kEq, 0U, 1);
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE NOT NOT (1 = 1) AND age > 18;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        expect_column_int_comparison(
            test, select == nullptr ? nullptr : select->predicate.get(), CmpOp::kGt, 2U, 18);
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE NOT NOT (1 = 2) OR age > 18;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        expect_column_int_comparison(
            test, select == nullptr ? nullptr : select->predicate.get(), CmpOp::kGt, 2U, 18);
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE NOT NOT (age > 18 AND id = 1);",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        const auto* conjunction = select == nullptr ? nullptr : binary_of(select->predicate.get());
        test.expect(has_logic(conjunction, LogicOp::kAnd), "D9 double NOT exposes AND");
        if (conjunction != nullptr) {
            expect_column_int_comparison(test, conjunction->lhs.get(), CmpOp::kGt, 2U, 18);
            expect_column_int_comparison(test, conjunction->rhs.get(), CmpOp::kEq, 0U, 1);
            test.expect(has_valid_children(select->predicate.get()), "D9 Bound tree children non-null");
        }
    }
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE NOT (age > 18 AND id = 1);",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        const auto* negation = select == nullptr ? nullptr : unary_of(select->predicate.get());
        test.expect(negation != nullptr, "D10 single NOT remains");
        test.expect(
            negation != nullptr && has_logic(binary_of(negation->operand.get()), LogicOp::kAnd),
            "D10 single NOT keeps AND operand");
    }
}

void test_idempotence(TestContext& test, CatalogView catalog) {
    {
        const BoundStatement statement = optimize_twice_sql(
            "SELECT id FROM student WHERE 1 = 1;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        test.expect(select != nullptr && select->predicate == nullptr, "D11 SELECT TRUE idempotent");
    }
    {
        const BoundStatement statement = optimize_twice_sql(
            "SELECT id FROM student WHERE NOT NOT (1 = 1) AND age > 18;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        expect_column_int_comparison(
            test, select == nullptr ? nullptr : select->predicate.get(), CmpOp::kGt, 2U, 18);
    }
    {
        const BoundStatement statement = optimize_twice_sql(
            "SELECT id FROM student WHERE 1 = 2;",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        test.expect(select != nullptr && select->predicate != nullptr, "D12 SELECT FALSE idempotent");
        test.expect(
            select != nullptr && has_valid_children(select->predicate.get()),
            "D12 SELECT FALSE tree valid");
    }
    {
        const BoundStatement statement = optimize_twice_sql(
            "DELETE FROM student WHERE 1 = 1;",
            catalog);
        const BoundDelete* deletion = delete_of(test, statement);
        test.expect(deletion != nullptr && deletion->predicate == nullptr, "D13 DELETE TRUE idempotent");
    }
    {
        const BoundStatement statement = optimize_twice_sql(
            "DELETE FROM student WHERE 1 = 2;",
            catalog);
        const BoundDelete* deletion = delete_of(test, statement);
        test.expect(deletion != nullptr && deletion->predicate != nullptr, "D14 DELETE FALSE idempotent");
        test.expect(
            deletion != nullptr && has_valid_children(deletion->predicate.get()),
            "D14 DELETE FALSE tree valid");
    }
    {
        const BoundStatement statement = optimize_twice_sql(
            "CREATE TABLE course(code INT,title VARCHAR);",
            catalog);
        const auto* create = std::get_if<BoundCreateTable>(&statement.kind);
        test.expect(create != nullptr && create->table_name == "course", "D15 CREATE idempotent table");
        test.expect(
            create != nullptr && create->columns.size() == 2 &&
                create->columns[0].name == "code" && create->columns[1].type == Type::kVarchar,
            "D15 CREATE idempotent columns");
    }
    {
        const BoundStatement statement = optimize_twice_sql(
            "INSERT INTO student(age,id,name) VALUES (20,1,'Alice'),(21,2,'Bob');",
            catalog);
        const auto* insert = std::get_if<BoundInsert>(&statement.kind);
        test.expect(insert != nullptr && insert->table_id == 7U, "D16 INSERT idempotent TableId");
        test.expect(
            insert != nullptr && insert->columns == std::vector<ColumnId>{2U, 0U, 1U},
            "D16 INSERT idempotent columns");
        test.expect(
            insert != nullptr && insert->rows.size() == 2 &&
                std::get<std::int32_t>(insert->rows[0][0].data) == 20 &&
                std::get<std::string>(insert->rows[1][2].data) == "Bob",
            "D16 INSERT idempotent rows");
    }
}

void test_planner_compatibility(TestContext& test, CatalogView catalog) {
    {
        BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE NOT NOT age > 18;",
            catalog);
        auto select = std::get<BoundSelect>(std::move(statement.kind));
        const Plan plan = generate_plan(std::move(select));
        const auto* query = std::get_if<QueryPlan>(&plan.kind);
        const auto* project = query == nullptr || query->root == nullptr
            ? nullptr
            : std::get_if<ProjectNode>(&query->root->kind);
        const auto* filter = project == nullptr || project->child == nullptr
            ? nullptr
            : std::get_if<FilterNode>(&project->child->kind);
        const auto* comparison = filter == nullptr
            ? nullptr
            : std::get_if<Binary>(&filter->predicate.kind);
        test.expect(project != nullptr && project->outputs == std::vector<ColumnId>{0U}, "D17 Project output");
        test.expect(filter != nullptr && comparison != nullptr, "D17 optimized SELECT has Filter comparison");
        test.expect(
            comparison != nullptr && std::get_if<CmpOp>(&comparison->op) != nullptr &&
                *std::get_if<CmpOp>(&comparison->op) == CmpOp::kGt,
            "D17 optimized SELECT comparison");
        test.expect(
            filter != nullptr && filter->child != nullptr &&
                std::holds_alternative<SeqScanNode>(filter->child->kind),
            "D17 optimized SELECT reaches SeqScan");

        BoundStatement true_statement = optimize_sql(
            "SELECT id FROM student WHERE NOT NOT (1 = 1);",
            catalog);
        auto true_select = std::get<BoundSelect>(std::move(true_statement.kind));
        const Plan true_plan = generate_plan(std::move(true_select));
        const auto* true_query = std::get_if<QueryPlan>(&true_plan.kind);
        const auto* true_project = true_query == nullptr || true_query->root == nullptr
            ? nullptr
            : std::get_if<ProjectNode>(&true_query->root->kind);
        test.expect(
            true_project != nullptr && true_project->child != nullptr &&
                std::holds_alternative<SeqScanNode>(true_project->child->kind),
            "D17 TRUE SELECT omits Filter");
    }
    {
        BoundStatement statement = optimize_sql(
            "DELETE FROM student WHERE NOT NOT id = 1;",
            catalog);
        auto deletion = std::get<BoundDelete>(std::move(statement.kind));
        const Plan plan = generate_plan(std::move(deletion));
        const auto* delete_plan = std::get_if<DeletePlan>(&plan.kind);
        const auto* comparison = delete_plan == nullptr || !delete_plan->predicate.has_value()
            ? nullptr
            : std::get_if<Binary>(&delete_plan->predicate->kind);
        test.expect(delete_plan != nullptr && delete_plan->table_id == 7U, "D18 DELETE TableId");
        test.expect(
            comparison != nullptr && std::get_if<CmpOp>(&comparison->op) != nullptr &&
                *std::get_if<CmpOp>(&comparison->op) == CmpOp::kEq,
            "D18 optimized DELETE comparison");

        BoundStatement true_statement = optimize_sql(
            "DELETE FROM student WHERE NOT NOT (1 = 1);",
            catalog);
        auto true_deletion = std::get<BoundDelete>(std::move(true_statement.kind));
        const Plan true_plan = generate_plan(std::move(true_deletion));
        const auto* true_delete = std::get_if<DeletePlan>(&true_plan.kind);
        test.expect(
            true_delete != nullptr && !true_delete->predicate.has_value(),
            "D18 TRUE DELETE has nullopt predicate");
    }
}

void test_nested_and_deep_not(TestContext& test, CatalogView catalog) {
    {
        const BoundStatement statement = optimize_sql(
            "SELECT id FROM student WHERE "
            "NOT NOT (1 = 1 AND age > 18) AND NOT NOT (name = 'Tom');",
            catalog);
        const BoundSelect* select = select_of(test, statement);
        const auto* conjunction = select == nullptr ? nullptr : binary_of(select->predicate.get());
        test.expect(has_logic(conjunction, LogicOp::kAnd), "D19 nested combination keeps AND");
        if (conjunction != nullptr) {
            expect_column_int_comparison(test, conjunction->lhs.get(), CmpOp::kGt, 2U, 18);
            expect_column_string_comparison(test, conjunction->rhs.get(), CmpOp::kEq, 1U, "Tom");
            test.expect(has_valid_children(select->predicate.get()), "D19 nested tree children non-null");
        }
    }
    {
        std::string even_sql = "SELECT id FROM student WHERE ";
        for (int index = 0; index < 20; ++index) {
            even_sql += "NOT ";
        }
        even_sql += "id = 1;";
        const BoundStatement even_statement = optimize_sql(even_sql, catalog);
        const BoundSelect* even_select = select_of(test, even_statement);
        std::size_t even_count = 0;
        const BoundExpr* even_leaf = even_select == nullptr
            ? nullptr
            : skip_unary_not(even_select->predicate.get(), even_count);
        test.expect(even_count == 0, "D20 twenty NOTs become zero");
        expect_column_int_comparison(test, even_leaf, CmpOp::kEq, 0U, 1);

        std::string odd_sql = "SELECT id FROM student WHERE ";
        for (int index = 0; index < 19; ++index) {
            odd_sql += "NOT ";
        }
        odd_sql += "id = 1;";
        const BoundStatement odd_statement = optimize_sql(odd_sql, catalog);
        const BoundSelect* odd_select = select_of(test, odd_statement);
        std::size_t odd_count = 0;
        const BoundExpr* odd_leaf = odd_select == nullptr
            ? nullptr
            : skip_unary_not(odd_select->predicate.get(), odd_count);
        test.expect(odd_count == 1, "D20 nineteen NOTs become one");
        expect_column_int_comparison(test, odd_leaf, CmpOp::kEq, 0U, 1);
        test.expect(
            odd_select != nullptr && has_valid_children(odd_select->predicate.get()),
            "D20 deep NOT tree children non-null");
    }
}

void test_passthrough(TestContext& test, CatalogView catalog) {
    {
        const BoundStatement statement = optimize_sql(
            "CREATE TABLE course(code INT,title VARCHAR);",
            catalog);
        const auto* create = std::get_if<BoundCreateTable>(&statement.kind);
        test.expect(create != nullptr, "CREATE passthrough kind");
        if (create != nullptr) {
            test.expect(create->table_name == "course", "CREATE passthrough table");
            test.expect(create->columns.size() == 2, "CREATE passthrough columns");
            test.expect(create->columns[0].name == "code", "CREATE passthrough first column");
            test.expect(create->columns[1].type == Type::kVarchar, "CREATE passthrough second type");
        }
    }
    {
        const BoundStatement statement = optimize_sql(
            "INSERT INTO student(age,id,name) VALUES (20,1,'Alice'),(21,2,'Bob');",
            catalog);
        const auto* insert = std::get_if<BoundInsert>(&statement.kind);
        test.expect(insert != nullptr, "INSERT passthrough kind");
        if (insert != nullptr) {
            test.expect(insert->table_id == 7U, "INSERT passthrough TableId");
            test.expect(insert->columns == std::vector<ColumnId>{2U, 0U, 1U}, "INSERT passthrough columns");
            test.expect(insert->rows.size() == 2, "INSERT passthrough rows");
            test.expect(std::get<std::int32_t>(insert->rows[0][0].data) == 20, "INSERT first value");
            test.expect(std::get<std::string>(insert->rows[1][2].data) == "Bob", "INSERT final value");
        }
    }
}

void test_rule_identity_and_trace(TestContext& test, CatalogView catalog) {
    test.expect(
        optimization_rule_name(OptimizationRule::kConstantComparison) ==
            "ConstantComparison",
        "ConstantComparison stable name");
    test.expect(
        optimization_rule_name(OptimizationRule::kBooleanSimplification) ==
            "BooleanSimplification",
        "BooleanSimplification stable name");
    test.expect(
        optimization_rule_name(OptimizationRule::kDoubleNotElimination) ==
            "DoubleNotElimination",
        "DoubleNotElimination stable name");
    test.expect(
        optimization_rule_name(OptimizationRule::kRedundantTruePredicateElimination) ==
            "RedundantTruePredicateElimination",
        "RedundantTruePredicateElimination stable name");

    {
        OptimizationResult result = optimize_with_trace_sql(
            "SELECT id FROM student WHERE 1 = 2;",
            catalog);
        expect_trace(
            test,
            result.trace,
            {OptimizationRule::kConstantComparison},
            "FALSE comparison trace");
        const BoundSelect* select = select_of(test, result.statement);
        test.expect(select != nullptr && select->predicate != nullptr, "FALSE trace predicate retained");
    }
    {
        OptimizationResult result = optimize_with_trace_sql(
            "SELECT id FROM student WHERE 1 = 1;",
            catalog);
        expect_trace(
            test,
            result.trace,
            {
                OptimizationRule::kConstantComparison,
                OptimizationRule::kRedundantTruePredicateElimination,
            },
            "TRUE root trace");
    }
    {
        OptimizationResult result = optimize_with_trace_sql(
            "SELECT id FROM student WHERE 1 = 1 AND age > 18;",
            catalog);
        expect_trace(
            test,
            result.trace,
            {
                OptimizationRule::kConstantComparison,
                OptimizationRule::kBooleanSimplification,
            },
            "AND trace");
    }
    {
        OptimizationResult result = optimize_with_trace_sql(
            "SELECT id FROM student WHERE NOT NOT age > 18;",
            catalog);
        expect_trace(
            test,
            result.trace,
            {OptimizationRule::kDoubleNotElimination},
            "double NOT trace");
    }
    {
        OptimizationResult result = optimize_with_trace_sql(
            "SELECT name FROM student WHERE 1 = 1 AND NOT NOT age > 18;",
            catalog);
        expect_trace(
            test,
            result.trace,
            {
                OptimizationRule::kConstantComparison,
                OptimizationRule::kDoubleNotElimination,
                OptimizationRule::kBooleanSimplification,
            },
            "combined bottom-up trace");
        const BoundSelect* select = select_of(test, result.statement);
        expect_column_int_comparison(
            test,
            select == nullptr ? nullptr : select->predicate.get(),
            CmpOp::kGt,
            2U,
            18);
    }
    {
        OptimizationResult result = optimize_with_trace_sql(
            "SELECT name FROM student WHERE "
            "(1 = 2 OR NOT NOT age > 18) AND "
            "(1 = 1 AND name = 'Tom');",
            catalog);
        expect_trace(
            test,
            result.trace,
            {
                OptimizationRule::kConstantComparison,
                OptimizationRule::kDoubleNotElimination,
                OptimizationRule::kBooleanSimplification,
                OptimizationRule::kConstantComparison,
                OptimizationRule::kBooleanSimplification,
            },
            "complex deterministic pipeline trace");
        const BoundSelect* select = select_of(test, result.statement);
        const auto* conjunction = select == nullptr
            ? nullptr
            : binary_of(select->predicate.get());
        test.expect(
            has_logic(conjunction, LogicOp::kAnd),
            "complex deterministic pipeline AND root");
        if (conjunction != nullptr) {
            expect_column_int_comparison(
                test, conjunction->lhs.get(), CmpOp::kGt, 2U, 18);
            expect_column_string_comparison(
                test, conjunction->rhs.get(), CmpOp::kEq, 1U, "Tom");
        }
    }
    {
        OptimizationResult result = optimize_with_trace_sql(
            "SELECT id FROM student WHERE NOT NOT NOT NOT id = 1;",
            catalog);
        expect_trace(
            test,
            result.trace,
            {
                OptimizationRule::kDoubleNotElimination,
                OptimizationRule::kDoubleNotElimination,
            },
            "four NOT trace");
    }
    {
        OptimizationResult result = optimize_with_trace_sql(
            "SELECT id FROM student WHERE 1 = 1 OR age > 18;",
            catalog);
        expect_trace(
            test,
            result.trace,
            {
                OptimizationRule::kConstantComparison,
                OptimizationRule::kBooleanSimplification,
                OptimizationRule::kRedundantTruePredicateElimination,
            },
            "TRUE OR trace");
    }
    {
        OptimizationResult result = optimize_with_trace_sql(
            "SELECT id FROM student WHERE 1 = 2 OR age > 18;",
            catalog);
        expect_trace(
            test,
            result.trace,
            {
                OptimizationRule::kConstantComparison,
                OptimizationRule::kBooleanSimplification,
            },
            "FALSE OR trace");
    }
    {
        OptimizationResult true_result = optimize_with_trace_sql(
            "DELETE FROM student WHERE 1 = 1;",
            catalog);
        expect_trace(
            test,
            true_result.trace,
            {
                OptimizationRule::kConstantComparison,
                OptimizationRule::kRedundantTruePredicateElimination,
            },
            "DELETE TRUE trace");

        OptimizationResult false_result = optimize_with_trace_sql(
            "DELETE FROM student WHERE 1 = 2;",
            catalog);
        expect_trace(
            test,
            false_result.trace,
            {OptimizationRule::kConstantComparison},
            "DELETE FALSE trace");
    }
    {
        OptimizationResult create = optimize_with_trace_sql(
            "CREATE TABLE traced(code INT);",
            catalog);
        OptimizationResult insert = optimize_with_trace_sql(
            "INSERT INTO student VALUES (1,'Alice',20);",
            catalog);
        OptimizationResult ordinary = optimize_with_trace_sql(
            "SELECT id FROM student WHERE age > 18;",
            catalog);
        test.expect(create.trace.empty(), "CREATE trace empty");
        test.expect(insert.trace.empty(), "INSERT trace empty");
        test.expect(ordinary.trace.empty(), "ordinary SELECT trace empty");
    }
}

void test_trace_idempotence_and_behavior(TestContext& test, CatalogView catalog) {
    {
        OptimizationResult first = optimize_with_trace_sql(
            "SELECT id FROM student WHERE 1 = 1 AND age > 18;",
            catalog);
        expect_trace(
            test,
            first.trace,
            {
                OptimizationRule::kConstantComparison,
                OptimizationRule::kBooleanSimplification,
            },
            "first structural trace");
        OptimizationResult second = optimize_with_trace(std::move(first.statement));
        test.expect(second.trace.empty(), "second structural trace empty");
        const BoundSelect* select = select_of(test, second.statement);
        expect_column_int_comparison(
            test,
            select == nullptr ? nullptr : select->predicate.get(),
            CmpOp::kGt,
            2U,
            18);
    }
    {
        OptimizationResult first = optimize_with_trace_sql(
            "SELECT id FROM student WHERE 1 = 2;",
            catalog);
        OptimizationResult second = optimize_with_trace(std::move(first.statement));
        expect_trace(
            test,
            second.trace,
            {OptimizationRule::kConstantComparison},
            "retained comparison is analyzed again");
        const BoundSelect* select = select_of(test, second.statement);
        test.expect(select != nullptr && select->predicate != nullptr, "repeated FALSE remains");
    }
    {
        OptimizationResult first = optimize_with_trace_sql(
            "SELECT id FROM student WHERE 1 = 1;",
            catalog);
        OptimizationResult second = optimize_with_trace(std::move(first.statement));
        test.expect(second.trace.empty(), "removed TRUE has no second trace");
    }

    const std::string_view sql =
        "SELECT name FROM student WHERE 1 = 1 AND NOT NOT age > 18;";
    BoundStatement regular = optimize_sql(sql, catalog);
    OptimizationResult traced = optimize_with_trace_sql(sql, catalog);
    test.expect(
        format_bound_plan(std::move(regular)) ==
            format_bound_plan(std::move(traced.statement)),
        "optimize and optimize_with_trace produce identical plan");
}

void test_rule_registry(TestContext& test, CatalogView catalog) {
    const std::vector<OptimizationRule> expected_rules{
        OptimizationRule::kConstantComparison,
        OptimizationRule::kBooleanSimplification,
        OptimizationRule::kDoubleNotElimination,
        OptimizationRule::kRedundantTruePredicateElimination,
    };
    const std::vector<std::string_view> expected_names{
        "ConstantComparison",
        "BooleanSimplification",
        "DoubleNotElimination",
        "RedundantTruePredicateElimination",
    };
    const std::vector<OptimizationRuleScope> expected_scopes{
        OptimizationRuleScope::kComparisonExpression,
        OptimizationRuleScope::kLogicalExpression,
        OptimizationRuleScope::kUnaryExpression,
        OptimizationRuleScope::kStatementPredicate,
    };
    const std::vector<std::string_view> expected_scope_names{
        "ComparisonExpression",
        "LogicalExpression",
        "UnaryExpression",
        "StatementPredicate",
    };

    const auto registry = optimization_rule_registry();
    test.expect(registry.size() == expected_rules.size(), "registry size");
    const std::size_t count = registry.size() < expected_rules.size()
        ? registry.size()
        : expected_rules.size();
    for (std::size_t index = 0; index < count; ++index) {
        const OptimizationRuleDescriptor& descriptor = registry[index];
        test.expect(descriptor.rule == expected_rules[index], "registry rule order");
        test.expect(descriptor.name == expected_names[index], "registry stable name");
        test.expect(descriptor.scope == expected_scopes[index], "registry scope order");
        test.expect(
            optimization_rule_scope_name(descriptor.scope) == expected_scope_names[index],
            "registry stable scope name");

        const OptimizationRuleDescriptor& lookup =
            optimization_rule_descriptor(descriptor.rule);
        test.expect(lookup.rule == descriptor.rule, "descriptor lookup rule");
        test.expect(lookup.name == descriptor.name, "descriptor lookup name");
        test.expect(lookup.scope == descriptor.scope, "descriptor lookup scope");
        test.expect(
            optimization_rule_name(descriptor.rule) == descriptor.name,
            "rule name uses descriptor metadata");
    }

    for (std::size_t lhs = 0; lhs < registry.size(); ++lhs) {
        test.expect(!registry[lhs].name.empty(), "registry name non-empty");
        for (std::size_t rhs = lhs + 1; rhs < registry.size(); ++rhs) {
            test.expect(registry[lhs].rule != registry[rhs].rule, "registry rule unique");
            test.expect(registry[lhs].name != registry[rhs].name, "registry name unique");
        }
    }

    const auto repeated = optimization_rule_registry();
    test.expect(repeated.size() == registry.size(), "registry repeated size stable");
    const std::size_t repeated_count = repeated.size() < registry.size()
        ? repeated.size()
        : registry.size();
    for (std::size_t index = 0; index < repeated_count; ++index) {
        test.expect(repeated[index].rule == registry[index].rule, "registry repeated rule stable");
        test.expect(repeated[index].name == registry[index].name, "registry repeated name stable");
        test.expect(repeated[index].scope == registry[index].scope, "registry repeated scope stable");
    }

    OptimizationResult traced = optimize_with_trace_sql(
        "SELECT name FROM student WHERE 1 = 1 AND NOT NOT age > 18;",
        catalog);
    const std::vector<OptimizationRule> trace_rules{
        OptimizationRule::kConstantComparison,
        OptimizationRule::kDoubleNotElimination,
        OptimizationRule::kBooleanSimplification,
    };
    const std::vector<std::string_view> trace_names{
        "ConstantComparison",
        "DoubleNotElimination",
        "BooleanSimplification",
    };
    const std::vector<OptimizationRuleScope> trace_scopes{
        OptimizationRuleScope::kComparisonExpression,
        OptimizationRuleScope::kUnaryExpression,
        OptimizationRuleScope::kLogicalExpression,
    };
    expect_trace(test, traced.trace, trace_rules, "registry-resolved trace order");
    test.expect(traced.trace.size() == trace_rules.size(), "registry-resolved trace size");
    const std::size_t trace_count = traced.trace.size() < trace_rules.size()
        ? traced.trace.size()
        : trace_rules.size();
    for (std::size_t index = 0; index < trace_count; ++index) {
        const OptimizationRuleDescriptor& descriptor =
            optimization_rule_descriptor(traced.trace[index].rule);
        test.expect(descriptor.name == trace_names[index], "trace descriptor name");
        test.expect(descriptor.scope == trace_scopes[index], "trace descriptor scope");
    }
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

    test_constant_comparisons(test, catalog);
    test_and_rules(test, catalog);
    test_or_rules(test, catalog);
    test_statement_roots(test, catalog);
    test_recursive_rules(test, catalog);
    test_double_not_rules(test, catalog);
    test_idempotence(test, catalog);
    test_planner_compatibility(test, catalog);
    test_nested_and_deep_not(test, catalog);
    test_passthrough(test, catalog);
    test_rule_identity_and_trace(test, catalog);
    test_trace_idempotence_and_behavior(test, catalog);
    test_rule_registry(test, catalog);

    if (test.failures() != 0) {
        std::cerr << test.failures() << " optimizer test assertion(s) failed\n";
        return 1;
    }
    return 0;
}
