#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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
    PlannerResult planned = std::visit(
        [catalog](auto&& value) -> PlannerResult {
            using StatementType = std::decay_t<decltype(value)>;
            if constexpr (
                std::is_same_v<StatementType, BoundSelect> ||
                std::is_same_v<StatementType, BoundDelete> ||
                std::is_same_v<StatementType, BoundUpdate>) {
                return generate_plan(std::move(value), catalog);
            } else {
                return PlannerResult{generate_plan(std::move(value))};
            }
        },
        std::move(bound.kind));
    return std::get<Plan>(std::move(planned.outcome));
}

void test_basic_plans(TestContext& test, CatalogView catalog) {
    test.begin_case("QueryPlan full equality");
    const Plan query = plan_sql(
        "SELECT name,id FROM student WHERE age >= 18 AND name != 'Tom';",
        catalog,
        false);
    test.expect_equal(
        format_plan(query),
        "Project outputs=[s1,s0]\n"
        "└── Filter\n"
        "    ├── predicate: AND\n"
        "    │   ├── >=\n"
        "    │   │   ├── ColumnRef[s2]\n"
        "    │   │   └── Literal(INT:18)\n"
        "    │   └── !=\n"
        "    │       ├── ColumnRef[s1]\n"
        "    │       └── Literal(VARCHAR:\"Tom\")\n"
        "    └── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n");

    test.begin_case("ORDER BY full equality");
    const Plan ordered = plan_sql(
        "SELECT name FROM student WHERE id > 0 ORDER BY age DESC,id;",
        catalog,
        false);
    test.expect_equal(
        format_plan(ordered),
        "Project outputs=[s1]\n"
        "└── Sort keys=[s2 DESC,s0 ASC]\n"
        "    └── Filter\n"
        "        ├── predicate: >\n"
        "        │   ├── ColumnRef[s0]\n"
        "        │   └── Literal(INT:0)\n"
        "        └── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n");

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

    test.begin_case("BIGINT create and mixed predicate");
    const Plan bigint_create = plan_sql(
        "CREATE TABLE big_table(id BIGINT,name VARCHAR);",
        catalog,
        false);
    test.expect_equal(
        format_plan(bigint_create),
        "CreateTable name=big_table\n"
        "├── column[0] name=id type=BIGINT\n"
        "└── column[1] name=name type=VARCHAR\n");

    const Plan bigint_predicate = plan_sql(
        "SELECT id FROM student WHERE id < 2147483648;",
        catalog,
        false);
    test.expect_equal(
        format_plan(bigint_predicate),
        "Project outputs=[s0]\n"
        "└── Filter\n"
        "    ├── predicate: <\n"
        "    │   ├── ColumnRef[s0]\n"
        "    │   └── Literal(BIGINT:2147483648)\n"
        "    └── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n");

    test.begin_case("DOUBLE create and mixed predicate");
    const Plan double_create = plan_sql(
        "CREATE TABLE measurements(value DOUBLE);",
        catalog,
        false);
    test.expect_equal(
        format_plan(double_create),
        "CreateTable name=measurements\n"
        "└── column[0] name=value type=DOUBLE\n");

    const Plan double_predicate = plan_sql(
        "SELECT id FROM student WHERE id < 12.5;",
        catalog,
        false);
    test.expect_equal(
        format_plan(double_predicate),
        "Project outputs=[s0]\n"
        "└── Filter\n"
        "    ├── predicate: <\n"
        "    │   ├── ColumnRef[s0]\n"
        "    │   └── Literal(DOUBLE:12.5)\n"
        "    └── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n");

    const Plan double_one{InsertPlan{7U, {}, {{Value{1.0}}}}};
    test.expect_equal(
        format_plan(double_one),
        "Insert table_id=7\n"
        "├── columns=<schema-order>\n"
        "└── row[0]=[DOUBLE:1.0]\n");

    test.begin_case("BOOLEAN create, literals, and predicate");
    const Plan boolean_create = plan_sql(
        "CREATE TABLE boolean_table(active BOOLEAN);", catalog, false);
    test.expect_equal(
        format_plan(boolean_create),
        "CreateTable name=boolean_table\n"
        "└── column[0] name=active type=BOOLEAN\n");

    const Plan boolean_insert = plan_sql(
        "INSERT INTO flags VALUES (1,TRUE),(2,FALSE);", catalog, false);
    test.expect_equal(
        format_plan(boolean_insert),
        "Insert table_id=8\n"
        "├── columns=<schema-order>\n"
        "├── row[0]=[INT:1,BOOLEAN:TRUE]\n"
        "└── row[1]=[INT:2,BOOLEAN:FALSE]\n");

    const Plan boolean_predicate = plan_sql(
        "SELECT active FROM flags WHERE active AND TRUE;", catalog, false);
    test.expect_equal(
        format_plan(boolean_predicate),
        "Project outputs=[s1]\n"
        "└── Filter\n"
        "    ├── predicate: AND\n"
        "    │   ├── ColumnRef[s1]\n"
        "    │   └── Literal(BOOLEAN:TRUE)\n"
        "    └── SeqScan table_id=8 columns=[c0->s0,c1->s1]\n");

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

    test.begin_case("BIGINT InsertPlan value");
    const Plan bigint_insert{InsertPlan{
        8U,
        {},
        {{Value{std::int64_t{1}}, Value{std::int64_t{2147483648LL}}}}
    }};
    test.expect_equal(
        format_plan(bigint_insert),
        "Insert table_id=8\n"
        "├── columns=<schema-order>\n"
        "└── row[0]=[BIGINT:1,BIGINT:2147483648]\n");

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
        "Delete table_id=7 input=[c0->s0,c1->s1,c2->s2]\n"
        "└── predicate=<none>\n");

    test.begin_case("Delete with predicate");
    const Plan delete_one = plan_sql(
        "DELETE FROM student WHERE id = 1;",
        catalog,
        false);
    test.expect_equal(
        format_plan(delete_one),
        "Delete table_id=7 input=[c0->s0,c1->s1,c2->s2]\n"
        "└── predicate: =\n"
        "    ├── ColumnRef[s0]\n"
        "    └── Literal(INT:1)\n");

    test.begin_case("Update multiple assignments with predicate");
    const Plan update = plan_sql(
        "UPDATE student SET name='Alice',age=21 WHERE id=1;",
        catalog,
        false);
    test.expect_equal(
        format_plan(update),
        "Update table_id=7 input=[c0->s0,c1->s1,c2->s2]\n"
        "├── assignment[c1]=VARCHAR:\"Alice\"\n"
        "├── assignment[c2]=INT:21\n"
        "└── predicate: =\n"
        "    ├── ColumnRef[s0]\n"
        "    └── Literal(INT:1)\n");

    test.begin_case("Update NULL without predicate");
    const Plan update_null = plan_sql(
        "UPDATE nullable_values SET note=NULL;",
        catalog,
        false);
    test.expect_equal(
        format_plan(update_null),
        "Update table_id=9 input=[c0->s0,c1->s1]\n"
        "└── assignment[c1]=NULL\n");

    test.begin_case("Unary expression");
    const Plan unary = plan_sql(
        "SELECT id FROM student WHERE NOT id = 1;",
        catalog,
        false);
    test.expect_equal(
        format_plan(unary),
        "Project outputs=[s0]\n"
        "└── Filter\n"
        "    ├── predicate: NOT\n"
        "    │   └── =\n"
        "    │       ├── ColumnRef[s0]\n"
        "    │       └── Literal(INT:1)\n"
        "    └── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n");

    test.begin_case("NULL literal and IS NULL");
    const Plan null_test = plan_sql(
        "SELECT name FROM student WHERE NULL OR name IS NULL;",
        catalog,
        false);
    test.expect_equal(
        format_plan(null_test),
        "Project outputs=[s1]\n"
        "└── Filter\n"
        "    ├── predicate: OR\n"
        "    │   ├── Literal(NULL)\n"
        "    │   └── IS NULL\n"
        "    │       └── ColumnRef[s1]\n"
        "    └── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n");

    test.begin_case("IS NOT NULL");
    const Plan not_null_test = plan_sql(
        "DELETE FROM student WHERE name IS NOT NULL;",
        catalog,
        false);
    test.expect_equal(
        format_plan(not_null_test),
        "Delete table_id=7 input=[c0->s0,c1->s1,c2->s2]\n"
        "└── predicate: IS NOT NULL\n"
        "    └── ColumnRef[s1]\n");
}

