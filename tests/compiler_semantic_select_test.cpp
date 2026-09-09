#include <iostream>
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

void expect_error(
    TestContext& test,
    std::string_view name,
    std::string_view sql,
    CatalogView catalog,
    SourceLocation location,
    std::string_view message_part) {
    const SemanticResult result = analyze_sql(test, name, sql, catalog);
    const auto* error = std::get_if<CompileError>(&result.outcome);
    const std::string prefix{name};
    test.expect(error != nullptr, prefix + ": semantic error");
    if (error == nullptr) {
        return;
    }
    test.expect(error->kind == CompileErrorKind::kSemantic, prefix + ": error kind");
    test.expect(error->location.line == location.line, prefix + ": line");
    test.expect(error->location.column == location.column, prefix + ": column");
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
        TableMeta{8U, "other", {ColumnMeta{"score", Type::kInt}}},
    };
    const CatalogView catalog{tables};

    {
        const SemanticResult result = analyze_sql(test, "star", "SELECT * FROM student;", catalog);
        const auto* select = expect_select(test, "star", result);
        if (select != nullptr) {
            test.expect(select->table_id == 7U, "star: TableId");
            test.expect(select->outputs == std::vector<ColumnId>{0U, 1U, 2U}, "star: schema order");
            test.expect(select->predicate == nullptr, "star: no predicate");
        }
    }
    {
        const SemanticResult result = analyze_sql(test, "projection order", "SELECT name,id FROM student;", catalog);
        const auto* select = expect_select(test, "projection order", result);
        if (select != nullptr) {
            test.expect(select->outputs == std::vector<ColumnId>{1U, 0U}, "projection order: ids");
        }
    }
    {
        const SemanticResult result = analyze_sql(test, "repeated projection", "SELECT id,id FROM student;", catalog);
        const auto* select = expect_select(test, "repeated projection", result);
        if (select != nullptr) {
            test.expect(select->outputs == std::vector<ColumnId>{0U, 0U}, "repeated projection: retained");
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

    expect_error(test, "unknown table", "SELECT * FROM unknown;", catalog, {1, 15}, "table 'unknown' does not exist");
    expect_error(test, "unknown projection", "SELECT score FROM student;", catalog, {1, 8}, "column 'score' does not exist");
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

    if (test.failures() != 0) {
        std::cerr << test.failures() << " SELECT semantic test assertion(s) failed\n";
        return 1;
    }
    return 0;
}
