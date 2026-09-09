#include <cstdint>
#include <iostream>
#include <span>
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
    void begin_case(std::string_view name) {
        current_case_ = name;
        ++case_count_;
    }

    void expect_equal(const std::string& actual, std::string_view expected) {
        if (actual != expected) {
            std::cerr << "FAIL: " << current_case_ << "\nEXPECTED:\n"
                      << expected << "ACTUAL:\n" << actual;
            ++failures_;
        }
    }

    void expect(bool condition, std::string_view message) {
        if (!condition) {
            std::cerr << "FAIL: " << current_case_ << ": " << message << '\n';
            ++failures_;
        }
    }

    [[nodiscard]] int failures() const { return failures_; }
    [[nodiscard]] int case_count() const { return case_count_; }

private:
    std::string current_case_;
    int failures_{0};
    int case_count_{0};
};

Plan plan_sql(std::string_view sql, CatalogView catalog, bool apply_optimizer) {
    LexResult lexed = tokenize(sql);
    auto tokens = std::get<std::vector<Token>>(std::move(lexed.outcome));
    ParseResult parsed = parse(tokens);
    const auto& statement = std::get<StatementAst>(parsed.outcome);
    SemanticResult analyzed = analyze(statement, catalog);
    BoundStatement bound = std::get<BoundStatement>(std::move(analyzed.outcome));
    if (apply_optimizer) {
        bound = optimize(std::move(bound));
    }
    return std::visit(
        [](auto&& value) { return generate_plan(std::move(value)); },
        std::move(bound.kind));
}

void test_basic_plans(TestContext& test, CatalogView catalog) {
    test.begin_case("QueryPlan full equality");
    const Plan query = plan_sql(
        "SELECT name,id FROM student WHERE age >= 18 AND name != 'Tom';",
        catalog,
        false);
    test.expect_equal(
        format_plan(query),
        "Project outputs=[1,0]\n"
        "└── Filter\n"
        "    ├── predicate: AND\n"
        "    │   ├── >=\n"
        "    │   │   ├── ColumnRef[2]\n"
        "    │   │   └── Literal(INT:18)\n"
        "    │   └── !=\n"
        "    │       ├── ColumnRef[1]\n"
        "    │       └── Literal(VARCHAR:\"Tom\")\n"
        "    └── SeqScan table_id=7\n");

    test.begin_case("CreateTable full equality");
    const Plan create = plan_sql(
        "CREATE TABLE course(id INT,title VARCHAR);",
        catalog,
        false);
    test.expect_equal(
        format_plan(create),
        "CreateTable name=course\n"
        "├── column[0] name=id type=INT\n"
        "└── column[1] name=title type=VARCHAR\n");

    test.begin_case("Insert explicit columns and escaping");
    const Plan insert{InsertPlan{
        7U,
        {2U, 0U, 1U},
        {
            {Value{std::int32_t{20}}, Value{std::int32_t{1}}, Value{std::string{"Alice"}}},
            {Value{std::int32_t{21}}, Value{std::int32_t{2}},
             Value{std::string{"Tom \"A\"\\\n\r\t"}}},
        }
    }};
    test.expect_equal(
        format_plan(insert),
        "Insert table_id=7\n"
        "├── columns=[2,0,1]\n"
        "├── row[0]=[INT:20,INT:1,VARCHAR:\"Alice\"]\n"
        "└── row[1]=[INT:21,INT:2,VARCHAR:\"Tom \\\"A\\\"\\\\\\n\\r\\t\"]\n");

    test.begin_case("Insert schema order");
    const Plan schema_order{InsertPlan{
        7U,
        {},
        {{Value{std::int32_t{1}}, Value{std::string{"Alice"}}, Value{std::int32_t{20}}}}
    }};
    test.expect_equal(
        format_plan(schema_order),
        "Insert table_id=7\n"
        "├── columns=<schema-order>\n"
        "└── row[0]=[INT:1,VARCHAR:\"Alice\",INT:20]\n");

    test.begin_case("Delete without predicate");
    const Plan delete_all = plan_sql("DELETE FROM student;", catalog, false);
    test.expect_equal(
        format_plan(delete_all),
        "Delete table_id=7\n"
        "└── predicate=<none>\n");

    test.begin_case("Delete with predicate");
    const Plan delete_one = plan_sql(
        "DELETE FROM student WHERE id = 1;",
        catalog,
        false);
    test.expect_equal(
        format_plan(delete_one),
        "Delete table_id=7\n"
        "└── predicate: =\n"
        "    ├── ColumnRef[0]\n"
        "    └── Literal(INT:1)\n");

    test.begin_case("Unary expression");
    const Plan unary = plan_sql(
        "SELECT id FROM student WHERE NOT id = 1;",
        catalog,
        false);
    test.expect_equal(
        format_plan(unary),
        "Project outputs=[0]\n"
        "└── Filter\n"
        "    ├── predicate: NOT\n"
        "    │   └── =\n"
        "    │       ├── ColumnRef[0]\n"
        "    │       └── Literal(INT:1)\n"
        "    └── SeqScan table_id=7\n");
}

