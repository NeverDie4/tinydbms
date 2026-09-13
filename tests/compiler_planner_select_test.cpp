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

const QueryPlan* generate_select(
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
    auto* select = std::get_if<BoundSelect>(&statement->kind);
    test.expect(select != nullptr, std::string{name} + ": bound SELECT");
    if (select == nullptr) {
        return nullptr;
    }
    PlannerResult planned = generate_plan(std::move(*select), catalog);
    auto* generated = std::get_if<Plan>(&planned.outcome);
    test.expect(generated != nullptr, std::string{name} + ": planner success");
    if (generated == nullptr) {
        return nullptr;
    }
    plan = std::move(*generated);
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

const SortNode* project_sort(const ProjectNode* project) {
    return project == nullptr || project->child == nullptr
        ? nullptr
        : std::get_if<SortNode>(&project->child->kind);
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
        test.expect(scan->columns.size() == 3U, std::string{name} + ": all scan columns");
        for (std::size_t index = 0; index < scan->columns.size(); ++index) {
            test.expect(
                scan->columns[index].column_id == static_cast<ColumnId>(index) &&
                    scan->columns[index].output_slot == static_cast<SlotId>(index),
                std::string{name} + ": deterministic scan mapping");
            for (std::size_t other = index + 1U; other < scan->columns.size(); ++other) {
                test.expect(
                    scan->columns[index].output_slot != scan->columns[other].output_slot,
                    std::string{name} + ": unique scan slots");
            }
        }
    }
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

Plan placeholder_plan() {
    return Plan{QueryPlan{
        std::make_unique<PlanNode>(SeqScanNode{0U, {}}),
        {}}};
}

}  // namespace

