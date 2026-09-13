#include <iostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ast.hpp"
#include "lexer.hpp"
#include "parser.hpp"

namespace {

using tinydbms::CompileStage;
using tinydbms::compiler::CompileError;
using namespace tinydbms::compiler::internal;

struct ExpectedLocation {
    int line;
    int column;
};

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

ParseResult parse_sql(TestContext& test, std::string_view name, std::string_view sql) {
    const LexResult lexed = tokenize(sql);
    const auto* tokens = std::get_if<std::vector<Token>>(&lexed.outcome);
    test.expect(tokens != nullptr, std::string{name} + ": lexer success");
    if (tokens == nullptr) {
        return ParseResult{std::get<CompileError>(lexed.outcome)};
    }
    return parse(*tokens);
}

const SelectAst* expect_select(TestContext& test, std::string_view name, const ParseResult& result) {
    const auto* statement = std::get_if<StatementAst>(&result.outcome);
    test.expect(statement != nullptr, std::string{name} + ": parse success");
    if (statement == nullptr) {
        return nullptr;
    }
    const auto* select = std::get_if<SelectAst>(&statement->kind);
    test.expect(select != nullptr, std::string{name} + ": select AST");
    return select;
}

const AstSelectColumn* column_item(const SelectAst& select, std::size_t index) {
    return index < select.items.size()
        ? std::get_if<AstSelectColumn>(&select.items[index])
        : nullptr;
}

const AstAggregateCall* aggregate_item(const SelectAst& select, std::size_t index) {
    return index < select.items.size()
        ? std::get_if<AstAggregateCall>(&select.items[index])
        : nullptr;
}

void expect_error(
    TestContext& test,
    std::string_view name,
    std::string_view sql,
    ExpectedLocation expected,
    std::string_view message_part) {
    const auto result = parse_sql(test, name, sql);
    const auto* error = std::get_if<CompileError>(&result.outcome);
    const std::string prefix{name};
    test.expect(error != nullptr, prefix + ": syntax error");
    if (error != nullptr) {
        test.expect(error->stage == CompileStage::kSyntax, prefix + ": stage");
        test.expect(error->source.begin.line == expected.line, prefix + ": line");
        test.expect(error->source.begin.column == expected.column, prefix + ": column");
        test.expect(error->message.find(message_part) != std::string::npos, prefix + ": message");
    }
}

}  // namespace


