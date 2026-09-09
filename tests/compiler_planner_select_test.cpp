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

const QueryPlan* generate_select(
    TestContext& test,
    std::string_view name,
    SemanticResult& result,
    Plan& plan) {
    auto* statement = std::get_if<BoundStatement>(&result.outcome);
    test.expect(statement != nullptr, std::string{name} + ": semantic success");
    if (statement == nullptr) {
        return nullptr;
    }
    auto* select = std::get_if<BoundSelect>(&statement->kind);
    test.expect(select != nullptr, std::string{name} + ": bound SELECT");
    if (select == nullptr) {
        return nullptr;
    }
    plan = generate_plan(std::move(*select));
    const auto* query = std::get_if<QueryPlan>(&plan.kind);
    test.expect(query != nullptr, std::string{name} + ": public QueryPlan");
    test.expect(query != nullptr && query->root != nullptr, std::string{name} + ": non-null root");
    return query;
}

const ProjectNode* project_root(const QueryPlan* query) {
    return query == nullptr || query->root == nullptr
        ? nullptr
        : std::get_if<ProjectNode>(&query->root->kind);
}

const FilterNode* project_filter(const ProjectNode* project) {
    return project == nullptr || project->child == nullptr
        ? nullptr
        : std::get_if<FilterNode>(&project->child->kind);
}