int main() {
    TestContext test;
    const std::vector<TableMeta> tables{
        TableMeta{7U, "student", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"name", Type::kVarchar, true},
            ColumnMeta{"age", Type::kInt},
        }},
        TableMeta{8U, "other", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"student_id", Type::kInt},
            ColumnMeta{"score", Type::kInt},
        }},
        TableMeta{9U, "third", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"other_id", Type::kInt},
        }}
    };
    const CatalogView catalog{tables};

    {
        SemanticResult result = analyze_sql(
            test, "without WHERE", "SELECT name,id FROM student;", catalog);
        Plan plan = placeholder_plan();
        const QueryPlan* query = generate_select(test, "without WHERE", result, catalog, plan);
        const ProjectNode* project = project_root(query);
        test.expect(project != nullptr, "without WHERE: Project root");
        if (project != nullptr) {
            test.expect(project->outputs == std::vector<SlotId>{1U, 0U}, "without WHERE: outputs");
            test.expect(project->child != nullptr, "without WHERE: non-null Project child");
            expect_scan(test, "without WHERE", project->child.get());
        }
        test.expect(query != nullptr && query->outputs.size() == 2U, "without WHERE: metadata size");
        if (query != nullptr && query->outputs.size() == 2U && project != nullptr) {
            test.expect(query->outputs[0].slot_id == 1U && query->outputs[0].name == "name" &&
                            query->outputs[0].type == Type::kVarchar && query->outputs[0].nullable,
                        "without WHERE: name metadata");
            test.expect(query->outputs[1].slot_id == 0U && query->outputs[1].name == "id" &&
                            query->outputs[1].type == Type::kInt && !query->outputs[1].nullable,
                        "without WHERE: id metadata");
            test.expect(project->outputs.size() == query->outputs.size(),
                        "without WHERE: project/metadata size invariant");
        }
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "ORDER BY",
            "SELECT name FROM student WHERE id > 0 ORDER BY age DESC,id;",
            catalog);
        Plan plan = placeholder_plan();
        const QueryPlan* query = generate_select(test, "ORDER BY", result, catalog, plan);
        const ProjectNode* project = project_root(query);
        const SortNode* sort = project_sort(project);
        test.expect(project != nullptr && project->outputs == std::vector<SlotId>{1U},
                    "ORDER BY: projection excludes hidden keys");
        test.expect(sort != nullptr && sort->keys.size() == 2U,
                    "ORDER BY: Sort with two keys");
        if (sort != nullptr && sort->keys.size() == 2U) {
            test.expect(sort->keys[0].slot_id == 2U &&
                            sort->keys[0].direction == SortDirection::kDesc,
                        "ORDER BY: age c2 maps to s2 DESC");
            test.expect(sort->keys[1].slot_id == 0U &&
                            sort->keys[1].direction == SortDirection::kAsc,
                        "ORDER BY: id c0 maps to s0 default ASC");
            const auto* filter = sort->child == nullptr
                ? nullptr
                : std::get_if<FilterNode>(&sort->child->kind);
            const auto* scan = filter == nullptr || filter->child == nullptr
                ? nullptr
                : std::get_if<SeqScanNode>(&filter->child->kind);
            test.expect(scan != nullptr, "ORDER BY: Project -> Sort -> Filter -> SeqScan");
            test.expect(scan != nullptr && scan->columns[2].column_id == 2U &&
                            scan->columns[2].output_slot == sort->keys[0].slot_id,
                        "ORDER BY: key validated through ScanColumn mapping");
        }
        test.expect(query != nullptr && query->outputs.size() == 1U &&
                        query->outputs[0].slot_id == 1U,
                    "ORDER BY: QueryOutput remains SELECT list only");
    }

    {
        SemanticResult result = analyze_sql(
            test, "IS NULL", "SELECT name FROM student WHERE name IS NULL;", catalog);
        Plan plan = placeholder_plan();
        const FilterNode* filter = project_filter(
            project_root(generate_select(test, "IS NULL", result, catalog, plan)));
        const auto* null_test = filter == nullptr
            ? nullptr
            : std::get_if<NullTest>(&filter->predicate.kind);
        test.expect(
            null_test != nullptr && null_test->op == NullTestOp::kIsNull,
            "IS NULL: dedicated public node");
        const auto* column = null_test == nullptr || !null_test->operand
            ? nullptr
            : std::get_if<ColumnRef>(&null_test->operand->kind);
        test.expect(column != nullptr && column->slot_id == 1U,
                    "IS NULL: name maps to s1");
        const ProjectNode* project = project_root(std::get_if<QueryPlan>(&plan.kind));
        const FilterNode* planned_filter = project_filter(project);
        const SeqScanNode* scan = planned_filter == nullptr
            ? nullptr
            : scan_node(planned_filter->child.get());
        test.expect(
            scan != nullptr && null_test != nullptr &&
                expression_slots_are_mapped(filter->predicate, scan->columns),
            "IS NULL: operand slot mapped by scan");
    }

    {
        SemanticResult result = analyze_sql(test, "star", "SELECT * FROM student;", catalog);
        Plan plan = placeholder_plan();
        const QueryPlan* query = generate_select(test, "star", result, catalog, plan);
        const ProjectNode* project = project_root(query);
        test.expect(project != nullptr, "star: Project root");
        if (project != nullptr) {
            test.expect(project->outputs == std::vector<SlotId>{0U, 1U, 2U}, "star: expanded outputs");
            expect_scan(test, "star", project->child.get());
        }
        test.expect(query != nullptr && query->outputs.size() == 3U, "star: metadata size");
        if (query != nullptr && query->outputs.size() == 3U) {
            test.expect(query->outputs[0].name == "id" && query->outputs[0].slot_id == 0U,
                        "star: id metadata");
            test.expect(query->outputs[1].name == "name" && query->outputs[1].slot_id == 1U,
                        "star: name metadata");
            test.expect(query->outputs[2].name == "age" && query->outputs[2].slot_id == 2U,
                        "star: age metadata");
        }
    }

    {
        SemanticResult result = analyze_sql(
            test, "projection order", "SELECT id,id FROM student;", catalog);
        Plan plan = placeholder_plan();
        const ProjectNode* project = project_root(
            generate_select(test, "projection order", result, catalog, plan));
        const auto* query = std::get_if<QueryPlan>(&plan.kind);
        test.expect(project != nullptr, "projection order: Project root");
        if (project != nullptr) {
            test.expect(
                project->outputs == std::vector<SlotId>{0U, 0U},
                "projection order: reorder and duplicate retained");
        }
        test.expect(query != nullptr && query->outputs.size() == 2U,
                    "projection order: duplicate metadata retained");
        if (query != nullptr && query->outputs.size() == 2U && project != nullptr) {
            for (std::size_t index = 0; index < project->outputs.size(); ++index) {
                test.expect(project->outputs[index] == query->outputs[index].slot_id,
                            "projection order: positional metadata invariant");
            }
            test.expect(query->outputs[0].name == "id" && query->outputs[1].name == "id" &&
                            query->outputs[0].slot_id == 0U && query->outputs[1].slot_id == 0U,
                        "projection order: repeated metadata");
        }
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "simple WHERE",
            "SELECT name FROM student WHERE age >= 18;",
            catalog);
        Plan plan = placeholder_plan();
        const ProjectNode* project = project_root(
            generate_select(test, "simple WHERE", result, catalog, plan));
        test.expect(project != nullptr, "simple WHERE: Project root");
        const FilterNode* filter = project_filter(project);
        test.expect(filter != nullptr, "simple WHERE: Filter child");
        if (project != nullptr) {
            test.expect(project->outputs == std::vector<SlotId>{1U}, "simple WHERE: outputs");
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
            const SeqScanNode* scan = scan_node(filter->child.get());
            test.expect(scan != nullptr && expression_slots_are_mapped(filter->predicate, scan->columns),
                        "simple WHERE: predicate slots are mapped");
            test.expect(scan != nullptr && scan->columns.size() > 2U &&
                            scan->columns[2].column_id == 2U &&
                            scan->columns[2].output_slot == 2U,
                        "simple WHERE: age c2 maps to s2");
        }
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "complex WHERE",
            "SELECT name,id FROM student WHERE age >= 18 AND name != 'Tom';",
            catalog);
        Plan plan = placeholder_plan();
        const ProjectNode* project = project_root(
            generate_select(test, "complex WHERE", result, catalog, plan));
        const FilterNode* filter = project_filter(project);
        test.expect(project != nullptr && project->outputs == std::vector<SlotId>{1U, 0U}, "complex WHERE: outputs");
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
            project_root(generate_select(test, "NOT", result, catalog, plan)));
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
            project_root(generate_select(test, "parentheses", result, catalog, plan)));
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
            project_root(generate_select(test, "literal comparison", result, catalog, plan)));
        const Binary* comparison = filter == nullptr ? nullptr : binary(&filter->predicate);
        test.expect(has_compare(comparison, CmpOp::kLt), "literal comparison: < root");
        if (comparison != nullptr) {
            expect_integer(test, "literal comparison lhs", comparison->lhs.get(), 1);
            expect_integer(test, "literal comparison rhs", comparison->rhs.get(), 2);
        }
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "INNER JOIN",
            "SELECT student.name,other.score FROM student JOIN other "
            "ON student.id = other.student_id;",
            catalog);
        Plan plan = placeholder_plan();
        const QueryPlan* query = generate_select(test, "INNER JOIN", result, catalog, plan);
        const ProjectNode* project = project_root(query);
        const auto* join = project == nullptr || project->child == nullptr
            ? nullptr
            : std::get_if<JoinNode>(&project->child->kind);
        test.expect(project != nullptr && project->outputs == std::vector<SlotId>{1U, 5U},
                    "INNER JOIN: projection uses global slots");
        test.expect(join != nullptr && join->kind == JoinKind::kInner,
                    "INNER JOIN: public JoinNode");
        if (join != nullptr) {
            const auto* left = scan_node(join->left.get());
            const auto* right = scan_node(join->right.get());
            test.expect(left != nullptr && left->table_id == 7U &&
                            left->columns[0].output_slot == 0U &&
                            left->columns[2].output_slot == 2U,
                        "INNER JOIN: left slots s0-s2");
            test.expect(right != nullptr && right->table_id == 8U &&
                            right->columns[0].output_slot == 3U &&
                            right->columns[2].output_slot == 5U,
                        "INNER JOIN: right slots continue at s3");
            const Binary* condition = binary(&join->condition);
            test.expect(condition != nullptr, "INNER JOIN: comparison condition");
            if (condition != nullptr) {
                expect_column(test, "INNER JOIN left key", condition->lhs.get(), 0U);
                expect_column(test, "INNER JOIN right key", condition->rhs.get(), 4U);
            }
        }
        test.expect(query != nullptr && query->outputs.size() == 2U &&
                        query->outputs[0].name == "name" &&
                        query->outputs[1].name == "score",
                    "INNER JOIN: QueryOutput metadata");
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "chained JOIN",
            "SELECT third.id FROM student JOIN other ON student.id=other.student_id "
            "JOIN third ON other.id=third.other_id;",
            catalog);
        Plan plan = placeholder_plan();
        const ProjectNode* project = project_root(
            generate_select(test, "chained JOIN", result, catalog, plan));
        const auto* outer_join = project == nullptr || project->child == nullptr
            ? nullptr
            : std::get_if<JoinNode>(&project->child->kind);
        const auto* inner_join = outer_join == nullptr || !outer_join->left
            ? nullptr
            : std::get_if<JoinNode>(&outer_join->left->kind);
        const auto* right = outer_join == nullptr ? nullptr : scan_node(outer_join->right.get());
        test.expect(inner_join != nullptr && right != nullptr && right->table_id == 9U,
                    "chained JOIN: deterministic left-deep tree");
        test.expect(project != nullptr && project->outputs == std::vector<SlotId>{6U},
                    "chained JOIN: third relation continues global slots");
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "grouped aggregate",
            "SELECT name,COUNT(*),SUM(age) FROM student WHERE age>0 "
            "GROUP BY name ORDER BY name;",
            catalog);
        Plan plan = placeholder_plan();
        const QueryPlan* query = generate_select(
            test, "grouped aggregate", result, catalog, plan);
        const ProjectNode* project = project_root(query);
        const SortNode* sort = project_sort(project);
        const auto* aggregate = sort == nullptr || !sort->child
            ? nullptr
            : std::get_if<AggregateNode>(&sort->child->kind);
        const auto* filter = aggregate == nullptr || !aggregate->child
            ? nullptr
            : std::get_if<FilterNode>(&aggregate->child->kind);
        test.expect(project != nullptr &&
                        project->outputs == std::vector<SlotId>{1U, 3U, 4U},
                    "grouped aggregate: Project source and derived slots");
        test.expect(sort != nullptr && sort->keys.size() == 1U &&
                        sort->keys[0].slot_id == 1U,
                    "grouped aggregate: Sort sees group slot");
        test.expect(aggregate != nullptr &&
                        aggregate->group_keys == std::vector<SlotId>{1U} &&
                        aggregate->aggregates.size() == 2U,
                    "grouped aggregate: AggregateNode shape");
        if (aggregate != nullptr && aggregate->aggregates.size() == 2U) {
            const AggregateCall& count = aggregate->aggregates[0];
            const AggregateCall& sum = aggregate->aggregates[1];
            test.expect(count.kind == AggregateKind::kCount &&
                            !count.input_slot.has_value() && count.output_slot == 3U &&
                            count.output_type == Type::kBigInt && !count.nullable,
                        "grouped aggregate: COUNT derived s3");
            test.expect(sum.kind == AggregateKind::kSum && sum.input_slot == 2U &&
                            sum.output_slot == 4U && sum.output_type == Type::kBigInt &&
                            sum.nullable,
                        "grouped aggregate: SUM derived s4");
        }
        test.expect(filter != nullptr, "grouped aggregate: Filter before Aggregate");
        test.expect(query != nullptr && query->outputs.size() == 3U &&
                        query->outputs[1].name == "COUNT(*)" &&
                        query->outputs[1].slot_id == 3U &&
                        query->outputs[2].name == "SUM(age)" &&
                        query->outputs[2].slot_id == 4U,
                    "grouped aggregate: canonical QueryOutput metadata");
    }
    {
        SemanticResult result = analyze_sql(
            test,
            "hidden group key",
            "SELECT COUNT(*) FROM student GROUP BY name ORDER BY name;",
            catalog);
        Plan plan = placeholder_plan();
        const QueryPlan* query = generate_select(
            test, "hidden group key", result, catalog, plan);
        const ProjectNode* project = project_root(query);
        const SortNode* sort = project_sort(project);
        const auto* aggregate = sort == nullptr || !sort->child
            ? nullptr
            : std::get_if<AggregateNode>(&sort->child->kind);
        test.expect(project != nullptr && project->outputs == std::vector<SlotId>{3U},
                    "hidden group key: Project only COUNT");
        test.expect(sort != nullptr && sort->keys[0].slot_id == 1U,
                    "hidden group key: Sort uses retained group slot");
        test.expect(aggregate != nullptr && aggregate->group_keys == std::vector<SlotId>{1U},
                    "hidden group key: Aggregate retains name slot");
    }
    {
        SemanticResult result = analyze_sql(
            test,
            "JOIN aggregate",
            "SELECT student.name,COUNT(other.id),SUM(other.score) FROM student "
            "JOIN other ON student.id=other.student_id GROUP BY student.name;",
            catalog);
        Plan plan = placeholder_plan();
        const ProjectNode* project = project_root(
            generate_select(test, "JOIN aggregate", result, catalog, plan));
        const auto* aggregate = project == nullptr || !project->child
            ? nullptr
            : std::get_if<AggregateNode>(&project->child->kind);
        test.expect(project != nullptr &&
                        project->outputs == std::vector<SlotId>{1U, 6U, 7U},
                    "JOIN aggregate: derived slots follow all source slots");
        test.expect(aggregate != nullptr && aggregate->aggregates.size() == 2U &&
                        aggregate->aggregates[0].input_slot == 3U &&
                        aggregate->aggregates[0].output_slot == 6U &&
                        aggregate->aggregates[1].input_slot == 5U &&
                        aggregate->aggregates[1].output_slot == 7U,
                    "JOIN aggregate: relation-aware inputs and collision-free outputs");
        test.expect(aggregate != nullptr && aggregate->child != nullptr &&
                        std::holds_alternative<JoinNode>(aggregate->child->kind),
                    "JOIN aggregate: Join before Aggregate");
    }
    {
        SemanticResult result = analyze_sql(
            test,
            "group only",
            "SELECT name FROM student GROUP BY name;",
            catalog);
        Plan plan = placeholder_plan();
        const ProjectNode* project = project_root(
            generate_select(test, "group only", result, catalog, plan));
        const auto* aggregate = project == nullptr || !project->child
            ? nullptr
            : std::get_if<AggregateNode>(&project->child->kind);
        test.expect(aggregate != nullptr && aggregate->aggregates.empty() &&
                        aggregate->group_keys == std::vector<SlotId>{1U},
                    "group only: AggregateNode provides distinct groups");
    }

    if (test.failures() != 0) {
        std::cerr << test.failures() << " SELECT planner test assertion(s) failed\n";
        return 1;
    }
    return 0;
}
