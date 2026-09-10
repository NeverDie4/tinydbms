#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "tinydbms/compiler.hpp"

namespace {

using namespace tinydbms;
using namespace tinydbms::compiler;

class TestContext {
public:
    void begin_case(std::string_view name) {
        current_case_ = name;
        ++case_count_;
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

CompileResult compile_sql(std::string sql, CatalogView catalog) {
    return compile(CompileRequest{std::move(sql), catalog});
}

const Plan* plan_of(TestContext& test, const CompileResult& result) {
    const auto* plan = std::get_if<Plan>(&result.outcome);
    test.expect(plan != nullptr, "expected compile success");
    return plan;
}

const CompileError* error_of(
    TestContext& test,
    const CompileResult& result,
    CompileErrorKind expected_kind) {
    const auto* error = std::get_if<CompileError>(&result.outcome);
    test.expect(error != nullptr, "expected compile error");
    if (error != nullptr) {
        test.expect(error->kind == expected_kind, "error kind");
    }
    return error;
}

void expect_location(TestContext& test, const CompileError* error, int line, int column) {
    if (error == nullptr) {
        return;
    }
    test.expect(error->location.line == line, "error line");
    test.expect(error->location.column == column, "error column");
}

const ProjectNode* project_of(TestContext& test, const Plan* plan) {
    const auto* query = plan == nullptr ? nullptr : std::get_if<QueryPlan>(&plan->kind);
    test.expect(query != nullptr, "expected QueryPlan");
    test.expect(query != nullptr && query->root != nullptr, "QueryPlan root is non-null");
    const auto* project = query == nullptr || query->root == nullptr
        ? nullptr
        : std::get_if<ProjectNode>(&query->root->kind);
    test.expect(project != nullptr, "expected Project root");
    return project;
}

const FilterNode* filter_of(TestContext& test, const ProjectNode* project) {
    test.expect(project != nullptr && project->child != nullptr, "Project child is non-null");
    const auto* filter = project == nullptr || project->child == nullptr
        ? nullptr
        : std::get_if<FilterNode>(&project->child->kind);
    test.expect(filter != nullptr, "expected Filter child");
    if (filter != nullptr) {
        test.expect(filter->child != nullptr, "Filter child is non-null");
    }
    return filter;
}

const Expr* predicate_of(TestContext& test, const CompileResult& result) {
    const auto* project = project_of(test, plan_of(test, result));
    const auto* filter = filter_of(test, project);
    return filter == nullptr ? nullptr : &filter->predicate;
}

const Binary* binary_of(const Expr* expression) {
    return expression == nullptr ? nullptr : std::get_if<Binary>(&expression->kind);
}

const Unary* unary_of(const Expr* expression) {
    return expression == nullptr ? nullptr : std::get_if<Unary>(&expression->kind);
}

bool has_logic(const Binary* expression, LogicOp expected) {
    const auto* op = expression == nullptr ? nullptr : std::get_if<LogicOp>(&expression->op);
    return op != nullptr && *op == expected;
}

bool has_compare(const Binary* expression, CmpOp expected) {
    const auto* op = expression == nullptr ? nullptr : std::get_if<CmpOp>(&expression->op);
    return op != nullptr && *op == expected;
}

const SeqScanNode* scan_of(const PlanNode* node) {
    return node == nullptr ? nullptr : std::get_if<SeqScanNode>(&node->kind);
}

void expect_split(
    TestContext& test,
    std::string_view input,
    const std::vector<std::string_view>& sql,
    const std::vector<SourceLocation>& locations) {
    const auto result = split_statements(input);
    const auto* statements = std::get_if<std::vector<SplitStatement>>(&result.outcome);
    test.expect(statements != nullptr, "split success");
    if (statements == nullptr) {
        return;
    }
    test.expect(statements->size() == sql.size(), "split statement count");
    test.expect(statements->size() == locations.size(), "split location count");
    if (statements->size() != sql.size() || statements->size() != locations.size()) {
        return;
    }
    for (std::size_t index = 0; index < statements->size(); ++index) {
        test.expect((*statements)[index].sql == sql[index], "split SQL");
        test.expect((*statements)[index].start.line == locations[index].line, "split line");
        test.expect((*statements)[index].start.column == locations[index].column, "split column");
    }
}

void test_splitter(TestContext& test) {
    test.begin_case("split string with all delimiters");
    expect_split(
        test,
        "INSERT INTO t VALUES ('; -- /* */');",
        {"INSERT INTO t VALUES ('; -- /* */');"},
        {{1, 1}});

    test.begin_case("split escaped quote followed by statement");
    expect_split(
        test,
        "INSERT INTO t VALUES ('Tom''s;book');SELECT * FROM t;",
        {"INSERT INTO t VALUES ('Tom''s;book');", "SELECT * FROM t;"},
        {{1, 1}, {1, 38}});

    test.begin_case("split comment containing string markers");
    expect_split(
        test,
        "/* ' ; -- */\nSELECT * FROM t;",
        {"/* ' ; -- */\nSELECT * FROM t;"},
        {{1, 1}});

    test.begin_case("split trailing line comment");
    expect_split(test, "SELECT * FROM t; -- comment", {"SELECT * FROM t;"}, {{1, 1}});

    test.begin_case("split trailing complete block comment");
    expect_split(test, "SELECT * FROM t; /* completed */", {"SELECT * FROM t;"}, {{1, 1}});

    test.begin_case("split trailing incomplete block comment");
    expect_split(
        test,
        "SELECT * FROM t; /* unfinished",
        {"SELECT * FROM t;", " /* unfinished"},
        {{1, 1}, {1, 17}});

    test.begin_case("split empty segments and comments");
    expect_split(
        test,
        ";; /*a*/ ; --b\r\n; SELECT * FROM t;;;;;",
        {" SELECT * FROM t;"},
        {{2, 2}});

    test.begin_case("split CRLF comments and statements");
    expect_split(
        test,
        "-- lead\r\nSELECT * FROM a;/*gap*/\r\nSELECT * FROM b;",
        {"-- lead\r\nSELECT * FROM a;", "/*gap*/\r\nSELECT * FROM b;"},
        {{1, 1}, {2, 17}});

    test.begin_case("split deterministic repeated calls");
    const std::string input = "SELECT * FROM a; -- gap\nSELECT * FROM b;";
    const auto first_result = split_statements(input);
    const auto* first = std::get_if<std::vector<SplitStatement>>(&first_result.outcome);
    test.expect(first != nullptr, "first split success");
    for (int iteration = 0; iteration < 10; ++iteration) {
        const auto next_result = split_statements(input);
        const auto* next = std::get_if<std::vector<SplitStatement>>(&next_result.outcome);
        test.expect(next != nullptr, "repeated split success");
        if (first == nullptr || next == nullptr) {
            continue;
        }
        test.expect(next->size() == first->size(), "repeated split size");
        if (next->size() == first->size()) {
            for (std::size_t index = 0; index < first->size(); ++index) {
                test.expect((*next)[index].sql == (*first)[index].sql, "repeated split SQL");
                test.expect(
                    (*next)[index].start.line == (*first)[index].start.line,
                    "repeated split line");
                test.expect(
                    (*next)[index].start.column == (*first)[index].start.column,
                    "repeated split column");
            }
        }
    }
}

void test_lexical_and_locations(TestContext& test, CatalogView catalog) {
    test.begin_case("keyword prefixes remain identifiers");
    const auto prefixes = compile_sql(
        "SELECT SELECTA,TABLE1,ORANGE,NOTHING,INTEGER FROM lexical;",
        catalog);
    const auto* prefix_project = project_of(test, plan_of(test, prefixes));
    if (prefix_project != nullptr) {
        test.expect(
            prefix_project->outputs == std::vector<ColumnId>{0U, 1U, 2U, 3U, 4U},
            "keyword-prefix ColumnIds");
    }

    test.begin_case("extreme keyword case");
    plan_of(test, compile_sql("sElEcT a FrOm lexical WhErE a = 1 aNd a = 1;", catalog));

    test.begin_case("identifier underscore forms");
    const auto underscores = compile_sql("SELECT a_b,a_,a1_,A_B_C FROM lexical;", catalog);
    const auto* underscore_project = project_of(test, plan_of(test, underscores));
    if (underscore_project != nullptr) {
        test.expect(
            underscore_project->outputs == std::vector<ColumnId>{6U, 7U, 8U, 9U},
            "underscore ColumnIds");
    }

    test.begin_case("invalid leading underscore");
    expect_location(
        test,
        error_of(test, compile_sql("SELECT _abc FROM lexical;", catalog), CompileErrorKind::kLex),
        1,
        8);

    const std::string identifier_64(64, 'x');
    const std::string identifier_65(65, 'x');
    test.begin_case("identifier 64 in statement");
    plan_of(test, compile_sql("SELECT " + identifier_64 + " FROM long_names;", catalog));

    test.begin_case("identifier 65 location in statement");
    expect_location(
        test,
        error_of(
            test,
            compile_sql("SELECT " + identifier_65 + " FROM long_names;", catalog),
            CompileErrorKind::kLex),
        1,
        8);

    test.begin_case("integer boundaries");
    plan_of(test, compile_sql("SELECT id FROM numbers WHERE 0 <= 2147483647;", catalog));

    test.begin_case("negative integer and INT32_MIN");
    plan_of(test, compile_sql("SELECT id FROM numbers WHERE id >= -2147483648;", catalog));
    plan_of(test, compile_sql("INSERT INTO numbers VALUES (-1),(-2147483648);", catalog));

    test.begin_case("positive integer overflow is semantic error");
    expect_location(
        test,
        error_of(
            test,
            compile_sql("SELECT id FROM numbers WHERE id = 2147483648;", catalog),
            CompileErrorKind::kSemantic),
        1,
        35);

    test.begin_case("negative integer overflow is semantic error");
    expect_location(
        test,
        error_of(
            test,
            compile_sql("SELECT id FROM numbers WHERE id = -2147483649;", catalog),
            CompileErrorKind::kSemantic),
        1,
        35);

    test.begin_case("very long integer is stable semantic error");
    expect_location(
        test,
        error_of(
            test,
            compile_sql(
                "SELECT id FROM numbers WHERE id = 999999999999999999999999999999;",
                catalog),
            CompileErrorKind::kSemantic),
        1,
        35);

    test.begin_case("escaped quote decodes to one quote");
    const auto quote_result = compile_sql("INSERT INTO strings VALUES ('''');", catalog);
    const auto* quote_plan = plan_of(test, quote_result);
    const auto* quote_insert = quote_plan == nullptr
        ? nullptr
        : std::get_if<InsertPlan>(&quote_plan->kind);
    test.expect(quote_insert != nullptr, "expected InsertPlan");
    if (quote_insert != nullptr) {
        const auto* value = std::get_if<std::string>(&quote_insert->rows[0][0].data);
        test.expect(value != nullptr && *value == "'", "decoded quote value");
    }

    test.begin_case("string punctuation decodes literally");
    const auto punctuation_result = compile_sql(
        "INSERT INTO strings VALUES (';'),('--'),('/* */'),('a,b;c');",
        catalog);
    const auto* punctuation_plan = plan_of(test, punctuation_result);
    const auto* punctuation_insert = punctuation_plan == nullptr
        ? nullptr
        : std::get_if<InsertPlan>(&punctuation_plan->kind);
    test.expect(punctuation_insert != nullptr, "expected punctuation InsertPlan");
    if (punctuation_insert != nullptr) {
        test.expect(punctuation_insert->rows.size() == 4, "punctuation row count");
    }

    test.begin_case("multiline unterminated string location");
    expect_location(
        test,
        error_of(test, compile_sql("'abc\ndef", catalog), CompileErrorKind::kLex),
        1,
        1);

    test.begin_case("UTF-8 byte length boundary");
    const std::string chinese = "中";
    const std::string exact = std::string(1021, 'a') + chinese;
    const std::string too_long = std::string(1022, 'a') + chinese;
    plan_of(test, compile_sql("INSERT INTO strings VALUES ('" + exact + "');", catalog));
    error_of(
        test,
        compile_sql("INSERT INTO strings VALUES ('" + too_long + "');", catalog),
        CompileErrorKind::kLex);

    test.begin_case("comment adjacency");
    plan_of(test, compile_sql("SELECT/**/a FROM lexical;", catalog));

    test.begin_case("operator longest match");
    plan_of(
        test,
        compile_sql("SELECT id FROM numbers WHERE id>=18 AND id!=19 AND id<=20;", catalog));

    test.begin_case("unsupported punctuation is rejected");
    const std::vector<std::string_view> unsupported{"#", "!", "+", "-", "/", "%"};
    for (const std::string_view symbol : unsupported) {
        const auto result = compile_sql(
            "SELECT id FROM numbers WHERE id " + std::string{symbol} + " 1;",
            catalog);
        test.expect(std::holds_alternative<CompileError>(result.outcome), "unsupported token rejected");
    }
    test.expect(
        std::holds_alternative<CompileError>(
            compile_sql("SELECT id FROM numbers WHERE id <> 1;", catalog).outcome),
        "unsupported <> rejected");

    test.begin_case("multiline lex error location");
    expect_location(
        test,
        error_of(
            test,
            compile_sql("SELECT id\nFROM student\nWHERE @ = 1;", catalog),
            CompileErrorKind::kLex),
        3,
        7);

    test.begin_case("multiline syntax error location");
    expect_location(
        test,
        error_of(
            test,
            compile_sql("SELECT id\nFROM student\nWHERE age = ;", catalog),
            CompileErrorKind::kSyntax),
        3,
        13);

    test.begin_case("multiline semantic error location");
    expect_location(
        test,
        error_of(
            test,
            compile_sql("SELECT id\nFROM student\nWHERE unknown = 1;", catalog),
            CompileErrorKind::kSemantic),
        3,
        7);

    test.begin_case("CRLF semantic error location");
    expect_location(
        test,
        error_of(
            test,
            compile_sql("SELECT id\r\nFROM student\r\nWHERE unknown = 1;", catalog),
            CompileErrorKind::kSemantic),
        3,
        7);

    test.begin_case("UTF-8 byte column after literal");
    expect_location(
        test,
        error_of(test, compile_sql("'中' @", catalog), CompileErrorKind::kLex),
        1,
        7);
}

void test_parser_and_semantics(TestContext& test, CatalogView catalog) {
    const std::vector<std::string_view> valid_sql{
        "CREATE TABLE created(a INT,b VARCHAR,c INT);",
        "INSERT INTO student VALUES (1,'Alice',20),(2,'Bob',21),(3,'Carol',22);",
        "INSERT INTO student(aGe,ID,NaMe) VALUES (20,1,'Alice'),(21,2,'Bob');",
        "SELECT id,name,age FROM student;",
        "SELECT * FROM student WHERE id = 1;",
        "DELETE FROM student;",
        "DELETE FROM student WHERE name != 'Alice';",
    };
    for (const std::string_view sql : valid_sql) {
        test.begin_case("valid parser and semantic form");
        plan_of(test, compile_sql(std::string{sql}, catalog));
    }

    const std::vector<std::string_view> invalid_syntax{
        "CREATE TABLE t(a INT,,b INT);",
        "CREATE TABLE t(a INT b VARCHAR);",
        "CREATE TABLE t(a INT,b VARCHAR,);",
        "INSERT INTO student(id) VALUES ((1));",
        "INSERT INTO student VALUES (1), (2), ;",
        "SELECT *,id FROM student;",
        "SELECT id,* FROM student;",
        "SELECT id,,age FROM student;",
        "DELETE FROM student WHERE;",
    };
    for (const std::string_view sql : invalid_syntax) {
        test.begin_case("invalid parser form");
        error_of(test, compile_sql(std::string{sql}, catalog), CompileErrorKind::kSyntax);
    }

    test.begin_case("projection error precedes predicate error");
    expect_location(
        test,
        error_of(
            test,
            compile_sql("SELECT unknown FROM student WHERE another_unknown = 1;", catalog),
            CompileErrorKind::kSemantic),
        1,
        8);

    test.begin_case("table error precedes predicate error");
    expect_location(
        test,
        error_of(
            test,
            compile_sql("SELECT id FROM unknown WHERE x = 1;", catalog),
            CompileErrorKind::kSemantic),
        1,
        16);

    test.begin_case("INSERT unknown column precedes other errors");
    expect_location(
        test,
        error_of(
            test,
            compile_sql("INSERT INTO student(unknown,id,id) VALUES ('bad');", catalog),
            CompileErrorKind::kSemantic),
        1,
        21);

    test.begin_case("INSERT duplicate precedes row checks");
    expect_location(
        test,
        error_of(
            test,
            compile_sql("INSERT INTO student(id,id,age) VALUES (1,2,3);", catalog),
            CompileErrorKind::kSemantic),
        1,
        24);

    test.begin_case("INSERT earlier row count error precedes later type error");
    const auto row_priority = compile_sql(
        "INSERT INTO student VALUES (1,'ok',20),(2,'short'),(3,4,30);",
        catalog);
    const auto* row_error = error_of(test, row_priority, CompileErrorKind::kSemantic);
    expect_location(test, row_error, 1, 41);
    if (row_error != nullptr) {
        test.expect(row_error->message.find("count") != std::string::npos, "row count reported first");
    }

    test.begin_case("INSERT partial columns rejected");
    error_of(
        test,
        compile_sql("INSERT INTO student(id,name) VALUES (1,'Alice');", catalog),
        CompileErrorKind::kSemantic);

    test.begin_case("INSERT too few values rejected");
    error_of(
        test,
        compile_sql("INSERT INTO student(id,name,age) VALUES (1,'Alice');", catalog),
        CompileErrorKind::kSemantic);

    test.begin_case("INSERT too many values rejected");
    error_of(
        test,
        compile_sql("INSERT INTO student(id,name,age) VALUES (1,'Alice',20,100);", catalog),
        CompileErrorKind::kSemantic);

    test.begin_case("INSERT reordered first type error");
    const auto reordered_error = compile_sql(
        "INSERT INTO student(name,age,id) VALUES (1,20,'Alice');",
        catalog);
    const auto* type_error = error_of(test, reordered_error, CompileErrorKind::kSemantic);
    if (type_error != nullptr) {
        test.expect(type_error->message.find("name") != std::string::npos, "first target column reported");
    }

    const std::vector<std::string_view> invalid_predicates{
        "SELECT id FROM student WHERE id;",
        "SELECT id FROM student WHERE 1;",
        "SELECT id FROM student WHERE id = '1';",
        "SELECT id FROM student WHERE name > 'A';",
        "SELECT id FROM student WHERE (id = 1) = (age = 2);",
    };
    for (const std::string_view sql : invalid_predicates) {
        test.begin_case("invalid predicate semantics");
        error_of(test, compile_sql(std::string{sql}, catalog), CompileErrorKind::kSemantic);
    }

    test.begin_case("duplicate SELECT projection retained");
    const auto duplicate = compile_sql("SELECT id,id FROM student;", catalog);
    const auto* duplicate_project = project_of(test, plan_of(test, duplicate));
    if (duplicate_project != nullptr) {
        test.expect(duplicate_project->outputs == std::vector<ColumnId>{0U, 0U}, "duplicate outputs");
    }

    test.begin_case("Catalog lookup stays within FROM table");
    error_of(
        test,
        compile_sql("SELECT score FROM table_a;", catalog),
        CompileErrorKind::kSemantic);

    test.begin_case("non-contiguous TableId retained");
    const auto id_result = compile_sql("SELECT id FROM student;", catalog);
    const auto* id_project = project_of(test, plan_of(test, id_result));
    const auto* id_scan = id_project == nullptr ? nullptr : scan_of(id_project->child.get());
    test.expect(id_scan != nullptr && id_scan->table_id == 42U, "TableId 42 retained");

    test.begin_case("empty Catalog CREATE and SELECT");
    const CatalogView empty{std::span<const TableMeta>{}};
    plan_of(test, compile_sql("CREATE TABLE empty_ok(id INT);", empty));
    error_of(test, compile_sql("SELECT * FROM student;", empty), CompileErrorKind::kSemantic);
}

void test_expression_plans(TestContext& test, CatalogView catalog) {
    test.begin_case("OR with tighter AND");
    const auto e1_result = compile_sql(
        "SELECT id FROM student WHERE id = 1 OR age = 2 AND id = 3;",
        catalog);
    const Expr* e1 = predicate_of(test, e1_result);
    const auto* e1_root = binary_of(e1);
    test.expect(has_logic(e1_root, LogicOp::kOr), "OR root");
    test.expect(e1_root != nullptr && has_logic(binary_of(e1_root->rhs.get()), LogicOp::kAnd), "AND rhs");

    test.begin_case("parentheses override precedence");
    const auto e2_result = compile_sql(
        "SELECT id FROM student WHERE (id = 1 OR age = 2) AND id = 3;",
        catalog);
    const Expr* e2 = predicate_of(test, e2_result);
    const auto* e2_root = binary_of(e2);
    test.expect(has_logic(e2_root, LogicOp::kAnd), "AND root");
    test.expect(e2_root != nullptr && has_logic(binary_of(e2_root->lhs.get()), LogicOp::kOr), "OR lhs");

    test.begin_case("NOT comparison before AND");
    const auto e3_result = compile_sql(
        "SELECT id FROM student WHERE NOT id = 1 AND age = 2;",
        catalog);
    const Expr* e3 = predicate_of(test, e3_result);
    const auto* e3_root = binary_of(e3);
    const auto* e3_not = e3_root == nullptr ? nullptr : unary_of(e3_root->lhs.get());
    test.expect(has_logic(e3_root, LogicOp::kAnd), "AND root");
    test.expect(e3_not != nullptr, "NOT lhs");
    test.expect(e3_not != nullptr && has_compare(binary_of(e3_not->operand.get()), CmpOp::kEq), "NOT comparison");

    test.begin_case("double NOT");
    const auto e4_result = compile_sql("SELECT id FROM student WHERE NOT NOT id = 1;", catalog);
    const Expr* e4 = predicate_of(test, e4_result);
    test.expect(has_compare(binary_of(e4), CmpOp::kEq), "double NOT eliminated");

    test.begin_case("OR is left associative");
    const auto e5_result = compile_sql(
        "SELECT id FROM student WHERE id = 1 OR age = 2 OR id = 3;",
        catalog);
    const Expr* e5 = predicate_of(test, e5_result);
    const auto* e5_root = binary_of(e5);
    test.expect(has_logic(e5_root, LogicOp::kOr), "OR root");
    test.expect(e5_root != nullptr && has_logic(binary_of(e5_root->lhs.get()), LogicOp::kOr), "OR lhs");

    test.begin_case("AND is left associative");
    const auto e6_result = compile_sql(
        "SELECT id FROM student WHERE id = 1 AND age = 2 AND id = 3;",
        catalog);
    const Expr* e6 = predicate_of(test, e6_result);
    const auto* e6_root = binary_of(e6);
    test.expect(has_logic(e6_root, LogicOp::kAnd), "AND root");
    test.expect(e6_root != nullptr && has_logic(binary_of(e6_root->lhs.get()), LogicOp::kAnd), "AND lhs");

    test.begin_case("nested parentheses produce no nodes");
    const auto e7_result = compile_sql("SELECT id FROM student WHERE (((id = 1))); ", catalog);
    const Expr* e7 = predicate_of(test, e7_result);
    test.expect(has_compare(binary_of(e7), CmpOp::kEq), "comparison remains root");

    test.begin_case("constant TRUE predicate is optimized");
    const auto constant_result = compile_sql("SELECT id FROM student WHERE 1 = 1;", catalog);
    const auto* constant_project = project_of(test, plan_of(test, constant_result));
    test.expect(
        constant_project != nullptr && scan_of(constant_project->child.get()) != nullptr,
        "constant TRUE Filter eliminated");
}

void test_plan_invariants(TestContext& test, CatalogView catalog) {
    test.begin_case("SELECT without WHERE plan shape");
    const auto plain = compile_sql("SELECT * FROM student;", catalog);
    const auto* plain_project = project_of(test, plan_of(test, plain));
    test.expect(plain_project != nullptr && plain_project->child != nullptr, "Project child");
    test.expect(
        plain_project != nullptr && scan_of(plain_project->child.get()) != nullptr,
        "Project directly contains SeqScan");

    test.begin_case("SELECT with WHERE plan shape and operands");
    const auto filtered = compile_sql("SELECT id FROM student WHERE NOT id = 1;", catalog);
    const auto* filtered_project = project_of(test, plan_of(test, filtered));
    const auto* filter = filter_of(test, filtered_project);
    const auto* negation = filter == nullptr ? nullptr : unary_of(&filter->predicate);
    const auto* comparison = negation == nullptr ? nullptr : binary_of(negation->operand.get());
    test.expect(negation != nullptr && negation->operand != nullptr, "Unary operand");
    test.expect(comparison != nullptr && comparison->lhs != nullptr, "Binary lhs");
    test.expect(comparison != nullptr && comparison->rhs != nullptr, "Binary rhs");
    test.expect(filter != nullptr && scan_of(filter->child.get()) != nullptr, "Filter contains SeqScan");

    test.begin_case("DELETE remains DeletePlan");
    const auto deletion = compile_sql("DELETE FROM student WHERE id = 1;", catalog);
    const auto* deletion_plan = plan_of(test, deletion);
    test.expect(
        deletion_plan != nullptr && std::holds_alternative<DeletePlan>(deletion_plan->kind),
        "DeletePlan kind");

    test.begin_case("INSERT reorder preserves row order");
    const auto insertion = compile_sql(
        "INSERT INTO student(age,id,name) VALUES (20,1,'Alice');",
        catalog);
    const auto* insertion_plan = plan_of(test, insertion);
    const auto* insert = insertion_plan == nullptr
        ? nullptr
        : std::get_if<InsertPlan>(&insertion_plan->kind);
    test.expect(insert != nullptr, "InsertPlan kind");
    if (insert != nullptr) {
        test.expect(insert->columns == std::vector<ColumnId>{2U, 0U, 1U}, "reordered columns");
        test.expect(std::get<std::int32_t>(insert->rows[0][0].data) == 20, "first row value remains age");
        test.expect(std::get<std::int32_t>(insert->rows[0][1].data) == 1, "second row value remains id");
        test.expect(std::get<std::string>(insert->rows[0][2].data) == "Alice", "third row value remains name");
    }
}

void test_public_defense_and_state(TestContext& test, CatalogView catalog, std::vector<TableMeta>& tables) {
    const std::vector<std::pair<std::string_view, CompileErrorKind>> invalid_inputs{
        {"", CompileErrorKind::kSyntax},
        {"   \t\r\n", CompileErrorKind::kSyntax},
        {"-- abc", CompileErrorKind::kSyntax},
        {"/* abc */", CompileErrorKind::kSyntax},
        {"/* abc", CompileErrorKind::kLex},
        {"'abc", CompileErrorKind::kLex},
        {"SELECT * FROM student; DELETE FROM student;", CompileErrorKind::kSyntax},
        {";", CompileErrorKind::kSyntax},
    };
    for (const auto& [sql, kind] : invalid_inputs) {
        test.begin_case("public compile defensive input");
        error_of(test, compile_sql(std::string{sql}, catalog), kind);
    }

    test.begin_case("unsupported UPDATE remains rejected");
    test.expect(
        std::holds_alternative<CompileError>(
            compile_sql("UPDATE student SET id = 1;", catalog).outcome),
        "UPDATE rejected");

    test.begin_case("ten repeated compiles agree");
    for (int iteration = 0; iteration < 10; ++iteration) {
        const auto result = compile_sql("SELECT name,id FROM student;", catalog);
        const auto* project = project_of(test, plan_of(test, result));
        if (project != nullptr) {
            test.expect(project->outputs == std::vector<ColumnId>{1U, 0U}, "stable outputs");
            const auto* scan = scan_of(project->child.get());
            test.expect(scan != nullptr && scan->table_id == 42U, "stable TableId");
        }
    }

    test.begin_case("interleaved requests are independent");
    const std::vector<std::string_view> sequence{
        "SELECT id FROM student;",
        "DELETE FROM student;",
        "INSERT INTO student VALUES (1,'A',2);",
        "SELECT name FROM student;",
        "CREATE TABLE fresh(id INT);",
        "SELECT age FROM student;",
    };
    for (const std::string_view sql : sequence) {
        plan_of(test, compile_sql(std::string{sql}, catalog));
    }

    test.begin_case("CatalogView remains unchanged");
    const auto before = tables;
    plan_of(test, compile_sql("CREATE TABLE untouched(id INT);", catalog));
    test.expect(tables.size() == before.size(), "catalog size");
    for (std::size_t index = 0; index < tables.size() && index < before.size(); ++index) {
        test.expect(tables[index].table_id == before[index].table_id, "catalog TableId");
        test.expect(tables[index].table_name == before[index].table_name, "catalog table name");
        test.expect(tables[index].columns.size() == before[index].columns.size(), "catalog columns size");
    }

    test.begin_case("valid compile after lex error");
    error_of(test, compile_sql("SELECT @ FROM student;", catalog), CompileErrorKind::kLex);
    plan_of(test, compile_sql("SELECT id FROM student;", catalog));

    test.begin_case("valid compile after semantic error");
    error_of(test, compile_sql("SELECT missing FROM student;", catalog), CompileErrorKind::kSemantic);
    plan_of(test, compile_sql("SELECT id FROM student;", catalog));
}

void test_moderate_stress(TestContext& test, CatalogView catalog) {
    test.begin_case("split 100 statements");
    std::string script;
    for (int index = 0; index < 100; ++index) {
        script += "SELECT * FROM student;";
    }
    const auto split_result = split_statements(script);
    const auto* statements = std::get_if<std::vector<SplitStatement>>(&split_result.outcome);
    test.expect(statements != nullptr && statements->size() == 100, "100 statements split");

    test.begin_case("INSERT 50 rows");
    std::string insert = "INSERT INTO numbers VALUES ";
    for (int index = 0; index < 50; ++index) {
        if (index != 0) {
            insert += ',';
        }
        insert += '(' + std::to_string(index) + ')';
    }
    insert += ';';
    const auto insert_result = compile_sql(std::move(insert), catalog);
    const auto* insert_plan = plan_of(test, insert_result);
    const auto* rows = insert_plan == nullptr ? nullptr : std::get_if<InsertPlan>(&insert_plan->kind);
    test.expect(rows != nullptr && rows->rows.size() == 50, "50 INSERT rows");

    test.begin_case("long AND OR expression");
    std::string query = "SELECT id FROM numbers WHERE ";
    for (int index = 0; index < 40; ++index) {
        if (index != 0) {
            query += index % 2 == 0 ? " OR " : " AND ";
        }
        query += "id = " + std::to_string(index);
    }
    query += ';';
    plan_of(test, compile_sql(std::move(query), catalog));

    test.begin_case("bounded NOT nesting remains valid");
    std::string bounded_not = "SELECT id FROM numbers WHERE ";
    for (int index = 0; index < 64; ++index) {
        bounded_not += "NOT ";
    }
    bounded_not += "id = 1;";
    plan_of(test, compile_sql(std::move(bounded_not), catalog));

    test.begin_case("expression complexity exact NOT boundary is accepted");
    std::string maximum_not = "SELECT id FROM numbers WHERE ";
    for (int index = 0; index < 253; ++index) {
        maximum_not += "NOT ";
    }
    maximum_not += "id = 1;";
    plan_of(test, compile_sql(std::move(maximum_not), catalog));

    test.begin_case("expression complexity beyond NOT boundary is rejected");
    std::string excessive_not = "SELECT id FROM numbers WHERE ";
    for (int index = 0; index < 254; ++index) {
        excessive_not += "NOT ";
    }
    excessive_not += "id = 1;";
    error_of(test, compile_sql(std::move(excessive_not), catalog), CompileErrorKind::kSyntax);

    test.begin_case("deep NOT nesting is rejected");
    std::string deep_not = "SELECT id FROM numbers WHERE ";
    for (int index = 0; index < 1024; ++index) {
        deep_not += "NOT ";
    }
    deep_not += "id = 1;";
    error_of(test, compile_sql(std::move(deep_not), catalog), CompileErrorKind::kSyntax);

    test.begin_case("deep left-associated expression is rejected");
    std::string deep_and = "SELECT id FROM numbers WHERE id = 1";
    for (int index = 0; index < 1024; ++index) {
        deep_and += " AND id = 1";
    }
    deep_and += ';';
    error_of(test, compile_sql(std::move(deep_and), catalog), CompileErrorKind::kSyntax);

    test.begin_case("expression complexity exact AND boundary is accepted");
    std::string maximum_and = "SELECT id FROM numbers WHERE ";
    for (int index = 0; index < 64; ++index) {
        if (index != 0) {
            maximum_and += " AND ";
        }
        maximum_and += "id = 1";
    }
    maximum_and += ';';
    plan_of(test, compile_sql(std::move(maximum_and), catalog));

    test.begin_case("expression complexity beyond AND boundary is rejected");
    std::string excessive_and = "SELECT id FROM numbers WHERE ";
    for (int index = 0; index < 65; ++index) {
        if (index != 0) {
            excessive_and += " AND ";
        }
        excessive_and += "id = 1";
    }
    excessive_and += ';';
    error_of(test, compile_sql(std::move(excessive_and), catalog), CompileErrorKind::kSyntax);

    test.begin_case("SQL text length boundary");
    std::string maximum_sql = "SELECT * FROM student";
    maximum_sql.resize(kMaxSqlBytes - 1, ' ');
    maximum_sql.push_back(';');
    plan_of(test, compile_sql(maximum_sql, catalog));
    const auto maximum_split = split_statements(maximum_sql);
    const auto* maximum_statements =
        std::get_if<std::vector<SplitStatement>>(&maximum_split.outcome);
    test.expect(
        maximum_statements != nullptr && maximum_statements->size() == 1,
        "maximum SQL text splits");

    test.begin_case("oversized compile input is rejected");
    std::string oversized_sql = maximum_sql + ' ';
    expect_location(
        test,
        error_of(
            test,
            compile_sql(oversized_sql, catalog),
            CompileErrorKind::kLex),
        1,
        1);

    test.begin_case("oversized splitter input returns lexical error");
    const auto oversized_split = split_statements(oversized_sql);
    const auto* split_error = std::get_if<CompileError>(&oversized_split.outcome);
    test.expect(split_error != nullptr, "oversized split error");
    if (split_error != nullptr) {
        test.expect(split_error->kind == CompileErrorKind::kLex, "oversized split error kind");
        test.expect(split_error->location.line == 1, "oversized split error line");
        test.expect(split_error->location.column == 1, "oversized split error column");
    }
}

}  // namespace

int main() {
    TestContext test;
    const std::string identifier_64(64, 'x');
    std::vector<TableMeta> tables{
        TableMeta{42U, "student", {
            ColumnMeta{"id", Type::kInt},
            ColumnMeta{"name", Type::kVarchar},
            ColumnMeta{"age", Type::kInt},
        }},
        TableMeta{3U, "lexical", {
            ColumnMeta{"selecta", Type::kInt},
            ColumnMeta{"table1", Type::kInt},
            ColumnMeta{"orange", Type::kInt},
            ColumnMeta{"nothing", Type::kInt},
            ColumnMeta{"integer", Type::kInt},
            ColumnMeta{"a", Type::kInt},
            ColumnMeta{"a_b", Type::kInt},
            ColumnMeta{"a_", Type::kInt},
            ColumnMeta{"a1_", Type::kInt},
            ColumnMeta{"a_b_c", Type::kInt},
        }},
        TableMeta{77U, "long_names", {ColumnMeta{identifier_64, Type::kInt}}},
        TableMeta{8U, "strings", {ColumnMeta{"value", Type::kVarchar}}},
        TableMeta{9U, "numbers", {ColumnMeta{"id", Type::kInt}}},
        TableMeta{10U, "table_a", {ColumnMeta{"id", Type::kInt}}},
        TableMeta{11U, "table_b", {ColumnMeta{"score", Type::kInt}}},
    };
    const CatalogView catalog{tables};

    test_splitter(test);
    test_lexical_and_locations(test, catalog);
    test_parser_and_semantics(test, catalog);
    test_expression_plans(test, catalog);
    test_plan_invariants(test, catalog);
    test_public_defense_and_state(test, catalog, tables);
    test_moderate_stress(test, catalog);

    if (test.failures() != 0) {
        std::cerr << test.failures() << " hardening assertion(s) failed across "
                  << test.case_count() << " cases\n";
        return 1;
    }

    std::cout << test.case_count() << " hardening cases passed\n";
    return 0;
}