const SeqScanNode* scan_node(const PlanNode* node) {
    return node == nullptr ? nullptr : std::get_if<SeqScanNode>(&node->kind);
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

void expect_scan(
    TestContext& test,
    std::string_view name,
    const PlanNode* node) {
    const auto* scan = scan_node(node);
    test.expect(scan != nullptr, std::string{name} + ": SeqScan child");
    if (scan != nullptr) {
        test.expect(scan->table_id == 7U, std::string{name} + ": SeqScan TableId");
    }
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

Plan placeholder_plan() {
    return Plan{QueryPlan{std::make_unique<PlanNode>(SeqScanNode{0U})}};
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
            test, "without WHERE", "SELECT id,name FROM student;", catalog);
        Plan plan = placeholder_plan();
        const QueryPlan* query = generate_select(test, "without WHERE", result, plan);
        const ProjectNode* project = project_root(query);
        test.expect(project != nullptr, "without WHERE: Project root");
        if (project != nullptr) {
            test.expect(project->outputs == std::vector<ColumnId>{0U, 1U}, "without WHERE: outputs");
            test.expect(project->child != nullptr, "without WHERE: non-null Project child");
            expect_scan(test, "without WHERE", project->child.get());
        }
    }

    {
        SemanticResult result = analyze_sql(test, "star", "SELECT * FROM student;", catalog);
        Plan plan = placeholder_plan();
        const QueryPlan* query = generate_select(test, "star", result, plan);
        const ProjectNode* project = project_root(query);
        test.expect(project != nullptr, "star: Project root");
        if (project != nullptr) {
            test.expect(project->outputs == std::vector<ColumnId>{0U, 1U, 2U}, "star: expanded outputs");
            expect_scan(test, "star", project->child.get());
        }
    }

    {
        SemanticResult result = analyze_sql(
            test, "projection order", "SELECT name,id,name FROM student;", catalog);
        Plan plan = placeholder_plan();
        const ProjectNode* project = project_root(
            generate_select(test, "projection order", result, plan));
        test.expect(project != nullptr, "projection order: Project root");
        if (project != nullptr) {
            test.expect(
                project->outputs == std::vector<ColumnId>{1U, 0U, 1U},
                "projection order: reorder and duplicate retained");
        }
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "simple WHERE",
            "SELECT name FROM student WHERE age >= 18;",
            catalog);
        Plan plan = placeholder_plan();
        const ProjectNode* project = project_root(generate_select(test, "simple WHERE", result, plan));
        test.expect(project != nullptr, "simple WHERE: Project root");
        const FilterNode* filter = project_filter(project);
        test.expect(filter != nullptr, "simple WHERE: Filter child");
        if (project != nullptr) {
            test.expect(project->outputs == std::vector<ColumnId>{1U}, "simple WHERE: outputs");
        }
        if (filter != nullptr) {
            test.expect(filter->child != nullptr, "simple WHERE: non-null Filter child");
            const Binary* comparison = binary(&filter->predicate);
            test.expect(has_compare(comparison, CmpOp::kGe), "simple WHERE: >= predicate");
            if (comparison != nullptr) {
                test.expect(comparison->lhs != nullptr && comparison->rhs != nullptr, "simple WHERE: non-null operands");
                expect_column(test, "simple WHERE lhs", comparison->lhs.get(), 2U);
                expect_integer(test, "simple WHERE rhs", comparison->rhs.get(), 18);
            }
            expect_scan(test, "simple WHERE", filter->child.get());
        }
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "complex WHERE",
            "SELECT name,id FROM student WHERE age >= 18 AND name != 'Tom';",
            catalog);
        Plan plan = placeholder_plan();
        const ProjectNode* project = project_root(generate_select(test, "complex WHERE", result, plan));
        const FilterNode* filter = project_filter(project);
        test.expect(project != nullptr && project->outputs == std::vector<ColumnId>{1U, 0U}, "complex WHERE: outputs");
        test.expect(filter != nullptr, "complex WHERE: Filter child");
        const Binary* conjunction = filter == nullptr ? nullptr : binary(&filter->predicate);
        test.expect(has_logic(conjunction, LogicOp::kAnd), "complex WHERE: AND root");
        if (conjunction != nullptr) {
            test.expect(conjunction->lhs != nullptr && conjunction->rhs != nullptr, "complex WHERE: non-null operands");
            const Binary* lhs = binary(conjunction->lhs.get());
            const Binary* rhs = binary(conjunction->rhs.get());
            test.expect(has_compare(lhs, CmpOp::kGe), "complex WHERE: lhs >=");
            test.expect(has_compare(rhs, CmpOp::kNe), "complex WHERE: rhs !=");
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
            test, "NOT", "SELECT id FROM student WHERE NOT id = 1;", catalog);
        Plan plan = placeholder_plan();
        const FilterNode* filter = project_filter(
            project_root(generate_select(test, "NOT", result, plan)));
        const Unary* negation = filter == nullptr ? nullptr : unary(&filter->predicate);
        test.expect(negation != nullptr && negation->op == UnaryOp::kNot, "NOT: Unary root");
        if (negation != nullptr) {
            test.expect(negation->operand != nullptr, "NOT: non-null operand");
            const Binary* comparison = binary(negation->operand.get());
            test.expect(has_compare(comparison, CmpOp::kEq), "NOT: equality operand");
        }
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "parentheses",
            "SELECT id FROM student WHERE (id = 1 OR age = 18) AND name = 'Alice';",
            catalog);
        Plan plan = placeholder_plan();
        const FilterNode* filter = project_filter(
            project_root(generate_select(test, "parentheses", result, plan)));
        const Binary* conjunction = filter == nullptr ? nullptr : binary(&filter->predicate);
        test.expect(has_logic(conjunction, LogicOp::kAnd), "parentheses: AND root");
        if (conjunction != nullptr) {
            const Binary* disjunction = binary(conjunction->lhs.get());
            const Binary* comparison = binary(conjunction->rhs.get());
            test.expect(has_logic(disjunction, LogicOp::kOr), "parentheses: OR lhs");
            test.expect(has_compare(comparison, CmpOp::kEq), "parentheses: equality rhs");
            if (disjunction != nullptr) {
                test.expect(
                    has_compare(binary(disjunction->lhs.get()), CmpOp::kEq),
                    "parentheses: OR lhs equality");
                test.expect(
                    has_compare(binary(disjunction->rhs.get()), CmpOp::kEq),
                    "parentheses: OR rhs equality");
            }
        }
    }

    {
        SemanticResult result = analyze_sql(
            test, "literal comparison", "SELECT id FROM student WHERE 1 < 2;", catalog);
        Plan plan = placeholder_plan();
        const FilterNode* filter = project_filter(
            project_root(generate_select(test, "literal comparison", result, plan)));
        const Binary* comparison = filter == nullptr ? nullptr : binary(&filter->predicate);
        test.expect(has_compare(comparison, CmpOp::kLt), "literal comparison: < root");
        if (comparison != nullptr) {
            expect_integer(test, "literal comparison lhs", comparison->lhs.get(), 1);
            expect_integer(test, "literal comparison rhs", comparison->rhs.get(), 2);
        }
    }

    if (test.failures() != 0) {
        std::cerr << test.failures() << " SELECT planner test assertion(s) failed\n";
        return 1;
    }
    return 0;
}
