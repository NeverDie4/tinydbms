#include <algorithm>
#include <iostream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "bound_ast.hpp"
#include "lexer.hpp"
#include "parser.hpp"
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

struct ExpectedLocation {
    int line;
    int column;
};

SemanticResult analyze_sql(TestContext& test, std::string_view name, std::string_view sql, CatalogView catalog) {
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

const BoundSelect* expect_select(
    TestContext& test,
    std::string_view name,
    const SemanticResult& result) {
    const auto* statement = std::get_if<BoundStatement>(&result.outcome);
    test.expect(statement != nullptr, std::string{name} + ": semantic success");
    if (statement == nullptr) {
        return nullptr;
    }
    const auto* select = std::get_if<BoundSelect>(&statement->kind);
    test.expect(select != nullptr, std::string{name} + ": bound SELECT");
    return select;
}

bool references_equal(
    const std::vector<BoundColumnRef>& actual,
    std::initializer_list<BoundColumnRef> expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    return std::equal(
        actual.begin(), actual.end(), expected.begin(),
        [](const BoundColumnRef& lhs, const BoundColumnRef& rhs) {
            return lhs.table_id == rhs.table_id && lhs.column_id == rhs.column_id;
        });
}

std::vector<BoundColumnRef> selected_columns(const BoundSelect& select) {
    std::vector<BoundColumnRef> columns;
    columns.reserve(select.items.size());
    for (const BoundSelectItem& item : select.items) {
        if (const auto* column = std::get_if<BoundColumnRef>(&item)) {
            columns.push_back(*column);
        }
    }
    return columns;
}

void expect_error(
    TestContext& test,
    std::string_view name,
    std::string_view sql,
    CatalogView catalog,
    ExpectedLocation location,
    std::string_view message_part) {
    const SemanticResult result = analyze_sql(test, name, sql, catalog);
    const auto* error = std::get_if<CompileError>(&result.outcome);
    const std::string prefix{name};
    test.expect(error != nullptr, prefix + ": semantic error");
    if (error == nullptr) {
        return;
    }
    test.expect(error->stage == CompileStage::kSemantic, prefix + ": error stage");
    test.expect(error->source.begin.line == location.line, prefix + ": line");
    test.expect(error->source.begin.column == location.column, prefix + ": column");
    test.expect(error->message.find(message_part) != std::string::npos, prefix + ": message");
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
        TableMeta{8U, "other", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"score", Type::kInt},
        }},
        TableMeta{9U, "staff", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"department", Type::kVarchar, true},
            ColumnMeta{"pay", Type::kBigInt, true},
            ColumnMeta{"ratio", Type::kDouble, true},
            ColumnMeta{"active", Type::kBoolean, true},
        }},
    };
    const CatalogView catalog{tables};

    {
        const SemanticResult result = analyze_sql(test, "star", "SELECT * FROM student;", catalog);
        const auto* select = expect_select(test, "star", result);
        if (select != nullptr) {
            test.expect(select->table_id == 7U, "star: TableId");
            test.expect(references_equal(
                            selected_columns(*select),
                            {{7U, 0U}, {7U, 1U}, {7U, 2U}}),
                        "star: schema order");
            test.expect(select->predicate == nullptr, "star: no predicate");
        }
    }
    {
        const SemanticResult result = analyze_sql(test, "projection order", "SELECT name,id FROM student;", catalog);
        const auto* select = expect_select(test, "projection order", result);
        if (select != nullptr) {
            test.expect(references_equal(selected_columns(*select), {{7U, 1U}, {7U, 0U}}),
                        "projection order: ids");
        }
    }
    {
        const SemanticResult result = analyze_sql(test, "repeated projection", "SELECT id,id FROM student;", catalog);
        const auto* select = expect_select(test, "repeated projection", result);
        if (select != nullptr) {
            test.expect(references_equal(selected_columns(*select), {{7U, 0U}, {7U, 0U}}),
                        "repeated projection: retained");
        }
    }
    {
        const SemanticResult result = analyze_sql(
            test,
            "order by non-output",
            "SELECT name FROM student ORDER BY age DESC,id;",
            catalog);
        const auto* select = expect_select(test, "order by non-output", result);
        if (select != nullptr && select->order_by.size() == 2U) {
            test.expect(references_equal(selected_columns(*select), {{7U, 1U}}),
                        "order by non-output: projection unchanged");
            test.expect(select->order_by[0].column.table_id == 7U &&
                            select->order_by[0].column.column_id == 2U &&
                            select->order_by[0].direction == SortDirection::kDesc,
                        "order by non-output: age c2 DESC");
            test.expect(select->order_by[1].column.table_id == 7U &&
                            select->order_by[1].column.column_id == 0U &&
                            select->order_by[1].direction == SortDirection::kAsc,
                        "order by non-output: id c0 default ASC");
        } else {
            test.expect(false, "order by non-output: two bound keys");
        }
    }

    const std::vector<std::string_view> valid_queries{
        "SELECT id FROM student WHERE age >= 18;",
        "SELECT name,id FROM student WHERE age >= 18 AND name != 'Tom';",
        "SELECT * FROM student WHERE id = 1 OR age > 20;",
    };
    for (const std::string_view sql : valid_queries) {
        const SemanticResult result = analyze_sql(test, sql, sql, catalog);
        const auto* select = expect_select(test, sql, result);
        if (select != nullptr) {
            test.expect(select->predicate != nullptr, std::string{sql} + ": predicate");
        }
    }

    {
        const SemanticResult result = analyze_sql(
            test,
            "qualified join",
            "SELECT student.name,other.score FROM student JOIN other "
            "ON student.id = other.id ORDER BY other.score;",
            catalog);
        const auto* select = expect_select(test, "qualified join", result);
        if (select != nullptr) {
            test.expect(select->joins.size() == 1U, "qualified join: one bound join");
            test.expect(references_equal(selected_columns(*select), {{7U, 1U}, {8U, 1U}}),
                        "qualified join: relation-aware outputs");
            test.expect(select->order_by.size() == 1U &&
                            select->order_by[0].column.table_id == 8U &&
                            select->order_by[0].column.column_id == 1U,
                        "qualified join: right ORDER BY binding");
        }
    }
    {
        const SemanticResult result = analyze_sql(
            test,
            "join star",
            "SELECT * FROM student INNER JOIN other ON student.id = other.id;",
            catalog);
        const auto* select = expect_select(test, "join star", result);
        if (select != nullptr) {
            test.expect(references_equal(
                            selected_columns(*select),
                            {{7U, 0U}, {7U, 1U}, {7U, 2U}, {8U, 0U}, {8U, 1U}}),
                        "join star: left schema then right schema");
        }
    }

    {
        const SemanticResult result = analyze_sql(
            test,
            "grouped aggregate",
            "SELECT department,COUNT(*),COUNT(pay),SUM(id),SUM(pay),AVG(pay),"
            "MIN(department),MAX(ratio) FROM staff GROUP BY department ORDER BY department;",
            catalog);
        const auto* select = expect_select(test, "grouped aggregate", result);
        if (select != nullptr) {
            test.expect(references_equal(select->group_by, {{9U, 1U}}),
                        "grouped aggregate: group binding");
            test.expect(select->items.size() == 8U, "grouped aggregate: item count");
            const auto* count_star = std::get_if<BoundAggregateCall>(&select->items[1]);
            const auto* count_column = std::get_if<BoundAggregateCall>(&select->items[2]);
            const auto* sum_int = std::get_if<BoundAggregateCall>(&select->items[3]);
            const auto* sum_bigint = std::get_if<BoundAggregateCall>(&select->items[4]);
            const auto* average = std::get_if<BoundAggregateCall>(&select->items[5]);
            const auto* minimum = std::get_if<BoundAggregateCall>(&select->items[6]);
            test.expect(count_star != nullptr && !count_star->argument.has_value() &&
                            count_star->output_type == Type::kBigInt && !count_star->nullable,
                        "grouped aggregate: COUNT star contract");
            test.expect(count_column != nullptr && count_column->argument.has_value() &&
                            count_column->output_type == Type::kBigInt && !count_column->nullable,
                        "grouped aggregate: COUNT column contract");
            test.expect(sum_int != nullptr && sum_int->output_type == Type::kBigInt &&
                            sum_int->nullable,
                        "grouped aggregate: SUM INT contract");
            test.expect(sum_bigint != nullptr && sum_bigint->output_type == Type::kBigInt,
                        "grouped aggregate: SUM BIGINT contract");
            test.expect(average != nullptr && average->output_type == Type::kDouble,
                        "grouped aggregate: AVG contract");
            test.expect(minimum != nullptr && minimum->output_type == Type::kVarchar,
                        "grouped aggregate: MIN VARCHAR contract");
        }
    }
    {
        const SemanticResult result = analyze_sql(
            test,
            "hidden group key",
            "SELECT COUNT(*) FROM staff GROUP BY department ORDER BY department;",
            catalog);
        const auto* select = expect_select(test, "hidden group key", result);
        test.expect(select != nullptr && select->group_by.size() == 1U &&
                        select->order_by.size() == 1U,
                    "hidden group key: retained for ORDER BY");
    }
    expect_select(
        test,
        "group only",
        analyze_sql(test, "group only", "SELECT department FROM staff GROUP BY department;", catalog));

    expect_error(test, "unknown table", "SELECT * FROM unknown;", catalog, {1, 15}, "table 'unknown' does not exist");
    expect_error(test, "ambiguous projection",
                 "SELECT id FROM student JOIN other ON student.id = other.id;",
                 catalog, {1, 8}, "column 'id' is ambiguous");
    expect_error(test, "unknown qualifier",
                 "SELECT missing.id FROM student JOIN other ON student.id = other.id;",
                 catalog, {1, 8}, "table qualifier 'missing' is not present in query");
    expect_error(test, "unknown qualified column",
                 "SELECT other.name FROM student JOIN other ON student.id = other.id;",
                 catalog, {1, 8}, "column 'name' does not exist in table 'other'");
    expect_error(test, "self join",
                 "SELECT student.id FROM student JOIN student ON student.id = student.id;",
                 catalog, {1, 37}, "aliases are not supported");
    expect_error(test, "nonboolean ON",
                 "SELECT student.name FROM student JOIN other ON other.score;",
                 catalog, {1, 48}, "JOIN ON predicate must be BOOL");
    expect_error(test, "unknown projection", "SELECT score FROM student;", catalog, {1, 8}, "column 'score' does not exist");
    expect_error(
        test,
        "unknown order column",
        "SELECT name FROM student ORDER BY missing;",
        catalog,
        {1, 35},
        "column 'missing' does not exist");
    expect_error(
        test, "middle unknown projection", "SELECT id,score,name FROM student;", catalog, {1, 11}, "column 'score' does not exist");
    expect_error(
        test,
        "unknown predicate column",
        "SELECT id FROM student WHERE score > 60;",
        catalog,
        {1, 30},
        "column 'score' does not exist");
    expect_error(
        test,
        "projection error first",
        "SELECT score FROM student WHERE unknown = 1;",
        catalog,
        {1, 8},
        "column 'score' does not exist");
    expect_error(
        test, "predicate mixed types", "SELECT id FROM student WHERE age = 'abc';", catalog, {1, 34}, "INT and VARCHAR");
    expect_error(
        test, "predicate VARCHAR ordering", "SELECT id FROM student WHERE name > 'Alice';", catalog, {1, 35}, "VARCHAR");
    expect_error(test, "INT predicate", "SELECT id FROM student WHERE age;", catalog, {1, 30}, "WHERE predicate must be BOOL");
    expect_error(test, "literal predicate", "SELECT id FROM student WHERE 1;", catalog, {1, 30}, "WHERE predicate must be BOOL");
    expect_error(test, "invalid logic", "SELECT id FROM student WHERE 1 AND 2;", catalog, {1, 32}, "AND");
    expect_error(test, "global plain column",
                 "SELECT department,COUNT(*) FROM staff;", catalog, {1, 8},
                 "neither grouped nor aggregated");
    expect_error(test, "nongrouped selected column",
                 "SELECT department,pay,COUNT(*) FROM staff GROUP BY department;", catalog,
                 {1, 19}, "neither grouped nor aggregated");
    expect_error(test, "nongrouped order column",
                 "SELECT department,COUNT(*) FROM staff GROUP BY department ORDER BY pay;", catalog,
                 {1, 68}, "neither grouped nor aggregated");
    expect_error(test, "aggregate star query",
                 "SELECT * FROM staff GROUP BY department;", catalog, {1, 15},
                 "SELECT * is not allowed");
    expect_error(test, "SUM VARCHAR", "SELECT SUM(department) FROM staff;", catalog,
                 {1, 8}, "SUM requires a numeric column");
    expect_error(test, "AVG BOOLEAN", "SELECT AVG(active) FROM staff;", catalog,
                 {1, 8}, "AVG requires a numeric column");
    expect_error(test, "MIN BOOLEAN", "SELECT MIN(active) FROM staff;", catalog,
                 {1, 8}, "MIN does not support BOOLEAN");
    expect_error(test, "ambiguous group key",
                 "SELECT COUNT(*) FROM student JOIN other ON student.id=other.id GROUP BY id;",
                 catalog, {1, 73}, "column 'id' is ambiguous");

    if (test.failures() != 0) {
        std::cerr << test.failures() << " SELECT semantic test assertion(s) failed\n";
        return 1;
    }
    return 0;
}
