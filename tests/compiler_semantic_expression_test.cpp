#include <cstdint>
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

const BoundExpr* expect_predicate(
    TestContext& test,
    std::string_view name,
    const SemanticResult& result) {
    const auto* statement = std::get_if<BoundStatement>(&result.outcome);
    test.expect(statement != nullptr, std::string{name} + ": semantic success");
    if (statement == nullptr) {
        return nullptr;
    }
    const auto* select = std::get_if<BoundSelect>(&statement->kind);
    test.expect(select != nullptr && select->predicate != nullptr, std::string{name} + ": predicate exists");
    return select == nullptr ? nullptr : select->predicate.get();
}

void expect_error(
    TestContext& test,
    std::string_view name,
    std::string_view expression,
    CatalogView catalog,
    SourceLocation location,
    std::string_view message_part) {
    const std::string sql = "SELECT * FROM student WHERE " + std::string{expression} + ";";
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

const BoundBinaryExpr* binary(const BoundExpr* expr) {
    return expr == nullptr ? nullptr : std::get_if<BoundBinaryExpr>(&expr->kind);
}

bool has_compare(const BoundBinaryExpr* expr, CmpOp expected) {
    if (expr == nullptr) {
        return false;
    }
    const auto* op = std::get_if<CmpOp>(&expr->op);
    return op != nullptr && *op == expected;
}

bool has_logic(const BoundBinaryExpr* expr, LogicOp expected) {
    if (expr == nullptr) {
        return false;
    }
    const auto* op = std::get_if<LogicOp>(&expr->op);
    return op != nullptr && *op == expected;
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

    const std::vector<std::string_view> valid_expressions{
        "id = 1",
        "id != 1",
        "id < age",
        "name = 'Alice'",
        "name != 'Bob'",
        "id = 1 AND age > 18",
        "id = 1 OR age > 18",
        "NOT id = 1",
        "id = 1 OR age >= 18 AND name != 'Tom'",
        "(id = 1 OR age = 18) AND name = 'Alice'",
        "1 < 2",
        "'A' = 'B'",
    };
    for (const std::string_view expression : valid_expressions) {
        const std::string sql = "SELECT * FROM student WHERE " + std::string{expression} + ";";
        const SemanticResult result = analyze_sql(test, expression, sql, catalog);
        expect_predicate(test, expression, result);
    }

    {
        const SemanticResult result = analyze_sql(
            test,
            "bound tree",
            "SELECT * FROM student WHERE age >= 18 AND name != 'Tom';",
            catalog);
        const BoundExpr* root = expect_predicate(test, "bound tree", result);
        const auto* conjunction = binary(root);
        test.expect(has_logic(conjunction, LogicOp::kAnd), "bound tree: AND root");
        if (conjunction != nullptr) {
            const auto* lhs = binary(conjunction->lhs.get());
            const auto* rhs = binary(conjunction->rhs.get());
            test.expect(has_compare(lhs, CmpOp::kGe), "bound tree: lhs >=");
            test.expect(has_compare(rhs, CmpOp::kNe), "bound tree: rhs !=");
            if (lhs != nullptr && rhs != nullptr) {
                const auto* age = std::get_if<BoundColumnRef>(&lhs->lhs->kind);
                const auto* age_value = std::get_if<BoundLiteral>(&lhs->rhs->kind);
                const auto* name = std::get_if<BoundColumnRef>(&rhs->lhs->kind);
                const auto* name_value = std::get_if<BoundLiteral>(&rhs->rhs->kind);
                test.expect(age != nullptr && age->column_id == 2U, "bound tree: age ColumnId");
                test.expect(name != nullptr && name->column_id == 1U, "bound tree: name ColumnId");
                const auto* integer = age_value == nullptr
                    ? nullptr
                    : std::get_if<std::int32_t>(&age_value->value.data);
                const auto* string = name_value == nullptr
                    ? nullptr
                    : std::get_if<std::string>(&name_value->value.data);
                test.expect(integer != nullptr && *integer == 18, "bound tree: integer literal");
                test.expect(string != nullptr && *string == "Tom", "bound tree: string literal");
            }
        }
    }

    expect_error(test, "unknown lhs", "unknown = 1", catalog, {1, 29}, "column 'unknown' does not exist");
    expect_error(test, "unknown rhs", "id = unknown", catalog, {1, 34}, "column 'unknown' does not exist");
    expect_error(
        test,
        "left error first",
        "unknown = another_unknown",
        catalog,
        {1, 29},
        "column 'unknown' does not exist");
    expect_error(test, "mixed comparison", "age = '18'", catalog, {1, 33}, "INT and VARCHAR");
    expect_error(test, "VARCHAR ordering", "name > 'Alice'", catalog, {1, 34}, "cannot be applied to VARCHAR");
    expect_error(test, "invalid AND", "1 AND 2", catalog, {1, 31}, "AND");
    expect_error(test, "invalid OR", "id OR age", catalog, {1, 32}, "OR");
    expect_error(test, "invalid NOT", "NOT 123", catalog, {1, 29}, "NOT");
    expect_error(test, "BOOL comparison", "(id = 1) = (age = 2)", catalog, {1, 38}, "BOOL");

    if (test.failures() != 0) {
        std::cerr << test.failures() << " expression semantic test assertion(s) failed\n";
        return 1;
    }
    return 0;
}
