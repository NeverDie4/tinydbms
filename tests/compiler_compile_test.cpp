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

struct ExpectedLocation {
    int line;
    int column;
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
    CompileStage stage,
    ExpectedLocation location,
    std::string_view message = {},
    bool expect_empty_extras = true) {
    const CompileResult result = compile_sql(sql, catalog);
    const auto* error = std::get_if<CompileError>(&result.outcome);
    const std::string prefix{name};
    test.expect(error != nullptr, prefix + ": compile error");
    if (error == nullptr) {
        return;
    }
    test.expect(error->stage == stage, prefix + ": error stage");
    test.expect(error->source.begin.line == location.line, prefix + ": error line");
    test.expect(error->source.begin.column == location.column, prefix + ": error column");
    if (!message.empty()) {
        test.expect(error->message == message, prefix + ": error message");
    }
    if (expect_empty_extras) {
        test.expect(!error->suggestion.has_value(), prefix + ": suggestion remains empty");
        test.expect(!error->fix_it.has_value(), prefix + ": fix-it remains empty");
    }
}

std::size_t source_offset(std::string_view source, SourceLocation location) {
    if (location.byte_offset <= source.size()) {
        return location.byte_offset;
    }
    int line = 1;
    int column = 1;
    for (std::size_t offset = 0; offset < source.size(); ++offset) {
        if (line == location.line && column == location.column) {
            return offset;
        }
        if (source[offset] == '\n') {
            ++line;
            column = 1;
        } else if (source[offset] != '\r') {
            ++column;
        }
    }
    return source.size();
}

std::string apply_fix_it(std::string_view source, const FixIt& fix_it) {
    const std::size_t begin = source_offset(source, fix_it.range.begin);
    const std::size_t end = source_offset(source, fix_it.range.end);
    return std::string{source.substr(0, begin)} + fix_it.replacement +
        std::string{source.substr(end)};
}

void expect_fixable(
    TestContext& test,
    std::string_view name,
    std::string_view sql,
    CatalogView catalog,
    std::string_view replacement,
    ExpectedLocation begin,
    ExpectedLocation end,
    std::optional<std::string_view> suggestion = std::nullopt) {
    const CompileResult result = compile_sql(sql, catalog);
    const auto* error = std::get_if<CompileError>(&result.outcome);
    const std::string prefix{name};
    test.expect(error != nullptr, prefix + ": compile error");
    if (error == nullptr) {
        return;
    }
    test.expect(error->fix_it.has_value(), prefix + ": fix-it exists");
    if (error->fix_it.has_value()) {
        test.expect(error->fix_it->range.begin.line == begin.line &&
                        error->fix_it->range.begin.column == begin.column &&
                        error->fix_it->range.end.line == end.line &&
                        error->fix_it->range.end.column == end.column,
                    prefix + ": exact half-open range");
        test.expect(error->fix_it->replacement == replacement, prefix + ": replacement");
        const CompileResult fixed = compile_sql(apply_fix_it(sql, *error->fix_it), catalog);
        test.expect(std::holds_alternative<Plan>(fixed.outcome), prefix + ": fixed SQL compiles");
    }
    if (suggestion.has_value()) {
        test.expect(error->suggestion == *suggestion, prefix + ": suggestion");
    }
}

const ProjectNode* project_root(const Plan* plan) {
    const auto* query = plan == nullptr ? nullptr : std::get_if<QueryPlan>(&plan->kind);
    return query == nullptr || query->root == nullptr
        ? nullptr
        : std::get_if<ProjectNode>(&query->root->kind);
}

const QueryPlan* query_plan(const Plan* plan) {
    return plan == nullptr ? nullptr : std::get_if<QueryPlan>(&plan->kind);
}

const FilterNode* filter_child(const ProjectNode* project) {
    return project == nullptr || project->child == nullptr
        ? nullptr
        : std::get_if<FilterNode>(&project->child->kind);
}

const SortNode* sort_child(const ProjectNode* project) {
    return project == nullptr || project->child == nullptr
        ? nullptr
        : std::get_if<SortNode>(&project->child->kind);
}

const SeqScanNode* scan_node(const PlanNode* node) {
    return node == nullptr ? nullptr : std::get_if<SeqScanNode>(&node->kind);
}

const ScanColumn* mapped_column(
    std::span<const ScanColumn> columns,
    SlotId slot_id) {
    for (const ScanColumn& column : columns) {
        if (column.output_slot == slot_id) {
            return &column;
        }
    }
    return nullptr;
}

void expect_student_mapping(
    TestContext& test,
    std::string_view name,
    std::span<const ScanColumn> columns) {
    const std::string prefix{name};
    test.expect(columns.size() == 3, prefix + ": full scan mapping");
    if (columns.size() != 3) {
        return;
    }
    for (std::size_t index = 0; index < columns.size(); ++index) {
        test.expect(
            columns[index].column_id == static_cast<ColumnId>(index),
            prefix + ": deterministic ColumnId");
        test.expect(
            columns[index].output_slot == static_cast<SlotId>(index),
            prefix + ": deterministic SlotId");
    }
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
    SlotId expected) {
    const auto* column = expression == nullptr
        ? nullptr
        : std::get_if<ColumnRef>(&expression->kind);
    test.expect(
        column != nullptr && column->slot_id == expected,
        std::string{name} + ": SlotId");
}

