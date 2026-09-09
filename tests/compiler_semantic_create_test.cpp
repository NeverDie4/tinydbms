#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "bound_ast.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "semantic.hpp"

namespace {

using tinydbms::ColumnMeta;
using tinydbms::SourceLocation;
using tinydbms::TableMeta;
using tinydbms::Type;
using tinydbms::compiler::CatalogView;
using tinydbms::compiler::CompileError;
using tinydbms::compiler::CompileErrorKind;
using tinydbms::compiler::internal::BoundCreateTable;
using tinydbms::compiler::internal::BoundStatement;
using tinydbms::compiler::internal::LexResult;
using tinydbms::compiler::internal::ParseResult;
using tinydbms::compiler::internal::SemanticResult;
using tinydbms::compiler::internal::StatementAst;
using tinydbms::compiler::internal::Token;
using tinydbms::compiler::internal::analyze;
using tinydbms::compiler::internal::parse;
using tinydbms::compiler::internal::tokenize;

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
    std::string_view case_name,
    std::string_view sql,
    CatalogView catalog) {
    const LexResult lex_result = tokenize(sql);
    const auto* tokens = std::get_if<std::vector<Token>>(&lex_result.outcome);
    test.expect(tokens != nullptr, std::string{case_name} + ": lexer success");
    if (tokens == nullptr) {
        return SemanticResult{std::get<CompileError>(lex_result.outcome)};
    }
    const ParseResult parse_result = parse(*tokens);
    const auto* statement = std::get_if<StatementAst>(&parse_result.outcome);
    test.expect(statement != nullptr, std::string{case_name} + ": parser success");
    if (statement == nullptr) {
        return SemanticResult{std::get<CompileError>(parse_result.outcome)};
    }
    return analyze(*statement, catalog);
}

const BoundCreateTable* expect_create(
    TestContext& test,
    std::string_view case_name,
    const SemanticResult& result) {
    const auto* statement = std::get_if<BoundStatement>(&result.outcome);
    test.expect(statement != nullptr, std::string{case_name} + ": semantic success");
    if (statement == nullptr) {
        return nullptr;
    }
    const auto* create = std::get_if<BoundCreateTable>(&statement->kind);
    test.expect(create != nullptr, std::string{case_name} + ": bound CREATE");
    return create;
}

void expect_semantic_error(
    TestContext& test,
    std::string_view case_name,
    std::string_view sql,
    CatalogView catalog,
    SourceLocation location,
    std::string_view message_part) {
    const SemanticResult result = analyze_sql(test, case_name, sql, catalog);
    const auto* error = std::get_if<CompileError>(&result.outcome);
    const std::string prefix{case_name};
    test.expect(error != nullptr, prefix + ": semantic error returned");
    if (error == nullptr) {
        return;
    }
    test.expect(error->kind == CompileErrorKind::kSemantic, prefix + ": error kind");
    test.expect(error->location.line == location.line, prefix + ": error line");
    test.expect(error->location.column == location.column, prefix + ": error column");
    test.expect(error->message.find(message_part) != std::string::npos, prefix + ": error message");
}

}  // namespace

int main() {
    TestContext test;
    const CatalogView empty_catalog{std::span<const TableMeta>{}};

    {
        const SemanticResult result = analyze_sql(
            test, "empty catalog", "CREATE TABLE student(id INT,name VARCHAR);", empty_catalog);
        const auto* create = expect_create(test, "empty catalog", result);
        if (create != nullptr) {
            test.expect(create->table_name == "student", "empty catalog: table name");
            test.expect(create->columns.size() == 2, "empty catalog: column count");
            if (create->columns.size() == 2) {
                test.expect(create->columns[0].name == "id", "empty catalog: first column");
                test.expect(create->columns[0].type == Type::kInt, "empty catalog: first type");
                test.expect(create->columns[1].name == "name", "empty catalog: second column");
                test.expect(create->columns[1].type == Type::kVarchar, "empty catalog: second type");
            }
        }
    }

    const std::vector<TableMeta> tables{
        TableMeta{7U, "student", {ColumnMeta{"id", Type::kInt}}}
    };
    const CatalogView catalog{tables};

    expect_semantic_error(test, "existing table", "CREATE TABLE student(id INT);", catalog, {1, 14}, "already exists");
    expect_semantic_error(test, "normalized existing table", "CREATE TABLE Student(id INT);", catalog, {1, 14}, "student");
    expect_semantic_error(
        test,
        "duplicate column",
        "CREATE TABLE t(\nid INT,\nname VARCHAR,\nid VARCHAR\n);",
        empty_catalog,
        {4, 1},
        "duplicate column 'id'");
    expect_create(
        test,
        "different columns",
        analyze_sql(test, "different columns", "CREATE TABLE t(id INT,name VARCHAR);", empty_catalog));

    if (test.failures() != 0) {
        std::cerr << test.failures() << " CREATE semantic test assertion(s) failed\n";
        return 1;
    }
    return 0;
}
