#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
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

const InsertPlan* generate_insert(
    TestContext& test,
    std::string_view name,
    SemanticResult& result,
    Plan& plan) {
    auto* statement = std::get_if<BoundStatement>(&result.outcome);
    test.expect(statement != nullptr, std::string{name} + ": semantic success");
    if (statement == nullptr) {
        return nullptr;
    }
    auto* insert = std::get_if<BoundInsert>(&statement->kind);
    test.expect(insert != nullptr, std::string{name} + ": bound INSERT");
    if (insert == nullptr) {
        return nullptr;
    }
    plan = generate_plan(std::move(*insert));
    const auto* output = std::get_if<InsertPlan>(&plan.kind);
    test.expect(output != nullptr, std::string{name} + ": public InsertPlan");
    return output;
}

const std::int32_t* integer_value(const InsertPlan& plan, std::size_t row, std::size_t column) {
    return std::get_if<std::int32_t>(&plan.rows[row][column].data);
}

const std::string* string_value(const InsertPlan& plan, std::size_t row, std::size_t column) {
    return std::get_if<std::string>(&plan.rows[row][column].data);
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
            test, "omitted columns", "INSERT INTO student VALUES (1,'Alice',20);", catalog);
        Plan plan{InsertPlan{}};
        const auto* insert = generate_insert(test, "omitted columns", result, plan);
        if (insert != nullptr) {
            test.expect(insert->table_id == 7U, "omitted columns: TableId");
            test.expect(insert->columns.empty(), "omitted columns: remains empty");
            test.expect(insert->rows.size() == 1 && insert->rows[0].size() == 3, "omitted columns: row shape");
            if (insert->rows.size() == 1 && insert->rows[0].size() == 3) {
                const auto* id = integer_value(*insert, 0, 0);
                const auto* name = string_value(*insert, 0, 1);
                const auto* age = integer_value(*insert, 0, 2);
                test.expect(id != nullptr && *id == 1, "omitted columns: id value");
                test.expect(name != nullptr && *name == "Alice", "omitted columns: name value");
                test.expect(age != nullptr && *age == 20, "omitted columns: age value");
            }
        }
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "schema order",
            "INSERT INTO student(id,name,age) VALUES (1,'Alice',20);",
            catalog);
        Plan plan{InsertPlan{}};
        const auto* insert = generate_insert(test, "schema order", result, plan);
        if (insert != nullptr) {
            test.expect(insert->columns == std::vector<ColumnId>{0U, 1U, 2U}, "schema order: ColumnIds");
        }
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "explicit reorder",
            "INSERT INTO student(age,id,name) VALUES (20,1,'Alice');",
            catalog);
        Plan plan{InsertPlan{}};
        const auto* insert = generate_insert(test, "explicit reorder", result, plan);
        if (insert != nullptr && insert->rows.size() == 1 && insert->rows[0].size() == 3) {
            test.expect(insert->columns == std::vector<ColumnId>{2U, 0U, 1U}, "explicit reorder: ColumnIds");
            const auto* age = integer_value(*insert, 0, 0);
            const auto* id = integer_value(*insert, 0, 1);
            const auto* name = string_value(*insert, 0, 2);
            test.expect(age != nullptr && *age == 20, "explicit reorder: first value remains age");
            test.expect(id != nullptr && *id == 1, "explicit reorder: second value remains id");
            test.expect(name != nullptr && *name == "Alice", "explicit reorder: third value remains name");
        }
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "multiple rows",
            "INSERT INTO student VALUES (1,'Alice',20),(2,'Bob',21);",
            catalog);
        Plan plan{InsertPlan{}};
        const auto* insert = generate_insert(test, "multiple rows", result, plan);
        if (insert != nullptr && insert->rows.size() == 2) {
            const auto* first_id = integer_value(*insert, 0, 0);
            const auto* second_id = integer_value(*insert, 1, 0);
            const auto* second_name = string_value(*insert, 1, 1);
            test.expect(first_id != nullptr && *first_id == 1, "multiple rows: first row order");
            test.expect(second_id != nullptr && *second_id == 2, "multiple rows: second row order");
            test.expect(second_name != nullptr && *second_name == "Bob", "multiple rows: second string");
        }
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "reordered multiple rows",
            "INSERT INTO student(age,id,name) VALUES (20,1,'Alice'),(21,2,'Bob');",
            catalog);
        Plan plan{InsertPlan{}};
        const auto* insert = generate_insert(test, "reordered multiple rows", result, plan);
        if (insert != nullptr && insert->rows.size() == 2) {
            test.expect(insert->columns == std::vector<ColumnId>{2U, 0U, 1U}, "reordered rows: ColumnIds");
            const auto* first_age = integer_value(*insert, 0, 0);
            const auto* second_age = integer_value(*insert, 1, 0);
            const auto* second_name = string_value(*insert, 1, 2);
            test.expect(first_age != nullptr && *first_age == 20, "reordered rows: first age");
            test.expect(second_age != nullptr && *second_age == 21, "reordered rows: second age");
            test.expect(second_name != nullptr && *second_name == "Bob", "reordered rows: second name");
        }
    }

    if (test.failures() != 0) {
        std::cerr << test.failures() << " INSERT planner test assertion(s) failed\n";
        return 1;
    }
    return 0;
}