void test_before_after(TestContext& test, CatalogView catalog) {
    test.begin_case("AND simplification before and after");
    const std::string_view and_sql =
        "SELECT name FROM student WHERE 1 = 1 AND age > 18;";
    const Plan and_before = plan_sql(and_sql, catalog, false);
    const Plan and_after = plan_sql(and_sql, catalog, true);
    test.expect_equal(
        format_plan(and_before),
        "Project outputs=[1]\n"
        "└── Filter\n"
        "    ├── predicate: AND\n"
        "    │   ├── =\n"
        "    │   │   ├── Literal(INT:1)\n"
        "    │   │   └── Literal(INT:1)\n"
        "    │   └── >\n"
        "    │       ├── ColumnRef[2]\n"
        "    │       └── Literal(INT:18)\n"
        "    └── SeqScan table_id=7\n");
    test.expect_equal(
        format_plan(and_after),
        "Project outputs=[1]\n"
        "└── Filter\n"
        "    ├── predicate: >\n"
        "    │   ├── ColumnRef[2]\n"
        "    │   └── Literal(INT:18)\n"
        "    └── SeqScan table_id=7\n");

    test.begin_case("TRUE Filter elimination before and after");
    const std::string_view true_sql = "SELECT name FROM student WHERE 1 = 1;";
    const Plan true_before = plan_sql(true_sql, catalog, false);
    const Plan true_after = plan_sql(true_sql, catalog, true);
    test.expect_equal(
        format_plan(true_before),
        "Project outputs=[1]\n"
        "└── Filter\n"
        "    ├── predicate: =\n"
        "    │   ├── Literal(INT:1)\n"
        "    │   └── Literal(INT:1)\n"
        "    └── SeqScan table_id=7\n");
    test.expect_equal(
        format_plan(true_after),
        "Project outputs=[1]\n"
        "└── SeqScan table_id=7\n");

    test.begin_case("Double-NOT before and after");
    const std::string_view not_sql =
        "SELECT name FROM student WHERE NOT NOT age > 18;";
    const Plan not_before = plan_sql(not_sql, catalog, false);
    const Plan not_after = plan_sql(not_sql, catalog, true);
    test.expect_equal(
        format_plan(not_before),
        "Project outputs=[1]\n"
        "└── Filter\n"
        "    ├── predicate: NOT\n"
        "    │   └── NOT\n"
        "    │       └── >\n"
        "    │           ├── ColumnRef[2]\n"
        "    │           └── Literal(INT:18)\n"
        "    └── SeqScan table_id=7\n");
    test.expect_equal(
        format_plan(not_after),
        "Project outputs=[1]\n"
        "└── Filter\n"
        "    ├── predicate: >\n"
        "    │   ├── ColumnRef[2]\n"
        "    │   └── Literal(INT:18)\n"
        "    └── SeqScan table_id=7\n");

    test.begin_case("complex defense example before and after");
    const std::string_view complex_sql =
        "SELECT name,id FROM student WHERE "
        "1 = 1 AND NOT NOT age >= 18 AND name != 'Tom';";
    const Plan complex_before = plan_sql(complex_sql, catalog, false);
    const Plan complex_after = plan_sql(complex_sql, catalog, true);
    test.expect_equal(
        format_plan(complex_before),
        "Project outputs=[1,0]\n"
        "└── Filter\n"
        "    ├── predicate: AND\n"
        "    │   ├── AND\n"
        "    │   │   ├── =\n"
        "    │   │   │   ├── Literal(INT:1)\n"
        "    │   │   │   └── Literal(INT:1)\n"
        "    │   │   └── NOT\n"
        "    │   │       └── NOT\n"
        "    │   │           └── >=\n"
        "    │   │               ├── ColumnRef[2]\n"
        "    │   │               └── Literal(INT:18)\n"
        "    │   └── !=\n"
        "    │       ├── ColumnRef[1]\n"
        "    │       └── Literal(VARCHAR:\"Tom\")\n"
        "    └── SeqScan table_id=7\n");
    test.expect_equal(
        format_plan(complex_after),
        "Project outputs=[1,0]\n"
        "└── Filter\n"
        "    ├── predicate: AND\n"
        "    │   ├── >=\n"
        "    │   │   ├── ColumnRef[2]\n"
        "    │   │   └── Literal(INT:18)\n"
        "    │   └── !=\n"
        "    │       ├── ColumnRef[1]\n"
        "    │       └── Literal(VARCHAR:\"Tom\")\n"
        "    └── SeqScan table_id=7\n");
}

