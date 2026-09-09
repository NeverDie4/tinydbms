#include <iostream>
#include <span>
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

SemanticResult analyze_sql(TestContext& test, std::string_view name, std::string_view sql) {
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
    return analyze(*statement, CatalogView{std::span<const TableMeta>{}});
}

const CreateTablePlan* generate_create(
    TestContext& test,
    std::string_view name,
    SemanticResult& result,
    Plan& plan) {
    auto* statement = std::get_if<BoundStatement>(&result.outcome);
    test.expect(statement != nullptr, std::string{name} + ": semantic success");
    if (statement == nullptr) {
        return nullptr;
    }
    auto* create = std::get_if<BoundCreateTable>(&statement->kind);
    test.expect(create != nullptr, std::string{name} + ": bound CREATE");
    if (create == nullptr) {
        return nullptr;
    }
    plan = generate_plan(std::move(*create));
    const auto* output = std::get_if<CreateTablePlan>(&plan.kind);
    test.expect(output != nullptr, std::string{name} + ": public CreateTablePlan");
    return output;
}

}  // namespace

int main() {
    TestContext test;

    {
        SemanticResult result = analyze_sql(test, "single column", "CREATE TABLE student(id INT);");
        Plan plan{CreateTablePlan{}};
        const auto* create = generate_create(test, "single column", result, plan);
        if (create != nullptr) {
            test.expect(create->table_name == "student", "single column: table name");
            test.expect(create->columns.size() == 1, "single column: column count");
            if (create->columns.size() == 1) {
                test.expect(create->columns[0].name == "id", "single column: column name");
                test.expect(create->columns[0].type == Type::kInt, "single column: column type");
            }
        }
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "column order",
            "CREATE TABLE student(z INT,a VARCHAR,m INT);");
        Plan plan{CreateTablePlan{}};
        const auto* create = generate_create(test, "column order", result, plan);
        if (create != nullptr && create->columns.size() == 3) {
            test.expect(create->columns[0].name == "z", "column order: z first");
            test.expect(create->columns[0].type == Type::kInt, "column order: z type");
            test.expect(create->columns[1].name == "a", "column order: a second");
            test.expect(create->columns[1].type == Type::kVarchar, "column order: a type");
            test.expect(create->columns[2].name == "m", "column order: m third");
            test.expect(create->columns[2].type == Type::kInt, "column order: m type");
        }
    }

    {
        SemanticResult result = analyze_sql(
            test,
            "normalization",
            "CREATE TABLE Student(ID INT,Name VARCHAR);");
        Plan plan{CreateTablePlan{}};
        const auto* create = generate_create(test, "normalization", result, plan);
        if (create != nullptr && create->columns.size() == 2) {
            test.expect(create->table_name == "student", "normalization: table");
            test.expect(create->columns[0].name == "id", "normalization: id");
            test.expect(create->columns[1].name == "name", "normalization: name");
        }
    }

    if (test.failures() != 0) {
        std::cerr << test.failures() << " CREATE planner test assertion(s) failed\n";
        return 1;
    }
    return 0;
}
