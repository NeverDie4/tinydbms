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

const BoundDelete* expect_delete(
    TestContext& test,
    std::string_view name,
    const SemanticResult& result) {
    const auto* statement = std::get_if<BoundStatement>(&result.outcome);
    test.expect(statement != nullptr, std::string{name} + ": semantic success");
    if (statement == nullptr) {
        return nullptr;
    }
    const auto* deletion = std::get_if<BoundDelete>(&statement->kind);
    test.expect(deletion != nullptr, std::string{name} + ": bound DELETE");
    return deletion;
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
        }}
    };
    const CatalogView catalog{tables};

    {
        const SemanticResult result = analyze_sql(test, "without WHERE", "DELETE FROM student;", catalog);
        const auto* deletion = expect_delete(test, "without WHERE", result);
        if (deletion != nullptr) {
            test.expect(deletion->table_id == 7U, "without WHERE: TableId");
            test.expect(deletion->predicate == nullptr, "without WHERE: no predicate");
        }
    }
    {
        const SemanticResult result = analyze_sql(test, "simple WHERE", "DELETE FROM student WHERE id = 1;", catalog);
        const auto* deletion = expect_delete(test, "simple WHERE", result);
        if (deletion != nullptr) {
            test.expect(deletion->predicate != nullptr, "simple WHERE: predicate");
        }
    }
    {
        const SemanticResult result = analyze_sql(
            test,
            "complex WHERE",
            "DELETE FROM student WHERE age < 18 OR name = 'Tom';",
            catalog);
        const auto* deletion = expect_delete(test, "complex WHERE", result);
        if (deletion != nullptr) {
            const auto* root = deletion->predicate == nullptr
                ? nullptr
                : std::get_if<BoundBinaryExpr>(&deletion->predicate->kind);
            const auto* op = root == nullptr ? nullptr : std::get_if<LogicOp>(&root->op);
            test.expect(op != nullptr && *op == LogicOp::kOr, "complex WHERE: OR root");
        }
    }

    expect_error(test, "unknown table", "DELETE FROM unknown;", catalog, {1, 13}, "table 'unknown' does not exist");
    expect_error(
        test,
        "table error first",
        "DELETE FROM unknown WHERE also_unknown = 1;",
        catalog,
        {1, 13},
        "table 'unknown' does not exist");
    expect_error(
        test,
        "unknown column",
        "DELETE FROM student WHERE unknown = 1;",
        catalog,
        {1, 27},
        "column 'unknown' does not exist");
    expect_error(
        test,
        "VARCHAR ordering",
        "DELETE FROM student WHERE name > 'A';",
        catalog,
        {1, 32},
        "VARCHAR");
    expect_error(test, "INT predicate", "DELETE FROM student WHERE age;", catalog, {1, 27}, "WHERE predicate must be BOOL");
    expect_error(test, "invalid NOT", "DELETE FROM student WHERE NOT 123;", catalog, {1, 27}, "NOT");

    if (test.failures() != 0) {
        std::cerr << test.failures() << " DELETE semantic test assertion(s) failed\n";
        return 1;
    }
    return 0;
}