void test_false_and_delete_optimization(TestContext& test, CatalogView catalog) {
    test.begin_case("FALSE Filter preservation");
    const std::string_view false_sql = "SELECT id FROM student WHERE 1 = 2;";
    const Plan false_before = plan_sql(false_sql, catalog, false);
    const Plan false_after = plan_sql(false_sql, catalog, true);
    const std::string expected =
        "Project outputs=[0]\n"
        "└── Filter\n"
        "    ├── predicate: =\n"
        "    │   ├── Literal(INT:1)\n"
        "    │   └── Literal(INT:2)\n"
        "    └── SeqScan table_id=7\n";
    test.expect_equal(format_plan(false_before), expected);
    test.expect_equal(format_plan(false_after), expected);

    test.begin_case("DELETE TRUE and FALSE optimization");
    const Plan delete_true_before = plan_sql(
        "DELETE FROM student WHERE 1 = 1;",
        catalog,
        false);
    const Plan delete_true_after = plan_sql(
        "DELETE FROM student WHERE 1 = 1;",
        catalog,
        true);
    const Plan delete_false_after = plan_sql(
        "DELETE FROM student WHERE 1 = 2;",
        catalog,
        true);
    test.expect_equal(
        format_plan(delete_true_before),
        "Delete table_id=7\n"
        "└── predicate: =\n"
        "    ├── Literal(INT:1)\n"
        "    └── Literal(INT:1)\n");
    test.expect_equal(
        format_plan(delete_true_after),
        "Delete table_id=7\n"
        "└── predicate=<none>\n");
    test.expect_equal(
        format_plan(delete_false_after),
        "Delete table_id=7\n"
        "└── predicate: =\n"
        "    ├── Literal(INT:1)\n"
        "    └── Literal(INT:2)\n");
}

void test_determinism(TestContext& test, CatalogView catalog) {
    test.begin_case("deterministic and non-consuming");
    const std::string_view sql =
        "SELECT name,id FROM student WHERE age >= 18 AND name != 'Tom';";
    const Plan first_plan = plan_sql(sql, catalog, true);
    const std::string first = format_plan(first_plan);
    const std::string same_plan_again = format_plan(first_plan);
    const Plan second_plan = plan_sql(sql, catalog, true);
    const std::string second = format_plan(second_plan);
    test.expect(first == same_plan_again, "same const Plan formats identically");
    test.expect(first == second, "equivalent Plans format identically");
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
    const CatalogView catalog{std::span<const TableMeta>{tables}};

    test_basic_plans(test, catalog);
    test_before_after(test, catalog);
    test_false_and_delete_optimization(test, catalog);
    test_determinism(test, catalog);

    if (test.failures() != 0) {
        std::cerr << test.failures() << " assertion(s) failed across "
                  << test.case_count() << " plan formatter cases\n";
        return 1;
    }
    return 0;
}