void test_before_after(TestContext& test, CatalogView catalog) {
    test.begin_case("AND simplification before and after");
    const std::string_view and_sql =
        "SELECT name FROM student WHERE 1 = 1 AND age > 18;";
    const Plan and_before = plan_sql(and_sql, catalog, false);
    const Plan and_after = plan_sql(and_sql, catalog, true);
    test.expect_equal(
        format_plan(and_before),
        "Project outputs=[s1]\n"
        "└── Filter\n"
        "    ├── predicate: AND\n"
        "    │   ├── =\n"
        "    │   │   ├── Literal(INT:1)\n"
        "    │   │   └── Literal(INT:1)\n"
        "    │   └── >\n"
        "    │       ├── ColumnRef[s2]\n"
        "    │       └── Literal(INT:18)\n"
        "    └── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n");
    test.expect_equal(
        format_plan(and_after),
        "Project outputs=[s1]\n"
        "└── Filter\n"
        "    ├── predicate: >\n"
        "    │   ├── ColumnRef[s2]\n"
        "    │   └── Literal(INT:18)\n"
        "    └── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n");

    test.begin_case("TRUE Filter elimination before and after");
    const std::string_view true_sql = "SELECT name FROM student WHERE 1 = 1;";
    const Plan true_before = plan_sql(true_sql, catalog, false);
    const Plan true_after = plan_sql(true_sql, catalog, true);
    test.expect_equal(
        format_plan(true_before),
        "Project outputs=[s1]\n"
        "└── Filter\n"
        "    ├── predicate: =\n"
        "    │   ├── Literal(INT:1)\n"
        "    │   └── Literal(INT:1)\n"
        "    └── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n");
    test.expect_equal(
        format_plan(true_after),
        "Project outputs=[s1]\n"
        "└── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n");

    test.begin_case("Double-NOT before and after");
    const std::string_view not_sql =
        "SELECT name FROM student WHERE NOT NOT age > 18;";
    const Plan not_before = plan_sql(not_sql, catalog, false);
    const Plan not_after = plan_sql(not_sql, catalog, true);
    test.expect_equal(
        format_plan(not_before),
        "Project outputs=[s1]\n"
        "└── Filter\n"
        "    ├── predicate: NOT\n"
        "    │   └── NOT\n"
        "    │       └── >\n"
        "    │           ├── ColumnRef[s2]\n"
        "    │           └── Literal(INT:18)\n"
        "    └── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n");
    test.expect_equal(
        format_plan(not_after),
        "Project outputs=[s1]\n"
        "└── Filter\n"
        "    ├── predicate: >\n"
        "    │   ├── ColumnRef[s2]\n"
        "    │   └── Literal(INT:18)\n"
        "    └── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n");

    test.begin_case("complex defense example before and after");
    const std::string_view complex_sql =
        "SELECT name,id FROM student WHERE "
        "1 = 1 AND NOT NOT age >= 18 AND name != 'Tom';";
    const Plan complex_before = plan_sql(complex_sql, catalog, false);
    const Plan complex_after = plan_sql(complex_sql, catalog, true);
    test.expect_equal(
        format_plan(complex_before),
        "Project outputs=[s1,s0]\n"
        "└── Filter\n"
        "    ├── predicate: AND\n"
        "    │   ├── AND\n"
        "    │   │   ├── =\n"
        "    │   │   │   ├── Literal(INT:1)\n"
        "    │   │   │   └── Literal(INT:1)\n"
        "    │   │   └── NOT\n"
        "    │   │       └── NOT\n"
        "    │   │           └── >=\n"
        "    │   │               ├── ColumnRef[s2]\n"
        "    │   │               └── Literal(INT:18)\n"
        "    │   └── !=\n"
        "    │       ├── ColumnRef[s1]\n"
        "    │       └── Literal(VARCHAR:\"Tom\")\n"
        "    └── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n");
    test.expect_equal(
        format_plan(complex_after),
        "Project outputs=[s1,s0]\n"
        "└── Filter\n"
        "    ├── predicate: AND\n"
        "    │   ├── >=\n"
        "    │   │   ├── ColumnRef[s2]\n"
        "    │   │   └── Literal(INT:18)\n"
        "    │   └── !=\n"
        "    │       ├── ColumnRef[s1]\n"
        "    │       └── Literal(VARCHAR:\"Tom\")\n"
        "    └── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n");
}