int main() {
    TestContext test;

    {
        const auto result = parse_sql(test, "star", "SELECT * FROM student;");
        const auto* select = expect_select(test, "star", result);
        if (select != nullptr) {
            test.expect(select->select_all, "star: select all");
            test.expect(select->items.empty(), "star: no explicit items");
            test.expect(select->table_name == "student", "star: table");
            test.expect(select->predicate == nullptr, "star: no predicate");
        }
    }

    {
        const auto result = parse_sql(test, "one column", "SELECT id FROM student;");
        const auto* select = expect_select(test, "one column", result);
        if (select != nullptr) {
            test.expect(!select->select_all, "one column: not all");
            const auto* column = column_item(*select, 0);
            test.expect(select->items.size() == 1 && column != nullptr && column->name == "id", "one column: projection");
        }
    }

    {
        const auto result = parse_sql(test, "three columns", "SELECT id,name,age FROM student;");
        const auto* select = expect_select(test, "three columns", result);
        if (select != nullptr && select->items.size() == 3) {
            const auto* id = column_item(*select, 0);
            const auto* name = column_item(*select, 1);
            const auto* age = column_item(*select, 2);
            test.expect(id != nullptr && id->name == "id", "three columns: id");
            test.expect(name != nullptr && name->name == "name", "three columns: name");
            test.expect(age != nullptr && age->name == "age", "three columns: age");
            test.expect(id != nullptr && id->source.begin.column == 8, "three columns: id location");
            test.expect(name != nullptr && name->source.begin.column == 11, "three columns: name location");
            test.expect(select->table_source.begin.column == 25, "three columns: table location");
        }
    }

    {
        const auto result = parse_sql(test, "case", "SeLeCt ID,Name FrOm Student;");
        const auto* select = expect_select(test, "case", result);
        if (select != nullptr && select->items.size() == 2) {
            const auto* id = column_item(*select, 0);
            const auto* name = column_item(*select, 1);
            test.expect(id != nullptr && id->name == "id", "case: id");
            test.expect(name != nullptr && name->name == "name", "case: name");
            test.expect(select->table_name == "student", "case: table");
        }
    }

    {
        const auto result = parse_sql(test, "where", "SELECT id FROM student WHERE age >= 18;");
        const auto* select = expect_select(test, "where", result);
        test.expect(select != nullptr && select->predicate != nullptr, "where: predicate");
    }

    {
        const auto result = parse_sql(
            test,
            "qualified join",
            "SELECT student.name,other.score FROM student INNER JOIN other "
            "ON student.id = other.id WHERE other.score > 0 ORDER BY other.score DESC;");
        const auto* select = expect_select(test, "qualified join", result);
        if (select != nullptr) {
            const auto* first = column_item(*select, 0);
            const auto* second = column_item(*select, 1);
            test.expect(select->items.size() == 2U && first != nullptr && second != nullptr &&
                            first->qualifier == "student" && second->qualifier == "other",
                        "qualified join: qualified SELECT list");
            test.expect(select->joins.size() == 1U &&
                            select->joins[0].table_name == "other" &&
                            select->joins[0].condition != nullptr,
                        "qualified join: INNER JOIN and ON");
            test.expect(select->predicate != nullptr, "qualified join: WHERE");
            test.expect(select->order_by.size() == 1U &&
                            select->order_by[0].qualifier == "other" &&
                            select->order_by[0].column_name == "score",
                        "qualified join: qualified ORDER BY");
        }
    }

    {
        const auto result = parse_sql(
            test,
            "aggregate and group",
            "SELECT department,COUNT(*),SUM(pay),AVG(pay),MIN(name),MAX(t.pay) "
            "FROM staff JOIN totals ON staff.id=totals.id WHERE active "
            "GROUP BY department,staff.active ORDER BY department;");
        const auto* select = expect_select(test, "aggregate and group", result);
        if (select != nullptr) {
            test.expect(select->items.size() == 6U, "aggregate and group: item count");
            const auto* count = aggregate_item(*select, 1);
            const auto* sum = aggregate_item(*select, 2);
            const auto* max = aggregate_item(*select, 5);
            test.expect(count != nullptr && count->kind == AstAggregateKind::kCount &&
                            !count->argument.has_value(),
                        "aggregate and group: COUNT star");
            test.expect(sum != nullptr && sum->kind == AstAggregateKind::kSum &&
                            sum->argument.has_value() && sum->argument->name == "pay",
                        "aggregate and group: SUM column");
            test.expect(max != nullptr && max->argument.has_value() &&
                            max->argument->qualifier == "t",
                        "aggregate and group: qualified argument");
            test.expect(select->group_by.size() == 2U &&
                            select->group_by[1].qualifier == "staff",
                        "aggregate and group: group keys");
        }
    }
    {
        const auto result = parse_sql(
            test,
            "chained joins",
            "SELECT a.id FROM a JOIN b ON a.id=b.id JOIN c ON b.id=c.id;");
        const auto* select = expect_select(test, "chained joins", result);
        test.expect(select != nullptr && select->joins.size() == 2U,
                    "chained joins: frozen left-deep contract accepted");
    }

    {
        const auto result = parse_sql(
            test,
            "order by",
            "SELECT name FROM student WHERE age >= 18 ORDER BY age DESC,id;");
        const auto* select = expect_select(test, "order by", result);
        if (select != nullptr && select->order_by.size() == 2) {
            test.expect(select->predicate != nullptr, "order by: predicate preserved");
            test.expect(select->order_by[0].column_name == "age", "order by: first key");
            test.expect(
                select->order_by[0].direction == AstSortDirection::kDesc,
                "order by: explicit DESC");
            test.expect(select->order_by[1].column_name == "id", "order by: second key");
            test.expect(
                select->order_by[1].direction == AstSortDirection::kAsc,
                "order by: default ASC");
        } else {
            test.expect(false, "order by: two keys");
        }
    }

    expect_select(
        test,
        "complex where",
        parse_sql(test, "complex where", "SELECT id,name\nFROM student\nWHERE age >= 18 AND name != 'Tom';"));
    expect_select(test, "star where", parse_sql(test, "star where", "SELECT * FROM student WHERE id = 1;"));
    expect_select(
        test,
        "comments",
        parse_sql(test, "comments", "SELECT /*a*/ id, -- first\nname FROM student /*b*/ WHERE id = 1;"));

    expect_select(test, "unknown table", parse_sql(test, "unknown table", "SELECT unknown FROM no_table;"));
    expect_select(test, "duplicate projection", parse_sql(test, "duplicate projection", "SELECT id,id FROM student;"));
    expect_select(test, "type mismatch", parse_sql(test, "type mismatch", "SELECT name FROM student WHERE age = 'abc';"));
    expect_select(test, "unknown predicate column", parse_sql(test, "unknown predicate column", "SELECT id FROM student WHERE unknown = 1;"));

    expect_error(test, "select only", "SELECT;", {1, 7}, "column");
    expect_error(test, "missing select list", "SELECT FROM t;", {1, 8}, "column");
    expect_error(test, "star missing from", "SELECT *;", {1, 9}, "FROM");
    expect_error(test, "missing from keyword", "SELECT * t;", {1, 10}, "FROM");
    expect_error(test, "missing projection comma", "SELECT id name FROM t;", {1, 11}, "FROM");
    expect_error(test, "trailing projection comma", "SELECT id, FROM t;", {1, 12}, "identifier");
    expect_error(test, "leading projection comma", "SELECT ,id FROM t;", {1, 8}, "column");
    expect_error(test, "star then column", "SELECT *,id FROM t;", {1, 9}, "FROM");
    expect_error(test, "column then star", "SELECT id,* FROM t;", {1, 11}, "identifier");
    expect_error(test, "missing table", "SELECT id FROM;", {1, 15}, "identifier");
    expect_error(test, "empty where", "SELECT id FROM t WHERE;", {1, 23}, "expression");
    expect_error(test, "where empty parens", "SELECT id FROM t WHERE ();", {1, 25}, "expression");
    expect_error(test, "comparison missing rhs", "SELECT id FROM t WHERE a = ;", {1, 28}, "expression");
    expect_error(test, "comparison missing lhs", "SELECT id FROM t WHERE = 1;", {1, 24}, "expression");
    expect_error(test, "missing close paren", "SELECT id FROM t WHERE (a = 1;", {1, 30}, "')'");
    expect_error(test, "extra close paren", "SELECT id FROM t WHERE a = 1);", {1, 29}, "';'");
    expect_error(test, "missing semicolon", "SELECT id FROM t", {1, 17}, "';'");
    expect_error(test, "trailing token", "SELECT * FROM t; abc", {1, 18}, "end of statement");
    expect_error(test, "order missing by", "SELECT id FROM t ORDER id;", {1, 24}, "BY");
    expect_error(test, "order empty", "SELECT id FROM t ORDER BY;", {1, 26}, "column identifier");
    expect_error(test, "order trailing comma", "SELECT id FROM t ORDER BY id,;", {1, 30}, "column identifier");
    expect_error(test, "order literal", "SELECT id FROM t ORDER BY 1;", {1, 27}, "column identifier");
    expect_error(test, "order star", "SELECT id FROM t ORDER BY *;", {1, 27}, "column identifier");
    expect_error(test, "order double direction", "SELECT id FROM t ORDER BY id ASC DESC;", {1, 34}, "';'");
    expect_error(test, "order before where", "SELECT id FROM t ORDER BY id WHERE id=1;", {1, 30}, "';'");
    expect_error(test, "group missing by", "SELECT COUNT(*) FROM t GROUP id;", {1, 30}, "BY");
    expect_error(test, "group empty", "SELECT COUNT(*) FROM t GROUP BY;", {1, 32}, "column identifier");
    expect_error(test, "group trailing comma", "SELECT COUNT(*) FROM t GROUP BY id,;", {1, 36}, "column identifier");
    expect_error(test, "sum star", "SELECT SUM(*) FROM t;", {1, 13}, "only COUNT");
    expect_error(test, "count empty", "SELECT COUNT() FROM t;", {1, 14}, "aggregate column");
    expect_error(test, "count literal", "SELECT COUNT(1) FROM t;", {1, 14}, "aggregate column");
    expect_error(test, "aggregate multiple args", "SELECT SUM(a,b) FROM t;", {1, 13}, "')'");
    expect_error(test, "group expression", "SELECT COUNT(*) FROM t GROUP BY id=1;", {1, 35}, "';'");
    expect_error(test, "aggregate in where", "SELECT id FROM t WHERE COUNT(*)=1;", {1, 24}, "expression");
    expect_error(test, "join missing table", "SELECT a.id FROM a JOIN ON a.id=1;", {1, 25}, "table identifier");
    expect_error(test, "inner missing join", "SELECT a.id FROM a INNER b ON a.id=b.id;", {1, 26}, "JOIN after INNER");
    expect_error(test, "join missing on", "SELECT a.id FROM a JOIN b WHERE a.id=1;", {1, 27}, "ON after joined table");
    expect_error(test, "table star unsupported", "SELECT a.* FROM a;", {1, 10}, "column identifier after '.'");
    expect_error(test, "table alias unsupported", "SELECT a.id FROM a x;", {1, 20}, "';'");

    if (test.failures() != 0) {
        std::cerr << test.failures() << " select parser assertion(s) failed\n";
        return 1;
    }
    return 0;
}
