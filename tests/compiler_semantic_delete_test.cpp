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

const BoundUpdate* expect_update(
    TestContext& test,
    std::string_view name,
    const SemanticResult& result) {
    const auto* statement = std::get_if<BoundStatement>(&result.outcome);
    test.expect(statement != nullptr, std::string{name} + ": semantic success");
    if (statement == nullptr) {
        return nullptr;
    }
    const auto* update = std::get_if<BoundUpdate>(&statement->kind);
    test.expect(update != nullptr, std::string{name} + ": bound UPDATE");
    return update;
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
        TableMeta{8U, "values_v2", {
            ColumnMeta{"id", Type::kInt, false},
            ColumnMeta{"big_value", Type::kBigInt, true},
            ColumnMeta{"score", Type::kDouble, true},
            ColumnMeta{"active", Type::kBoolean, true},
            ColumnMeta{"note", Type::kVarchar, true},
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

    {
        const SemanticResult result = analyze_sql(
            test,
            "update coercion",
            "UPDATE values_v2 SET big_value=1,score=2147483648,active=FALSE,note=NULL "
            "WHERE active IS NOT NULL;",
            catalog);
        const BoundUpdate* update = expect_update(test, "update coercion", result);
        test.expect(update != nullptr && update->table_id == 8U,
                    "update coercion: TableId");
        test.expect(update != nullptr && update->assignments.size() == 4U,
                    "update coercion: assignment count");
        if (update != nullptr && update->assignments.size() == 4U) {
            test.expect(update->assignments[0].column_id == 1U &&
                            std::holds_alternative<std::int64_t>(
                                update->assignments[0].value.data),
                        "update coercion: INT to BIGINT");
            test.expect(update->assignments[1].column_id == 2U &&
                            std::holds_alternative<double>(
                                update->assignments[1].value.data),
                        "update coercion: BIGINT to DOUBLE");
            test.expect(update->assignments[2].column_id == 3U &&
                            std::holds_alternative<bool>(
                                update->assignments[2].value.data),
                        "update coercion: BOOLEAN");
            test.expect(update->assignments[3].column_id == 4U &&
                            std::holds_alternative<std::monostate>(
                                update->assignments[3].value.data),
                        "update coercion: nullable NULL");
        }
        test.expect(update != nullptr && update->predicate != nullptr,
                    "update coercion: predicate");
    }
    expect_error(
        test, "duplicate target", "UPDATE values_v2 SET note='a',note='b';",
        catalog, {1, 31}, "duplicate column 'note'");
    expect_error(
        test, "unknown update table", "UPDATE missing SET id=1;",
        catalog, {1, 8}, "table 'missing' does not exist");
    expect_error(
        test, "unknown target", "UPDATE values_v2 SET missing=1;",
        catalog, {1, 22}, "column 'missing' does not exist");
    expect_error(
        test, "not null assignment", "UPDATE values_v2 SET id=NULL;",
        catalog, {1, 25}, "NOT NULL");
    expect_error(
        test, "narrowing assignment", "UPDATE values_v2 SET id=2147483648;",
        catalog, {1, 25}, "expects INT");
    expect_error(
        test, "invalid update predicate", "UPDATE values_v2 SET note='x' WHERE id;",
        catalog, {1, 37}, "WHERE predicate must be BOOL");

    if (test.failures() != 0) {
        std::cerr << test.failures() << " DELETE semantic test assertion(s) failed\n";
        return 1;
    }
    return 0;
}