void test_false_and_delete_optimization(TestContext& test, CatalogView catalog) {
    test.begin_case("FALSE Filter preservation");
    const std::string_view false_sql = "SELECT id FROM student WHERE 1 = 2;";
    const Plan false_before = plan_sql(false_sql, catalog, false);
    const Plan false_after = plan_sql(false_sql, catalog, true);
    const std::string expected =
        "Project outputs=[s0]\n"
        "└── Filter\n"
        "    ├── predicate: =\n"
        "    │   ├── Literal(INT:1)\n"
        "    │   └── Literal(INT:2)\n"
        "    └── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n";
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
        "Delete table_id=7 input=[c0->s0,c1->s1,c2->s2]\n"
        "└── predicate: =\n"
        "    ├── Literal(INT:1)\n"
        "    └── Literal(INT:1)\n");
    test.expect_equal(
        format_plan(delete_true_after),
        "Delete table_id=7 input=[c0->s0,c1->s1,c2->s2]\n"
        "└── predicate=<none>\n");
    test.expect_equal(
        format_plan(delete_false_after),
        "Delete table_id=7 input=[c0->s0,c1->s1,c2->s2]\n"
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

void test_join_plan(TestContext& test, CatalogView catalog) {
    const Plan join = plan_sql(
        "SELECT student.name,other.score FROM student JOIN other "
        "ON student.id=other.student_id WHERE other.score>0 ORDER BY other.score DESC;",
        catalog,
        false);
    test.expect_equal(
        format_plan(join),
        "Project outputs=[s1,s5]\n"
        "└── Sort keys=[s5 DESC]\n"
        "    └── Filter\n"
        "        ├── predicate: >\n"
        "        │   ├── ColumnRef[s5]\n"
        "        │   └── Literal(INT:0)\n"
        "        └── InnerJoin\n"
        "            ├── condition: =\n"
        "            │   ├── ColumnRef[s0]\n"
        "            │   └── ColumnRef[s4]\n"
        "            ├── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n"
        "            └── SeqScan table_id=10 columns=[c0->s3,c1->s4,c2->s5]\n");
}

void test_aggregate_plans(TestContext& test, CatalogView catalog) {
    const Plan global = plan_sql(
        "SELECT COUNT(*),SUM(age),AVG(age),MIN(name),MAX(age) FROM student;",
        catalog,
        false);
    test.expect_equal(
        format_plan(global),
        "Project outputs=[s3,s4,s5,s6,s7]\n"
        "└── Aggregate group_by=[]\n"
        "    ├── aggregate[0]: COUNT(*) -> s3 type=BIGINT nullable=false\n"
        "    ├── aggregate[1]: SUM(s2) -> s4 type=BIGINT nullable=true\n"
        "    ├── aggregate[2]: AVG(s2) -> s5 type=DOUBLE nullable=true\n"
        "    ├── aggregate[3]: MIN(s1) -> s6 type=VARCHAR nullable=true\n"
        "    ├── aggregate[4]: MAX(s2) -> s7 type=INT nullable=true\n"
        "    └── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n");

    const Plan grouped = plan_sql(
        "SELECT name,COUNT(*),SUM(other.score) FROM student JOIN other "
        "ON student.id=other.student_id WHERE other.score>0 "
        "GROUP BY student.name ORDER BY student.name;",
        catalog,
        false);
    test.expect_equal(
        format_plan(grouped),
        "Project outputs=[s1,s6,s7]\n"
        "└── Sort keys=[s1 ASC]\n"
        "    └── Aggregate group_by=[s1]\n"
        "        ├── aggregate[0]: COUNT(*) -> s6 type=BIGINT nullable=false\n"
        "        ├── aggregate[1]: SUM(s5) -> s7 type=BIGINT nullable=true\n"
        "        └── Filter\n"
        "            ├── predicate: >\n"
        "            │   ├── ColumnRef[s5]\n"
        "            │   └── Literal(INT:0)\n"
        "            └── InnerJoin\n"
        "                ├── condition: =\n"
        "                │   ├── ColumnRef[s0]\n"
        "                │   └── ColumnRef[s4]\n"
        "                ├── SeqScan table_id=7 columns=[c0->s0,c1->s1,c2->s2]\n"
        "                └── SeqScan table_id=10 columns=[c0->s3,c1->s4,c2->s5]\n");
}

}  // namespace

int main() {
    TestContext test;
    const std::vector<TableMeta> tables{
        TableMeta{7U, "student", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"name", Type::kVarchar},
            ColumnMeta{"age", Type::kInt},
        }},
        TableMeta{8U, "flags", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"active", Type::kBoolean},
        }},
        TableMeta{9U, "nullable_values", {
            ColumnMeta{"id", Type::kInt, false},
            ColumnMeta{"note", Type::kVarchar, true},
        }},
        TableMeta{10U, "other", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"student_id", Type::kInt},
            ColumnMeta{"score", Type::kInt},
        }}
    };
    const CatalogView catalog{std::span<const TableMeta>{tables}};

    test_basic_plans(test, catalog);
    test_join_plan(test, catalog);
    test_aggregate_plans(test, catalog);
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
