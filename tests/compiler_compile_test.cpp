#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "tinydbms/compiler.hpp"

namespace {

using namespace tinydbms;
using namespace tinydbms::compiler;

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

CompileResult compile_sql(std::string_view sql, CatalogView catalog) {
    return compile(CompileRequest{std::string{sql}, catalog});
}

const Plan* expect_plan(
    TestContext& test,
    std::string_view name,
    const CompileResult& result) {
    const auto* plan = std::get_if<Plan>(&result.outcome);
    test.expect(plan != nullptr, std::string{name} + ": compile success");
    return plan;
}

void expect_error(
    TestContext& test,
    std::string_view name,
    std::string_view sql,
    CatalogView catalog,
    CompileErrorKind kind,
    SourceLocation location) {
    const CompileResult result = compile_sql(sql, catalog);
    const auto* error = std::get_if<CompileError>(&result.outcome);
    const std::string prefix{name};
    test.expect(error != nullptr, prefix + ": compile error");
    if (error == nullptr) {
        return;
    }
    test.expect(error->kind == kind, prefix + ": error kind");
    test.expect(error->location.line == location.line, prefix + ": error line");
    test.expect(error->location.column == location.column, prefix + ": error column");
}

const ProjectNode* project_root(const Plan* plan) {
    const auto* query = plan == nullptr ? nullptr : std::get_if<QueryPlan>(&plan->kind);
    return query == nullptr || query->root == nullptr
        ? nullptr
        : std::get_if<ProjectNode>(&query->root->kind);
}

const FilterNode* filter_child(const ProjectNode* project) {
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

const std::int32_t* integer_value(const Value& value) {
    return std::get_if<std::int32_t>(&value.data);
}

const std::string* string_value(const Value& value) {
    return std::get_if<std::string>(&value.data);
}

const FilterNode* expect_age_filter(
    TestContext& test,
    std::string_view name,
    const CompileResult& result) {
    const ProjectNode* project = project_root(expect_plan(test, name, result));
    const FilterNode* filter = filter_child(project);
    test.expect(filter != nullptr, std::string{name} + ": Filter");
    const Binary* comparison = filter == nullptr ? nullptr : binary(&filter->predicate);
    test.expect(has_compare(comparison, CmpOp::kGt), std::string{name} + ": age > root");
    if (comparison != nullptr) {
        expect_column(test, name, comparison->lhs.get(), 2U);
        expect_integer(test, name, comparison->rhs.get(), 18);
    }
    test.expect(
        filter != nullptr && scan_node(filter->child.get()) != nullptr,
        std::string{name} + ": SeqScan");
    return filter;
}

void test_public_optimization(TestContext& test, CatalogView catalog) {
    {
        const CompileResult result = compile_sql(
            "SELECT name FROM student WHERE 1 = 1;",
            catalog);
        const ProjectNode* project = project_root(expect_plan(test, "O1 SELECT TRUE", result));
        test.expect(project != nullptr && filter_child(project) == nullptr, "O1: no Filter");
        test.expect(
            project != nullptr && scan_node(project->child.get()) != nullptr,
            "O1: Project directly contains SeqScan");
    }
    {
        const CompileResult result = compile_sql(
            "SELECT name FROM student WHERE 1 = 2;",
            catalog);
        const ProjectNode* project = project_root(expect_plan(test, "O2 SELECT FALSE", result));
        const FilterNode* filter = filter_child(project);
        const Binary* comparison = filter == nullptr ? nullptr : binary(&filter->predicate);
        test.expect(has_compare(comparison, CmpOp::kEq), "O2: false Filter retained");
        if (comparison != nullptr) {
            expect_integer(test, "O2 lhs", comparison->lhs.get(), 1);
            expect_integer(test, "O2 rhs", comparison->rhs.get(), 2);
        }
    }
    {
        const CompileResult result = compile_sql(
            "SELECT name FROM student WHERE 1 = 1 AND age > 18;",
            catalog);
        expect_age_filter(test, "O3 AND", result);
    }
    {
        const CompileResult result = compile_sql(
            "SELECT name FROM student WHERE 1 = 2 OR age > 18;",
            catalog);
        expect_age_filter(test, "O4 OR", result);
    }
    {
        const CompileResult result = compile_sql(
            "SELECT name FROM student WHERE 1 = 1 OR age > 18;",
            catalog);
        const ProjectNode* project = project_root(expect_plan(test, "O5 TRUE OR", result));
        test.expect(project != nullptr && filter_child(project) == nullptr, "O5: no Filter");
        test.expect(
            project != nullptr && scan_node(project->child.get()) != nullptr,
            "O5: Project directly contains SeqScan");
    }
    {
        const CompileResult result = compile_sql(
            "SELECT name FROM student WHERE NOT NOT age > 18;",
            catalog);
        const FilterNode* filter = expect_age_filter(test, "O6 Double-NOT", result);
        test.expect(
            filter != nullptr && unary(&filter->predicate) == nullptr,
            "O6: no Unary root");
    }
    {
        const CompileResult result = compile_sql(
            "SELECT name,id FROM student WHERE "
            "1 = 1 AND NOT NOT age >= 18 AND name != 'Tom';",
            catalog);
        const ProjectNode* project = project_root(expect_plan(test, "O7 combined", result));
        test.expect(
            project != nullptr && project->outputs == std::vector<ColumnId>{1U, 0U},
            "O7: outputs");
        const FilterNode* filter = filter_child(project);
        const Binary* conjunction = filter == nullptr ? nullptr : binary(&filter->predicate);
        test.expect(has_logic(conjunction, LogicOp::kAnd), "O7: AND root");
        if (conjunction != nullptr) {
            const Binary* lhs = binary(conjunction->lhs.get());
            const Binary* rhs = binary(conjunction->rhs.get());
            test.expect(has_compare(lhs, CmpOp::kGe), "O7: lhs >=");
            test.expect(has_compare(rhs, CmpOp::kNe), "O7: rhs !=");
            if (lhs != nullptr && rhs != nullptr) {
                expect_column(test, "O7 age", lhs->lhs.get(), 2U);
                expect_integer(test, "O7 age value", lhs->rhs.get(), 18);
                expect_column(test, "O7 name", rhs->lhs.get(), 1U);
                expect_string(test, "O7 name value", rhs->rhs.get(), "Tom");
            }
        }
        test.expect(
            filter != nullptr && scan_node(filter->child.get()) != nullptr,
            "O7: SeqScan");
    }
    {
        const CompileResult result = compile_sql(
            "DELETE FROM student WHERE 1 = 1;",
            catalog);
        const Plan* plan = expect_plan(test, "O8 DELETE TRUE", result);
        const auto* deletion = plan == nullptr ? nullptr : std::get_if<DeletePlan>(&plan->kind);
        test.expect(
            deletion != nullptr && deletion->table_id == 7U && !deletion->predicate.has_value(),
            "O8: nullopt predicate");
    }
    {
        const CompileResult result = compile_sql(
            "DELETE FROM student WHERE 1 = 2;",
            catalog);
        const Plan* plan = expect_plan(test, "O9 DELETE FALSE", result);
        const auto* deletion = plan == nullptr ? nullptr : std::get_if<DeletePlan>(&plan->kind);
        const Binary* comparison = deletion == nullptr || !deletion->predicate.has_value()
            ? nullptr
            : binary(&*deletion->predicate);
        test.expect(has_compare(comparison, CmpOp::kEq), "O9: false predicate retained");
    }
    {
        const CompileResult result = compile_sql(
            "DELETE FROM student WHERE NOT NOT id = 1;",
            catalog);
        const Plan* plan = expect_plan(test, "O10 DELETE Double-NOT", result);
        const auto* deletion = plan == nullptr ? nullptr : std::get_if<DeletePlan>(&plan->kind);
        const Expr* predicate = deletion == nullptr || !deletion->predicate.has_value()
            ? nullptr
            : &*deletion->predicate;
        const Binary* comparison = binary(predicate);
        test.expect(has_compare(comparison, CmpOp::kEq), "O10: comparison root");
        test.expect(unary(predicate) == nullptr, "O10: no Unary root");
        if (comparison != nullptr) {
            expect_column(test, "O10 id", comparison->lhs.get(), 0U);
            expect_integer(test, "O10 id value", comparison->rhs.get(), 1);
        }
    }
}

void test_optimizer_stateless(TestContext& test, CatalogView catalog) {
    const std::string_view sql =
        "SELECT name FROM student WHERE 1 = 1 AND age > 18;";
    const CompileResult first = compile_sql(sql, catalog);
    const CompileResult second = compile_sql(sql, catalog);
    expect_age_filter(test, "optimized repeated first", first);
    expect_age_filter(test, "optimized repeated second", second);

    const CompileResult before_error = compile_sql(sql, catalog);
    expect_age_filter(test, "optimized before error", before_error);
    expect_error(
        test,
        "lex error between optimized queries",
        "SELECT @ FROM student;",
        catalog,
        CompileErrorKind::kLex,
        {1, 8});
    const CompileResult after_error = compile_sql(sql, catalog);
    expect_age_filter(test, "optimized after error", after_error);
}

}  // namespace

int main() {
    TestContext test;
    const std::vector<TableMeta> empty_tables;
    const CatalogView empty_catalog{std::span<const TableMeta>{empty_tables}};
    std::vector<TableMeta> tables{
        TableMeta{7U, "student", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"name", Type::kVarchar},
            ColumnMeta{"age", Type::kInt},
        }}
    };
    const CatalogView catalog{tables};

    {
        const CompileResult result = compile_sql(
            "CREATE TABLE student(id INT,name VARCHAR);",
            empty_catalog);
        const Plan* plan = expect_plan(test, "CREATE", result);
        const auto* create = plan == nullptr ? nullptr : std::get_if<CreateTablePlan>(&plan->kind);
        test.expect(create != nullptr, "CREATE: CreateTablePlan");
        if (create != nullptr) {
            test.expect(create->table_name == "student", "CREATE: table name");
            test.expect(create->columns.size() == 2, "CREATE: column count");
            if (create->columns.size() == 2) {
                test.expect(create->columns[0].name == "id", "CREATE: first column");
                test.expect(create->columns[0].type == Type::kInt, "CREATE: first type");
                test.expect(create->columns[1].name == "name", "CREATE: second column");
                test.expect(create->columns[1].type == Type::kVarchar, "CREATE: second type");
            }
        }
    }

    {
        const CompileResult result = compile_sql(
            "INSERT INTO student(age,id,name) VALUES (20,1,'Alice');",
            catalog);
        const Plan* plan = expect_plan(test, "INSERT", result);
        const auto* insert = plan == nullptr ? nullptr : std::get_if<InsertPlan>(&plan->kind);
        test.expect(insert != nullptr, "INSERT: InsertPlan");
        if (insert != nullptr) {
            test.expect(insert->table_id == 7U, "INSERT: TableId");
            test.expect(insert->columns == std::vector<ColumnId>{2U, 0U, 1U}, "INSERT: columns");
            test.expect(insert->rows.size() == 1 && insert->rows[0].size() == 3, "INSERT: row shape");
            if (insert->rows.size() == 1 && insert->rows[0].size() == 3) {
                const auto* age = integer_value(insert->rows[0][0]);
                const auto* id = integer_value(insert->rows[0][1]);
                const auto* name = string_value(insert->rows[0][2]);
                test.expect(age != nullptr && *age == 20, "INSERT: age value");
                test.expect(id != nullptr && *id == 1, "INSERT: id value");
                test.expect(name != nullptr && *name == "Alice", "INSERT: name value");
            }
        }
    }

    {
        const CompileResult result = compile_sql("SELECT name,id FROM student;", catalog);
        const ProjectNode* project = project_root(expect_plan(test, "SELECT without WHERE", result));
        test.expect(project != nullptr, "SELECT without WHERE: Project root");
        if (project != nullptr) {
            test.expect(project->outputs == std::vector<ColumnId>{1U, 0U}, "SELECT without WHERE: outputs");
            const auto* scan = scan_node(project->child.get());
            test.expect(scan != nullptr && scan->table_id == 7U, "SELECT without WHERE: SeqScan");
        }
    }

    {
        const CompileResult result = compile_sql(
            "SELECT name FROM student WHERE age >= 18;",
            catalog);
        const ProjectNode* project = project_root(expect_plan(test, "SELECT with WHERE", result));
        const FilterNode* filter = filter_child(project);
        test.expect(project != nullptr && project->outputs == std::vector<ColumnId>{1U}, "SELECT with WHERE: outputs");
        test.expect(filter != nullptr, "SELECT with WHERE: Filter");
        if (filter != nullptr) {
            const Binary* comparison = binary(&filter->predicate);
            test.expect(has_compare(comparison, CmpOp::kGe), "SELECT with WHERE: >=");
            if (comparison != nullptr) {
                expect_column(test, "SELECT with WHERE lhs", comparison->lhs.get(), 2U);
                expect_integer(test, "SELECT with WHERE rhs", comparison->rhs.get(), 18);
            }
            const auto* scan = scan_node(filter->child.get());
            test.expect(scan != nullptr && scan->table_id == 7U, "SELECT with WHERE: SeqScan");
        }
    }

    {
        const CompileResult result = compile_sql("SELECT * FROM student;", catalog);
        const ProjectNode* project = project_root(expect_plan(test, "SELECT star", result));
        test.expect(project != nullptr, "SELECT star: Project root");
        if (project != nullptr) {
            test.expect(project->outputs == std::vector<ColumnId>{0U, 1U, 2U}, "SELECT star: expanded outputs");
        }
    }

    {
        const CompileResult result = compile_sql("DELETE FROM student;", catalog);
        const Plan* plan = expect_plan(test, "DELETE without WHERE", result);
        const auto* deletion = plan == nullptr ? nullptr : std::get_if<DeletePlan>(&plan->kind);
        test.expect(deletion != nullptr, "DELETE without WHERE: DeletePlan");
        if (deletion != nullptr) {
            test.expect(deletion->table_id == 7U, "DELETE without WHERE: TableId");
            test.expect(!deletion->predicate.has_value(), "DELETE without WHERE: no predicate");
        }
    }

    {
        const CompileResult result = compile_sql("DELETE FROM student WHERE id = 1;", catalog);
        const Plan* plan = expect_plan(test, "DELETE with WHERE", result);
        const auto* deletion = plan == nullptr ? nullptr : std::get_if<DeletePlan>(&plan->kind);
        test.expect(
            deletion != nullptr && deletion->predicate.has_value(),
            "DELETE with WHERE: predicate");
        const Binary* comparison = deletion == nullptr || !deletion->predicate.has_value()
            ? nullptr
            : binary(&*deletion->predicate);
        test.expect(has_compare(comparison, CmpOp::kEq), "DELETE with WHERE: equality");
        if (comparison != nullptr) {
            expect_column(test, "DELETE with WHERE lhs", comparison->lhs.get(), 0U);
            expect_integer(test, "DELETE with WHERE rhs", comparison->rhs.get(), 1);
        }
    }

    {
        const CompileResult result = compile_sql(
            "SELECT name,id FROM student WHERE age >= 18 AND name != 'Tom';",
            catalog);
        const ProjectNode* project = project_root(expect_plan(test, "complex SELECT", result));
        const FilterNode* filter = filter_child(project);
        test.expect(project != nullptr && project->outputs == std::vector<ColumnId>{1U, 0U}, "complex SELECT: outputs");
        const Binary* conjunction = filter == nullptr ? nullptr : binary(&filter->predicate);
        test.expect(has_logic(conjunction, LogicOp::kAnd), "complex SELECT: AND root");
        if (conjunction != nullptr) {
            const Binary* lhs = binary(conjunction->lhs.get());
            const Binary* rhs = binary(conjunction->rhs.get());
            test.expect(has_compare(lhs, CmpOp::kGe), "complex SELECT: lhs >=");
            test.expect(has_compare(rhs, CmpOp::kNe), "complex SELECT: rhs !=");
            if (lhs != nullptr && rhs != nullptr) {
                expect_column(test, "complex SELECT age", lhs->lhs.get(), 2U);
                expect_integer(test, "complex SELECT age value", lhs->rhs.get(), 18);
                expect_column(test, "complex SELECT name", rhs->lhs.get(), 1U);
                expect_string(test, "complex SELECT name value", rhs->rhs.get(), "Tom");
            }
        }
        const auto* scan = filter == nullptr ? nullptr : scan_node(filter->child.get());
        test.expect(scan != nullptr && scan->table_id == 7U, "complex SELECT: SeqScan");
    }

    test_public_optimization(test, catalog);

    expect_error(test, "lex error", "SELECT @ FROM student;", catalog, CompileErrorKind::kLex, {1, 8});
    expect_error(test, "syntax error", "SELECT FROM student;", catalog, CompileErrorKind::kSyntax, {1, 8});
    expect_error(test, "semantic table", "SELECT * FROM unknown;", catalog, CompileErrorKind::kSemantic, {1, 15});
    expect_error(test, "semantic column", "SELECT score FROM student;", catalog, CompileErrorKind::kSemantic, {1, 8});
    expect_error(
        test,
        "semantic expression",
        "SELECT id FROM student WHERE name > 'Alice';",
        catalog,
        CompileErrorKind::kSemantic,
        {1, 35});
    expect_error(
        test,
        "missing semicolon",
        "SELECT * FROM student",
        catalog,
        CompileErrorKind::kSyntax,
        {1, 22});
    expect_error(
        test,
        "multiple statements",
        "SELECT * FROM student; DELETE FROM student;",
        catalog,
        CompileErrorKind::kSyntax,
        {1, 24});
    expect_error(test, "empty input", "", catalog, CompileErrorKind::kSyntax, {1, 1});
    expect_error(test, "whitespace input", "   ", catalog, CompileErrorKind::kSyntax, {1, 4});
    expect_error(test, "comment input", "-- comment", catalog, CompileErrorKind::kSyntax, {1, 11});

    {
        const CompileResult first = compile_sql("SELECT name,id FROM student;", catalog);
        const CompileResult middle = compile_sql("DELETE FROM student WHERE id = 1;", catalog);
        const CompileResult second = compile_sql("SELECT name,id FROM student;", catalog);
        const ProjectNode* first_project = project_root(expect_plan(test, "stateless first", first));
        const Plan* middle_plan = expect_plan(test, "stateless middle", middle);
        const ProjectNode* second_project = project_root(expect_plan(test, "stateless second", second));
        test.expect(
            first_project != nullptr && second_project != nullptr &&
                first_project->outputs == second_project->outputs,
            "stateless: repeated SELECT outputs");
        const auto* first_scan = first_project == nullptr
            ? nullptr
            : scan_node(first_project->child.get());
        const auto* second_scan = second_project == nullptr
            ? nullptr
            : scan_node(second_project->child.get());
        test.expect(
            first_scan != nullptr && second_scan != nullptr &&
                first_scan->table_id == second_scan->table_id,
            "stateless: repeated SELECT scans");
        const auto* middle_delete = middle_plan == nullptr
            ? nullptr
            : std::get_if<DeletePlan>(&middle_plan->kind);
        test.expect(middle_delete != nullptr && middle_delete->table_id == 7U, "stateless: middle DELETE");
    }

    test_optimizer_stateless(test, catalog);

    {
        const CompileResult result = compile_sql("CREATE TABLE course(id INT);", catalog);
        expect_plan(test, "CatalogView read-only", result);
        test.expect(tables.size() == 1, "CatalogView read-only: table count");
        if (tables.size() == 1) {
            test.expect(tables[0].table_id == 7U, "CatalogView read-only: TableId");
            test.expect(tables[0].table_name == "student", "CatalogView read-only: table name");
            test.expect(tables[0].columns.size() == 3, "CatalogView read-only: column count");
            if (tables[0].columns.size() == 3) {
                test.expect(tables[0].columns[0].name == "id", "CatalogView read-only: first column");
                test.expect(tables[0].columns[1].name == "name", "CatalogView read-only: second column");
                test.expect(tables[0].columns[2].name == "age", "CatalogView read-only: third column");
                test.expect(tables[0].columns[0].type == Type::kInt, "CatalogView read-only: first type");
                test.expect(tables[0].columns[1].type == Type::kVarchar, "CatalogView read-only: second type");
                test.expect(tables[0].columns[2].type == Type::kInt, "CatalogView read-only: third type");
            }
        }
    }

    expect_error(
        test,
        "CREATE existing table",
        "CREATE TABLE student(id INT);",
        catalog,
        CompileErrorKind::kSemantic,
        {1, 14});

    if (test.failures() != 0) {
        std::cerr << test.failures() << " public compile test assertion(s) failed\n";
        return 1;
    }
    return 0;
}