void expect_column_binding(
    TestContext& test,
    std::string_view name,
    const Expr* expression,
    std::span<const ScanColumn> columns,
    ColumnId expected_column) {
    const auto* column = expression == nullptr
        ? nullptr
        : std::get_if<ColumnRef>(&expression->kind);
    const ScanColumn* mapping = column == nullptr
        ? nullptr
        : mapped_column(columns, column->slot_id);
    test.expect(
        mapping != nullptr && mapping->column_id == expected_column,
        std::string{name} + ": SlotId maps to expected ColumnId");
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

void expect_bigint(
    TestContext& test,
    std::string_view name,
    const Expr* expression,
    std::int64_t expected) {
    const auto* literal = expression == nullptr
        ? nullptr
        : std::get_if<Literal>(&expression->kind);
    const auto* value = literal == nullptr
        ? nullptr
        : std::get_if<std::int64_t>(&literal->value.data);
    test.expect(value != nullptr && *value == expected, std::string{name} + ": BIGINT literal");
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

const std::int64_t* bigint_value(const Value& value) {
    return std::get_if<std::int64_t>(&value.data);
}

const double* double_value(const Value& value) {
    return std::get_if<double>(&value.data);
}

const bool* boolean_value(const Value& value) {
    return std::get_if<bool>(&value.data);
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
    const SeqScanNode* scan = filter == nullptr ? nullptr : scan_node(filter->child.get());
    if (comparison != nullptr) {
        expect_column(test, name, comparison->lhs.get(), 2U);
        expect_integer(test, name, comparison->rhs.get(), 18);
        if (scan != nullptr) {
            expect_column_binding(test, name, comparison->lhs.get(), scan->columns, 2U);
        }
    }
    test.expect(
        scan != nullptr,
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
            "UPDATE nullable_values SET note=NULL,active=FALSE WHERE id=1;",
            catalog);
        const Plan* plan = expect_plan(test, "UPDATE public compile", result);
        const auto* update = plan == nullptr ? nullptr : std::get_if<UpdatePlan>(&plan->kind);
        test.expect(update != nullptr && update->table_id == 11U,
                    "UPDATE public compile: TableId");
        test.expect(update != nullptr && update->assignments.size() == 2U &&
                        update->assignments[0].column_id == 1U &&
                        std::holds_alternative<std::monostate>(
                            update->assignments[0].value.data) &&
                        update->assignments[1].column_id == 2U &&
                        std::holds_alternative<bool>(update->assignments[1].value.data),
                    "UPDATE public compile: normalized assignments");
        test.expect(update != nullptr && update->input_columns.size() == 3U &&
                        update->predicate.has_value(),
                    "UPDATE public compile: input mapping and predicate");
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
            project != nullptr && project->outputs == std::vector<SlotId>{1U, 0U},
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
        const SeqScanNode* scan = filter == nullptr ? nullptr : scan_node(filter->child.get());
        test.expect(scan != nullptr, "O7: SeqScan");
        if (scan != nullptr && conjunction != nullptr) {
            const Binary* lhs = binary(conjunction->lhs.get());
            const Binary* rhs = binary(conjunction->rhs.get());
            if (lhs != nullptr && rhs != nullptr) {
                expect_column_binding(test, "O7 age", lhs->lhs.get(), scan->columns, 2U);
                expect_column_binding(test, "O7 name", rhs->lhs.get(), scan->columns, 1U);
            }
        }
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
        CompileStage::kLex,
        {1, 8});
    const CompileResult after_error = compile_sql(sql, catalog);
    expect_age_filter(test, "optimized after error", after_error);
}

void test_advanced_diagnostics(TestContext& test, CatalogView catalog) {
    expect_fixable(
        test, "keyword transposition", "SELETC id FROM student;", catalog,
        "SELECT", {1, 1}, {1, 7}, "did you mean 'SELECT'?");
    expect_fixable(
        test, "FROM typo", "SELECT id FORM student;", catalog,
        "FROM", {1, 11}, {1, 15}, "did you mean 'FROM'?");
    expect_fixable(
        test, "WHERE typo", "SELECT id FROM student WHRE id=1;", catalog,
        "WHERE", {1, 24}, {1, 28}, "did you mean 'WHERE'?");
    expect_fixable(
        test, "ORDER typo", "SELECT id FROM student ODER BY id;", catalog,
        "ORDER", {1, 24}, {1, 28}, "did you mean 'ORDER'?");
    expect_fixable(
        test, "UPDATE typo", "UPDTAE student SET age=1;", catalog,
        "UPDATE", {1, 1}, {1, 7}, "did you mean 'UPDATE'?");
    expect_fixable(
        test, "SET typo", "UPDATE student STE age=1;", catalog,
        "SET", {1, 16}, {1, 19}, "did you mean 'SET'?");
    expect_fixable(
        test, "JOIN typo",
        "SELECT student.id FROM student JION numeric_values "
        "ON student.id=numeric_values.id;",
        catalog, "JOIN", {1, 32}, {1, 36}, "did you mean 'JOIN'?");
    expect_fixable(
        test, "GROUP typo", "SELECT name,COUNT(*) FROM student GORUP BY name;", catalog,
        "GROUP", {1, 35}, {1, 40}, "did you mean 'GROUP'?");
    expect_fixable(
        test, "missing semicolon", "SELECT id FROM student", catalog,
        ";", {1, 23}, {1, 23});
    expect_fixable(
        test, "SELECT comma", "SELECT id name FROM student;", catalog,
        ",", {1, 11}, {1, 11});
    expect_fixable(
        test, "CREATE comma", "CREATE TABLE fresh(id INT name VARCHAR);", catalog,
        ",", {1, 27}, {1, 27});
    expect_fixable(
        test, "INSERT comma", "INSERT INTO numeric_values VALUES (1 2);", catalog,
        ",", {1, 38}, {1, 38});
    expect_fixable(
        test, "UPDATE UTF-8 comma", "UPDATE student SET name='测试' age=20;", catalog,
        ",", {1, 34}, {1, 34});
    expect_fixable(
        test, "GROUP BY comma",
        "SELECT name,COUNT(*) FROM student GROUP BY name age;", catalog,
        ",", {1, 49}, {1, 49});
    expect_fixable(
        test, "ORDER BY comma", "SELECT id FROM student ORDER BY age name;", catalog,
        ",", {1, 37}, {1, 37});
    expect_fixable(
        test, "aggregate right parenthesis", "SELECT COUNT(* FROM student;", catalog,
        ")", {1, 16}, {1, 16});
    expect_fixable(
        test, "CREATE right parenthesis", "CREATE TABLE fresh2(id INT;", catalog,
        ")", {1, 27}, {1, 27});
    expect_fixable(
        test, "INSERT right parenthesis", "INSERT INTO numeric_values VALUES (1,2;", catalog,
        ")", {1, 39}, {1, 39});

    {
        const CompileResult result = compile_sql("SELECT * FROM studnet;", catalog);
        const auto* error = std::get_if<CompileError>(&result.outcome);
        test.expect(error != nullptr &&
                        error->suggestion == "did you mean 'student'?" &&
                        !error->fix_it.has_value(),
                    "table suggestion has no automatic fix-it");
    }
    {
        const CompileResult result = compile_sql("SELECT naem FROM student;", catalog);
        const auto* error = std::get_if<CompileError>(&result.outcome);
        test.expect(error != nullptr &&
                        error->suggestion == "did you mean 'name'?" &&
                        !error->fix_it.has_value(),
                    "single-table column suggestion");
    }
    {
        const CompileResult result = compile_sql("SELECT student.naem FROM student;", catalog);
        const auto* error = std::get_if<CompileError>(&result.outcome);
        test.expect(error != nullptr &&
                        error->suggestion == "did you mean 'student.name'?" &&
                        !error->fix_it.has_value(),
                    "qualified column suggestion remains relation-local");
    }
    {
        const CompileResult result = compile_sql(
            "UPDATE student SET naem='alice';", catalog);
        const auto* error = std::get_if<CompileError>(&result.outcome);
        test.expect(error != nullptr &&
                        error->suggestion == "did you mean 'student.name'?" &&
                        !error->fix_it.has_value(),
                    "UPDATE target reuses column suggestion policy");
    }
    {
        const CompileResult result = compile_sql(
            "SELECT student.id FROM student JOIN numeric_vlaues "
            "ON student.id=numeric_vlaues.id;",
            catalog);
        const auto* error = std::get_if<CompileError>(&result.outcome);
        test.expect(error != nullptr &&
                        error->suggestion == "did you mean 'numeric_values'?" &&
                        !error->fix_it.has_value(),
                    "joined table suggestion comes from CatalogView");
    }
    {
        const CompileResult result = compile_sql("SELECT studnet.name FROM student;", catalog);
        const auto* error = std::get_if<CompileError>(&result.outcome);
        test.expect(error != nullptr &&
                        error->suggestion == "did you mean 'student.name'?" &&
                        !error->fix_it.has_value(),
                    "qualifier suggestion requires valid resulting column");
    }
    {
        const CompileResult result = compile_sql("SELECT studnet.missing FROM student;", catalog);
        const auto* error = std::get_if<CompileError>(&result.outcome);
        test.expect(error != nullptr && !error->suggestion.has_value(),
                    "qualifier suggestion suppressed when corrected reference is invalid");
    }
    {
        const CompileResult result = compile_sql("SELECT * FROM completely_unrelated;", catalog);
        const auto* error = std::get_if<CompileError>(&result.outcome);
        test.expect(error != nullptr && !error->suggestion.has_value() &&
                        !error->fix_it.has_value(),
                    "far typo has no suggestion");
    }
    {
        const std::vector<TableMeta> tie_tables{
            TableMeta{100U, "cat", {ColumnMeta{"id", Type::kInt}}},
            TableMeta{101U, "bat", {ColumnMeta{"id", Type::kInt}}}};
        const CompileResult result = compile_sql(
            "SELECT * FROM dat;", CatalogView{tie_tables});
        const auto* error = std::get_if<CompileError>(&result.outcome);
        test.expect(error != nullptr && !error->suggestion.has_value(),
                    "equal best table candidates suppress suggestion");
    }
    {
        const std::vector<TableMeta> ambiguous_tables{
            TableMeta{100U, "left_table", {ColumnMeta{"name", Type::kVarchar}}},
            TableMeta{101U, "right_table", {ColumnMeta{"name", Type::kVarchar}}}};
        const CompileResult result = compile_sql(
            "SELECT naem FROM left_table JOIN right_table ON TRUE;",
            CatalogView{ambiguous_tables});
        const auto* error = std::get_if<CompileError>(&result.outcome);
        test.expect(error != nullptr && !error->suggestion.has_value(),
                    "suggestion suppressed when corrected unqualified name is ambiguous");
    }
    {
        const CompileResult first = compile_sql("SELECT * FROM studnet;", catalog);
        const CompileResult second = compile_sql("SELECT * FROM studnet;", catalog);
        const auto* lhs = std::get_if<CompileError>(&first.outcome);
        const auto* rhs = std::get_if<CompileError>(&second.outcome);
        test.expect(lhs != nullptr && rhs != nullptr && lhs->stage == rhs->stage &&
                        lhs->source.begin.line == rhs->source.begin.line &&
                        lhs->source.begin.column == rhs->source.begin.column &&
                        lhs->source.begin.byte_offset == rhs->source.begin.byte_offset &&
                        lhs->source.end.line == rhs->source.end.line &&
                        lhs->source.end.column == rhs->source.end.column &&
                        lhs->source.end.byte_offset == rhs->source.end.byte_offset &&
                        lhs->message == rhs->message && lhs->suggestion == rhs->suggestion &&
                        !lhs->fix_it.has_value() && !rhs->fix_it.has_value(),
                    "diagnostic generation is deterministic");
    }
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
        }},
        TableMeta{8U, "numeric_values", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"big_id", Type::kBigInt},
        }},
        TableMeta{9U, "measurements", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"big_id", Type::kBigInt},
            ColumnMeta{"value", Type::kDouble},
        }},
        TableMeta{10U, "flags", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"active", Type::kBoolean},
        }},
        TableMeta{11U, "nullable_values", {
            ColumnMeta{"id", Type::kInt, false},
            ColumnMeta{"note", Type::kVarchar, true},
            ColumnMeta{"active", Type::kBoolean, true},
        }}
    };
    const CatalogView catalog{tables};

    test_advanced_diagnostics(test, catalog);

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
                test.expect(create->columns[0].nullable, "CREATE: first column defaults NULLABLE");
                test.expect(create->columns[1].name == "name", "CREATE: second column");
                test.expect(create->columns[1].type == Type::kVarchar, "CREATE: second type");
                test.expect(create->columns[1].nullable, "CREATE: second column defaults NULLABLE");
            }
        }
    }

    {
        const CompileResult result = compile_sql(
            "CREATE TABLE v2_double(value DOUBLE);",
            empty_catalog);
        const Plan* plan = expect_plan(test, "CREATE DOUBLE", result);
        const auto* create = plan == nullptr ? nullptr : std::get_if<CreateTablePlan>(&plan->kind);
        test.expect(
            create != nullptr && create->columns.size() == 1 &&
                create->columns[0].type == Type::kDouble,
            "CREATE DOUBLE: public schema");
    }

    for (const std::string_view sql : {
             "INSERT INTO student VALUES (NULL,'Alice',20);"}) {
        const CompileResult result = compile_sql(sql, catalog);
        test.expect(
            std::holds_alternative<CompileError>(result.outcome),
            std::string{"remaining SQL v2 grammar disabled: "} + std::string{sql});
    }

    {
        const CompileResult result = compile_sql(
            "INSERT INTO nullable_values VALUES (1,NULL,NULL);",
            catalog);
        const Plan* plan = expect_plan(
            test,
            "INSERT nullable NULL",
            result);
        const auto* insert = plan == nullptr ? nullptr : std::get_if<InsertPlan>(&plan->kind);
        test.expect(
            insert != nullptr && insert->rows.size() == 1U &&
                insert->rows[0].size() == 3U &&
                std::holds_alternative<std::monostate>(insert->rows[0][1].data) &&
                std::holds_alternative<std::monostate>(insert->rows[0][2].data),
            "INSERT nullable NULL: public Value monostate");
    }

    {
        const CompileResult result = compile_sql(
            "SELECT note FROM nullable_values WHERE note IS NULL;",
            catalog);
        const Plan* plan = expect_plan(
            test,
            "IS NULL public plan",
            result);
        const QueryPlan* query = query_plan(plan);
        const ProjectNode* project = project_root(plan);
        const FilterNode* filter = filter_child(project);
        const auto* null_test = filter == nullptr
            ? nullptr
            : std::get_if<NullTest>(&filter->predicate.kind);
        const SeqScanNode* scan = filter == nullptr ? nullptr : scan_node(filter->child.get());
        test.expect(
            null_test != nullptr && null_test->op == NullTestOp::kIsNull &&
                null_test->operand != nullptr,
            "IS NULL public plan: dedicated NullTest");
        if (null_test != nullptr && null_test->operand != nullptr && scan != nullptr) {
            expect_column_binding(
                test,
                "IS NULL public plan operand",
                null_test->operand.get(),
                scan->columns,
                1U);
        }
        test.expect(
            project != nullptr && query != nullptr && project->outputs.size() == 1U &&
                query->outputs.size() == 1U &&
                project->outputs[0] == query->outputs[0].slot_id &&
                query->outputs[0].name == "note" &&
                query->outputs[0].type == Type::kVarchar &&
                query->outputs[0].nullable,
            "IS NULL public plan: nullable QueryOutput");
    }

    {
        const CompileResult result = compile_sql(
            "CREATE TABLE v2_boolean(active BOOLEAN);", empty_catalog);
        const Plan* plan = expect_plan(
            test, "CREATE BOOLEAN", result);
        const auto* create = plan == nullptr ? nullptr : std::get_if<CreateTablePlan>(&plan->kind);
        test.expect(
            create != nullptr && create->columns.size() == 1 &&
                create->columns[0].type == Type::kBoolean && create->columns[0].nullable,
            "CREATE BOOLEAN: public schema");
    }
    {
        const CompileResult result = compile_sql(
            "INSERT INTO flags VALUES (1,TRUE),(2,FALSE);", catalog);
        const Plan* plan = expect_plan(
            test, "INSERT BOOLEAN", result);
        const auto* insert = plan == nullptr ? nullptr : std::get_if<InsertPlan>(&plan->kind);
        test.expect(
            insert != nullptr && insert->rows.size() == 2 &&
                boolean_value(insert->rows[0][1]) != nullptr &&
                *boolean_value(insert->rows[0][1]) &&
                boolean_value(insert->rows[1][1]) != nullptr &&
                !*boolean_value(insert->rows[1][1]),
            "INSERT BOOLEAN: Value bool alternatives");
    }
    for (const std::string_view sql : {
             "SELECT active FROM flags WHERE active;",
             "SELECT active FROM flags WHERE NOT active;",
             "SELECT active FROM flags WHERE TRUE;",
             "SELECT active FROM flags WHERE FALSE;",
             "SELECT active FROM flags WHERE active = TRUE;",
             "SELECT active FROM flags WHERE active != FALSE;",
             "DELETE FROM flags WHERE active;"}) {
        expect_plan(test, "BOOLEAN predicate", compile_sql(sql, catalog));
    }
    for (const std::string_view sql : {
             "SELECT active FROM flags WHERE active < TRUE;",
             "SELECT active FROM flags WHERE active >= FALSE;",
             "SELECT active FROM flags WHERE active = 1;",
             "SELECT active FROM flags WHERE 1 AND TRUE;"}) {
        const CompileResult result = compile_sql(sql, catalog);
        const auto* error = std::get_if<CompileError>(&result.outcome);
        test.expect(
            error != nullptr && error->stage == CompileStage::kSemantic,
            std::string{"BOOLEAN semantic rejection: "} + std::string{sql});
    }

    {
        const CompileResult result = compile_sql(
            "INSERT INTO measurements VALUES (1,2147483648,12.5);",
            catalog);
        const Plan* plan = expect_plan(test, "INSERT DOUBLE", result);
        const auto* insert = plan == nullptr ? nullptr : std::get_if<InsertPlan>(&plan->kind);
        test.expect(
            insert != nullptr && insert->rows.size() == 1 &&
                double_value(insert->rows[0][2]) != nullptr &&
                *double_value(insert->rows[0][2]) == 12.5,
            "INSERT DOUBLE: literal remains double");
    }
    for (const auto& [spelling, expected] : std::vector<std::pair<std::string_view, double>>{
             {"0.0", 0.0}, {"1.0", 1.0}, {"12.5", 12.5}, {"00012.500", 12.5}}) {
        const CompileResult result = compile_sql(
            "INSERT INTO measurements VALUES (1,2147483648," +
                std::string{spelling} + ");",
            catalog);
        const Plan* plan = expect_plan(test, "DOUBLE spelling", result);
        const auto* insert = plan == nullptr ? nullptr : std::get_if<InsertPlan>(&plan->kind);
        test.expect(
            insert != nullptr && double_value(insert->rows[0][2]) != nullptr &&
                *double_value(insert->rows[0][2]) == expected,
            "DOUBLE spelling parsed as double: " + std::string{spelling});
    }
    for (const std::int64_t source : {std::int64_t{1}, std::int64_t{2147483648},
                                      std::int64_t{9007199254740993}}) {
        const CompileResult result = compile_sql(
            "INSERT INTO measurements VALUES (1,2147483648," +
                std::to_string(source) + ");",
            catalog);
        const Plan* plan = expect_plan(test, "INSERT numeric to DOUBLE", result);
        const auto* insert = plan == nullptr ? nullptr : std::get_if<InsertPlan>(&plan->kind);
        test.expect(
            insert != nullptr && double_value(insert->rows[0][2]) != nullptr &&
                *double_value(insert->rows[0][2]) == static_cast<double>(source),
            "INSERT numeric to DOUBLE: explicit widening");
    }

    {
        const CompileResult result = compile_sql(
            "SELECT value FROM measurements WHERE big_id < 2147483648.5;",
            catalog);
        const Plan* plan = expect_plan(test, "DOUBLE query", result);
        const QueryPlan* query = query_plan(plan);
        test.expect(
            query != nullptr && query->outputs.size() == 1 &&
                query->outputs[0].type == Type::kDouble,
            "DOUBLE query: QueryOutput type");
    }
    expect_plan(
        test,
        "DELETE DOUBLE predicate",
        compile_sql("DELETE FROM measurements WHERE value = 12.5;", catalog));
    expect_error(
        test,
        "DOUBLE to BIGINT narrowing",
        "INSERT INTO numeric_values VALUES (1,1.5);",
        catalog,
        CompileStage::kSemantic,
        {1, 38});
    expect_error(
        test,
        "DOUBLE to INT narrowing",
        "INSERT INTO student VALUES (1.5,'Alice',20);",
        catalog,
        CompileStage::kSemantic,
        {1, 29});

    struct InvalidDoubleCase {
        std::string_view sql;
        CompileStage stage;
        ExpectedLocation location;
    };
    for (const InvalidDoubleCase invalid : {
             InvalidDoubleCase{"SELECT id FROM student WHERE id = .5;", CompileStage::kSyntax, {1, 35}},
             InvalidDoubleCase{"SELECT id FROM student WHERE id = 1.;", CompileStage::kSyntax, {1, 36}},
             InvalidDoubleCase{"SELECT id FROM student WHERE id = -1.5;", CompileStage::kLex, {1, 35}},
             InvalidDoubleCase{"SELECT id FROM student WHERE id = +1.5;", CompileStage::kLex, {1, 35}},
             InvalidDoubleCase{"SELECT id FROM student WHERE id = 1e3;", CompileStage::kSyntax, {1, 36}},
             InvalidDoubleCase{"SELECT id FROM student WHERE id = 1.0e3;", CompileStage::kSyntax, {1, 38}},
             InvalidDoubleCase{"SELECT id FROM student WHERE id = NaN;", CompileStage::kSemantic, {1, 35}},
             InvalidDoubleCase{"SELECT id FROM student WHERE id = Infinity;", CompileStage::kSemantic, {1, 35}},
         }) {
        expect_error(
            test,
            "invalid DOUBLE syntax",
            invalid.sql,
            catalog,
            invalid.stage,
            invalid.location);
    }

    {
        const CompileResult result = compile_sql(
            "CREATE TABLE big_table(id BIGINT,name VARCHAR);",
            empty_catalog);
        const Plan* plan = expect_plan(test, "CREATE BIGINT", result);
        const auto* create = plan == nullptr ? nullptr : std::get_if<CreateTablePlan>(&plan->kind);
        test.expect(
            create != nullptr && create->columns.size() == 2 &&
                create->columns[0].type == Type::kBigInt &&
                create->columns[0].nullable,
            "CREATE BIGINT: public schema");
    }

    {
        const CompileResult result = compile_sql(
            "INSERT INTO numeric_values VALUES (1,1);",
            catalog);
        const Plan* plan = expect_plan(test, "INSERT BIGINT widening", result);
        const auto* insert = plan == nullptr ? nullptr : std::get_if<InsertPlan>(&plan->kind);
        test.expect(
            insert != nullptr && insert->rows.size() == 1 &&
                insert->rows[0].size() == 2 &&
                integer_value(insert->rows[0][0]) != nullptr &&
                bigint_value(insert->rows[0][1]) != nullptr &&
                *bigint_value(insert->rows[0][1]) == 1,
            "INSERT BIGINT widening: final Plan stores int64");
    }

    {
        const CompileResult result = compile_sql(
            "SELECT big_id FROM numeric_values WHERE id = 2147483648;",
            catalog);
        const Plan* plan = expect_plan(test, "BIGINT query", result);
        const QueryPlan* query = query_plan(plan);
        const ProjectNode* project = project_root(plan);
        const FilterNode* filter = filter_child(project);
        const Binary* comparison = filter == nullptr ? nullptr : binary(&filter->predicate);
        test.expect(
            query != nullptr && query->outputs.size() == 1 &&
                query->outputs[0].slot_id == 1U &&
                query->outputs[0].name == "big_id" &&
                query->outputs[0].type == Type::kBigInt &&
                !query->outputs[0].nullable,
            "BIGINT query: QueryOutput type");
        if (comparison != nullptr) {
            expect_bigint(test, "BIGINT query predicate", comparison->rhs.get(), 2147483648LL);
            const auto* scan = scan_node(filter->child.get());
            if (scan != nullptr) {
                expect_column_binding(
                    test,
                    "BIGINT query predicate mapping",
                    comparison->lhs.get(),
                    scan->columns,
                    0U);
            }
        }
    }
    expect_plan(
        test,
        "BIGINT column compared with INT literal",
        compile_sql("SELECT big_id FROM numeric_values WHERE big_id = 1;", catalog));
    expect_plan(
        test,
        "BIGINT literal on comparison lhs",
        compile_sql("SELECT id FROM numeric_values WHERE 2147483648 > id;", catalog));
    expect_error(
        test,
        "VARCHAR compared with BIGINT",
        "SELECT id FROM student WHERE name = 2147483648;",
        catalog,
        CompileStage::kSemantic,
        {1, 35});

    expect_error(
        test,
        "INT64 overflow",
        "INSERT INTO numeric_values VALUES (1,9223372036854775808);",
        catalog,
        CompileStage::kLex,
        {1, 38},
        "integer literal out of range");
    expect_error(
        test,
        "negative integer rejected",
        "INSERT INTO numeric_values VALUES (1,-1);",
        catalog,
        CompileStage::kLex,
        {1, 38},
        "invalid character");

    struct IntegerBoundaryCase {
        std::string_view literal;
        bool is_bigint;
    };
    for (const IntegerBoundaryCase boundary : {
             IntegerBoundaryCase{"0", false},
             IntegerBoundaryCase{"1", false},
             IntegerBoundaryCase{"2147483646", false},
             IntegerBoundaryCase{"2147483647", false},
             IntegerBoundaryCase{"2147483648", true},
             IntegerBoundaryCase{"2147483649", true},
             IntegerBoundaryCase{"9223372036854775806", true},
             IntegerBoundaryCase{"9223372036854775807", true},
             IntegerBoundaryCase{"0000000000000000000000001", false},
             IntegerBoundaryCase{"0002147483647", false},
             IntegerBoundaryCase{"0002147483648", true},
             IntegerBoundaryCase{"0009223372036854775807", true},
         }) {
        const CompileResult result = compile_sql(
            "SELECT id FROM numeric_values WHERE id = " + std::string{boundary.literal} + ";",
            catalog);
        const ProjectNode* project = project_root(expect_plan(test, "integer boundary", result));
        const FilterNode* filter = filter_child(project);
        const Binary* comparison = filter == nullptr ? nullptr : binary(&filter->predicate);
        const auto* literal = comparison == nullptr || comparison->rhs == nullptr
            ? nullptr
            : std::get_if<Literal>(&comparison->rhs->kind);
        const bool correct_type = literal != nullptr &&
            (boundary.is_bigint
                ? std::holds_alternative<std::int64_t>(literal->value.data)
                : std::holds_alternative<std::int32_t>(literal->value.data));
        test.expect(
            correct_type,
            "integer boundary classification: " + std::string{boundary.literal});
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
        const Plan* plan = expect_plan(test, "SELECT without WHERE", result);
        const QueryPlan* query = query_plan(plan);
        const ProjectNode* project = project_root(plan);
        test.expect(project != nullptr, "SELECT without WHERE: Project root");
        if (project != nullptr) {
            test.expect(project->outputs == std::vector<SlotId>{1U, 0U}, "SELECT without WHERE: outputs");
            const auto* scan = scan_node(project->child.get());
            test.expect(scan != nullptr && scan->table_id == 7U, "SELECT without WHERE: SeqScan");
            if (scan != nullptr) {
                expect_student_mapping(test, "SELECT without WHERE", scan->columns);
            }
            test.expect(
                query != nullptr && query->outputs.size() == project->outputs.size(),
                "SELECT without WHERE: QueryOutput count");
            if (query != nullptr && query->outputs.size() == 2) {
                test.expect(
                    query->outputs[0].slot_id == project->outputs[0] &&
                        query->outputs[0].name == "name" &&
                        query->outputs[0].type == Type::kVarchar &&
                        !query->outputs[0].nullable,
                    "SELECT without WHERE: name metadata");
                test.expect(
                    query->outputs[1].slot_id == project->outputs[1] &&
                        query->outputs[1].name == "id" &&
                        query->outputs[1].type == Type::kInt &&
                        !query->outputs[1].nullable,
                    "SELECT without WHERE: id metadata");
            }
        }
    }

    {
        const CompileResult result = compile_sql(
            "SELECT name FROM student WHERE age >= 18;",
            catalog);
        const ProjectNode* project = project_root(expect_plan(test, "SELECT with WHERE", result));
        const FilterNode* filter = filter_child(project);
        test.expect(project != nullptr && project->outputs == std::vector<SlotId>{1U}, "SELECT with WHERE: outputs");
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
            if (scan != nullptr && comparison != nullptr) {
                expect_student_mapping(test, "SELECT with WHERE", scan->columns);
                expect_column_binding(
                    test,
                    "SELECT with WHERE lhs mapping",
                    comparison->lhs.get(),
                    scan->columns,
                    2U);
            }
        }
    }

    {
        const CompileResult result = compile_sql("SELECT * FROM student;", catalog);
        const ProjectNode* project = project_root(expect_plan(test, "SELECT star", result));
        test.expect(project != nullptr, "SELECT star: Project root");
        if (project != nullptr) {
            test.expect(project->outputs == std::vector<SlotId>{0U, 1U, 2U}, "SELECT star: expanded outputs");
        }
    }

    {
        const CompileResult result = compile_sql(
            "SELECT name FROM student WHERE id > 0 ORDER BY age DESC,id;",
            catalog);
        const Plan* plan = expect_plan(test, "SELECT ORDER BY", result);
        const QueryPlan* query = query_plan(plan);
        const ProjectNode* project = project_root(plan);
        const SortNode* sort = sort_child(project);
        test.expect(project != nullptr && project->outputs == std::vector<SlotId>{1U},
                    "SELECT ORDER BY: projected slot");
        test.expect(sort != nullptr && sort->keys.size() == 2U,
                    "SELECT ORDER BY: two sort keys");
        if (sort != nullptr && sort->keys.size() == 2U) {
            test.expect(sort->keys[0].slot_id == 2U &&
                            sort->keys[0].direction == SortDirection::kDesc,
                        "SELECT ORDER BY: hidden age slot DESC");
            test.expect(sort->keys[1].slot_id == 0U &&
                            sort->keys[1].direction == SortDirection::kAsc,
                        "SELECT ORDER BY: id default ASC");
            const auto* filter = sort->child == nullptr
                ? nullptr
                : std::get_if<FilterNode>(&sort->child->kind);
            const auto* scan = filter == nullptr || filter->child == nullptr
                ? nullptr
                : std::get_if<SeqScanNode>(&filter->child->kind);
            test.expect(scan != nullptr && mapped_column(scan->columns, 2U) != nullptr &&
                            mapped_column(scan->columns, 2U)->column_id == 2U,
                        "SELECT ORDER BY: key proven by ScanColumn mapping");
        }
        test.expect(query != nullptr && query->outputs.size() == 1U &&
                        query->outputs[0].slot_id == 1U,
                    "SELECT ORDER BY: QueryOutput excludes hidden sort keys");
    }

    {
        const CompileResult result = compile_sql("DELETE FROM student;", catalog);
        const Plan* plan = expect_plan(test, "DELETE without WHERE", result);
        const auto* deletion = plan == nullptr ? nullptr : std::get_if<DeletePlan>(&plan->kind);
        test.expect(deletion != nullptr, "DELETE without WHERE: DeletePlan");
        if (deletion != nullptr) {
            test.expect(deletion->table_id == 7U, "DELETE without WHERE: TableId");
            test.expect(!deletion->predicate.has_value(), "DELETE without WHERE: no predicate");
            expect_student_mapping(test, "DELETE without WHERE", deletion->input_columns);
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
            expect_student_mapping(test, "DELETE with WHERE", deletion->input_columns);
            expect_column_binding(
                test,
                "DELETE with WHERE lhs mapping",
                comparison->lhs.get(),
                deletion->input_columns,
                0U);
        }
    }

    {
        const CompileResult result = compile_sql(
            "SELECT name,id FROM student WHERE age >= 18 AND name != 'Tom';",
            catalog);
        const ProjectNode* project = project_root(expect_plan(test, "complex SELECT", result));
        const FilterNode* filter = filter_child(project);
        test.expect(project != nullptr && project->outputs == std::vector<SlotId>{1U, 0U}, "complex SELECT: outputs");
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

    expect_error(
        test,
        "lex error",
        "SELECT @ FROM student;",
        catalog,
        CompileStage::kLex,
        {1, 8},
        "invalid character");
    expect_error(
        test,
        "syntax error",
        "SELECT FROM student;",
        catalog,
        CompileStage::kSyntax,
        {1, 8},
        "expected SELECT column or '*'");
    expect_error(
        test,
        "semantic table",
        "SELECT * FROM unknown;",
        catalog,
        CompileStage::kSemantic,
        {1, 15},
        "table 'unknown' does not exist");
    expect_error(test, "semantic column", "SELECT score FROM student;", catalog, CompileStage::kSemantic, {1, 8});
    expect_error(
        test,
        "semantic expression",
        "SELECT id FROM student WHERE name > 'Alice';",
        catalog,
        CompileStage::kSemantic,
        {1, 35});
    expect_error(
        test,
        "missing semicolon",
        "SELECT * FROM student",
        catalog,
        CompileStage::kSyntax,
        {1, 22},
        {},
        false);
    expect_error(
        test,
        "multiple statements",
        "SELECT * FROM student; DELETE FROM student;",
        catalog,
        CompileStage::kSyntax,
        {1, 24});
    expect_error(test, "empty input", "", catalog, CompileStage::kSyntax, {1, 1});
    expect_error(test, "whitespace input", "   ", catalog, CompileStage::kSyntax, {1, 4});
    expect_error(test, "comment input", "-- comment", catalog, CompileStage::kSyntax, {1, 11});

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
        std::vector<TableMeta> join_tables = tables;
        join_tables.push_back(TableMeta{12U, "other", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"student_id", Type::kInt},
            ColumnMeta{"score", Type::kInt}}});
        const CatalogView join_catalog{join_tables};
        const CompileResult result = compile_sql(
            "SELECT student.name,other.score FROM student JOIN other "
            "ON student.id=other.student_id ORDER BY other.score;",
            join_catalog);
        const Plan* plan = expect_plan(test, "JOIN", result);
        const QueryPlan* query = query_plan(plan);
        const ProjectNode* project = project_root(plan);
        const SortNode* sort = sort_child(project);
        const auto* join = sort == nullptr || !sort->child
            ? nullptr
            : std::get_if<JoinNode>(&sort->child->kind);
        test.expect(project != nullptr && project->outputs == std::vector<SlotId>{1U, 5U},
                    "JOIN: global projection slots");
        test.expect(sort != nullptr && sort->keys.size() == 1U &&
                        sort->keys[0].slot_id == 5U,
                    "JOIN: qualified ORDER BY right slot");
        test.expect(join != nullptr && join->kind == JoinKind::kInner,
                    "JOIN: public InnerJoin plan");
        if (join != nullptr) {
            const auto* left = scan_node(join->left.get());
            const auto* right = scan_node(join->right.get());
            test.expect(left != nullptr && right != nullptr &&
                            left->columns.front().output_slot == 0U &&
                            right->columns.front().output_slot == 3U,
                        "JOIN: disjoint deterministic scan slots");
        }
        test.expect(query != nullptr && query->outputs.size() == 2U &&
                        query->outputs[0].name == "name" &&
                        query->outputs[1].name == "score",
                    "JOIN: final QueryOutput metadata");

        expect_error(
            test,
            "JOIN ambiguous column",
            "SELECT id FROM student JOIN other ON student.id=other.student_id;",
            join_catalog,
            CompileStage::kSemantic,
            {1, 8},
            "column 'id' is ambiguous");
    }

    {
        std::vector<TableMeta> aggregate_tables = tables;
        aggregate_tables.push_back(TableMeta{12U, "other", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"student_id", Type::kInt},
            ColumnMeta{"score", Type::kInt, true}}});
        const CatalogView aggregate_catalog{aggregate_tables};
        const CompileResult result = compile_sql(
            "SELECT student.name,COUNT(other.id),SUM(other.score) FROM student "
            "JOIN other ON student.id=other.student_id WHERE other.score>0 "
            "GROUP BY student.name ORDER BY student.name;",
            aggregate_catalog);
        const Plan* plan = expect_plan(test, "aggregate query", result);
        const QueryPlan* query = query_plan(plan);
        const ProjectNode* project = project_root(plan);
        const SortNode* sort = sort_child(project);
        const auto* aggregate = sort == nullptr || !sort->child
            ? nullptr
            : std::get_if<AggregateNode>(&sort->child->kind);
        const auto* filter = aggregate == nullptr || !aggregate->child
            ? nullptr
            : std::get_if<FilterNode>(&aggregate->child->kind);
        const auto* join = filter == nullptr || !filter->child
            ? nullptr
            : std::get_if<JoinNode>(&filter->child->kind);
        test.expect(project != nullptr && project->outputs == std::vector<SlotId>{1U, 6U, 7U},
                    "aggregate query: group and derived projection slots");
        test.expect(sort != nullptr && sort->keys.size() == 1U &&
                        sort->keys[0].slot_id == 1U,
                    "aggregate query: ORDER BY retained group slot");
        test.expect(aggregate != nullptr && aggregate->group_keys == std::vector<SlotId>{1U} &&
                        aggregate->aggregates.size() == 2U,
                    "aggregate query: AggregateNode shape");
        if (aggregate != nullptr && aggregate->aggregates.size() == 2U) {
            test.expect(aggregate->aggregates[0].kind == AggregateKind::kCount &&
                            aggregate->aggregates[0].input_slot == 3U &&
                            aggregate->aggregates[0].output_slot == 6U,
                        "aggregate query: COUNT derived slot");
            test.expect(aggregate->aggregates[1].kind == AggregateKind::kSum &&
                            aggregate->aggregates[1].input_slot == 5U &&
                            aggregate->aggregates[1].output_slot == 7U,
                        "aggregate query: SUM derived slot");
        }
        test.expect(join != nullptr, "aggregate query: Filter before Aggregate and Join source");
        test.expect(query != nullptr && query->outputs.size() == 3U &&
                        query->outputs[0].name == "name" &&
                        query->outputs[1].name == "COUNT(id)" &&
                        query->outputs[1].type == Type::kBigInt &&
                        !query->outputs[1].nullable &&
                        query->outputs[2].name == "SUM(score)" &&
                        query->outputs[2].type == Type::kBigInt &&
                        query->outputs[2].nullable,
                    "aggregate query: canonical QueryOutput metadata");
    }

    {
        const CompileResult result = compile_sql("CREATE TABLE course(id INT);", catalog);
        expect_plan(test, "CatalogView read-only", result);
        test.expect(tables.size() == 5, "CatalogView read-only: table count");
        if (tables.size() == 5) {
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
        CompileStage::kSemantic,
        {1, 14});

    if (test.failures() != 0) {
        std::cerr << test.failures() << " public compile test assertion(s) failed\n";
        return 1;
    }
    return 0;
}
