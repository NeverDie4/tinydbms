#include "fakes/compiler_fake.hpp"
#include "fakes/storage_fake.hpp"
#include "tinydbms/core.hpp"

#include "../src/core/plan_text.hpp"

#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using tinydbms::ColumnMeta;
using tinydbms::CompileStage;
using tinydbms::SlotId;
using tinydbms::TableId;
using tinydbms::TableMeta;
using tinydbms::Type;
using tinydbms::Value;
using tinydbms::compiler::AggregateCall;
using tinydbms::compiler::AggregateKind;
using tinydbms::compiler::AggregateNode;
using tinydbms::compiler::CmpOp;
using tinydbms::compiler::ColumnRef;
using tinydbms::compiler::CompileResult;
using tinydbms::compiler::CreateTablePlan;
using tinydbms::compiler::DeletePlan;
using tinydbms::compiler::Expr;
using tinydbms::compiler::ExprPtr;
using tinydbms::compiler::FilterNode;
using tinydbms::compiler::InsertPlan;
using tinydbms::compiler::JoinKind;
using tinydbms::compiler::JoinNode;
using tinydbms::compiler::Literal;
using tinydbms::compiler::LogicOp;
using tinydbms::compiler::NullTestOp;
using tinydbms::compiler::NullTest;
using tinydbms::compiler::Plan;
using tinydbms::compiler::PlanNode;
using tinydbms::compiler::ProjectNode;
using tinydbms::compiler::QueryOutput;
using tinydbms::compiler::QueryPlan;
using tinydbms::compiler::ScanColumn;
using tinydbms::compiler::SeqScanNode;
using tinydbms::compiler::SortDirection;
using tinydbms::compiler::SortKey;
using tinydbms::compiler::SortNode;
using tinydbms::compiler::Unary;
using tinydbms::compiler::UnaryOp;
using tinydbms::compiler::UpdateAssignment;
using tinydbms::compiler::UpdatePlan;
using tinydbms::core::CommandResult;
using tinydbms::core::Database;
using tinydbms::core::Error;
using tinydbms::core::ErrorKind;
using tinydbms::core::ExecuteScriptRequest;
using tinydbms::core::ExecutionMode;
using tinydbms::core::QueryResult;
using tinydbms::core::ScriptErrorPolicy;
using tinydbms::core::StatementStatus;
namespace fake = tinydbms::testing::fake_storage;
namespace fake_compiler = tinydbms::testing::fake_compiler;

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " #condition \
                      << '\n';                                                              \
            return false;                                                                    \
        }                                                                                    \
    } while (false)

bool check_lines(
    const std::vector<std::string>& actual,
    const std::vector<std::string>& expected) {
    if (actual == expected) {
        return true;
    }
    std::cerr << "plan text mismatch\n--- actual ---\n";
    for (const std::string& line : actual) {
        std::cerr << line << '\n';
    }
    std::cerr << "--- expected ---\n";
    for (const std::string& line : expected) {
        std::cerr << line << '\n';
    }
    return false;
}

std::vector<std::string> render_lines(
    const Plan& plan,
    const std::vector<TableMeta>& catalog) {
    const std::variant<QueryResult, Error> rendered =
        tinydbms::core::internal::render_plan_result(plan, catalog);
    if (const Error* error = std::get_if<Error>(&rendered); error != nullptr) {
        std::cerr << "plan rendering failed: " << error->message << '\n';
        return {};
    }
    std::vector<std::string> lines;
    for (const auto& row : std::get<QueryResult>(rendered).rows) {
        lines.push_back(std::get<std::string>(row.front().data));
    }
    return lines;
}

std::optional<Error> render_error(
    const Plan& plan,
    const std::vector<TableMeta>& catalog) {
    const std::variant<QueryResult, Error> rendered =
        tinydbms::core::internal::render_plan_result(plan, catalog);
    if (const Error* error = std::get_if<Error>(&rendered); error != nullptr) {
        return *error;
    }
    return std::nullopt;
}

Expr column(SlotId slot_id) {
    return Expr{ColumnRef{slot_id}};
}

Expr int_literal(std::int32_t value) {
    return Expr{Literal{Value{value}}};
}

Expr bigint_literal(std::int64_t value) {
    return Expr{Literal{Value{value}}};
}

Expr double_literal(double value) {
    return Expr{Literal{Value{value}}};
}

Expr bool_literal(bool value) {
    return Expr{Literal{Value{value}}};
}

Expr null_literal() {
    return Expr{Literal{Value{std::monostate{}}}};
}

Expr text_literal(std::string value) {
    return Expr{Literal{Value{std::move(value)}}};
}

ExprPtr owned(Expr value) {
    return std::make_unique<Expr>(std::move(value));
}

Expr compare(CmpOp op, Expr lhs, Expr rhs) {
    return Expr{tinydbms::compiler::Binary{op, owned(std::move(lhs)), owned(std::move(rhs))}};
}

Expr logic(LogicOp op, Expr lhs, Expr rhs) {
    return Expr{tinydbms::compiler::Binary{op, owned(std::move(lhs)), owned(std::move(rhs))}};
}

Expr logical_not(Expr operand) {
    return Expr{Unary{owned(std::move(operand))}};
}

Expr null_test(NullTestOp op, Expr operand) {
    return Expr{NullTest{op, owned(std::move(operand))}};
}

std::vector<TableMeta> users_catalog() {
    return {TableMeta{
        0,
        "users",
        {{"id", Type::kInt, false}, {"name", Type::kVarchar, true}}}};
}

std::vector<ScanColumn> users_columns() {
    return {ScanColumn{0, 0}, ScanColumn{1, 1}};
}

bool test_statement_plans() {
    const std::vector<TableMeta> catalog = users_catalog();

    const Plan create_table{CreateTablePlan{"users", catalog.front().columns}};
    CHECK(check_lines(
        render_lines(create_table, catalog),
        {"CreateTable users(id INT NOT NULL, name VARCHAR)"}));

    const Plan insert_all{InsertPlan{
        0,
        {},
        {{Value{std::int32_t{1}}, Value{std::string{"a"}}}}}};
    CHECK(check_lines(
        render_lines(insert_all, catalog),
        {"Insert table=users columns=[*] rows=1"}));

    const Plan insert_columns{InsertPlan{
        0,
        {1},
        {{Value{std::string{"a"}}}, {Value{std::string{"b"}}}}}};
    CHECK(check_lines(
        render_lines(insert_columns, catalog),
        {"Insert table=users columns=[name] rows=2"}));

    const Plan delete_all{DeletePlan{0, users_columns(), std::nullopt}};
    CHECK(check_lines(render_lines(delete_all, catalog), {"Delete table=users predicate=ALL"}));

    const Plan delete_one{DeletePlan{
        0,
        users_columns(),
        compare(CmpOp::kEq, column(0), int_literal(1))}};
    CHECK(check_lines(
        render_lines(delete_one, catalog),
        {"Delete table=users predicate=(id#0 = 1)"}));

    const Plan update{UpdatePlan{
        0,
        users_columns(),
        {{1, Value{std::string{"x"}}}},
        std::nullopt}};
    CHECK(check_lines(
        render_lines(update, catalog),
        {"Update table=users assignments=[name = 'x'] predicate=ALL"}));

    const Plan update_many{UpdatePlan{
        0,
        users_columns(),
        {{0, Value{std::int32_t{7}}}, {1, Value{std::monostate{}}}},
        compare(CmpOp::kGe, column(1), text_literal("a'b"))}};
    CHECK(check_lines(
        render_lines(update_many, catalog),
        {"Update table=users assignments=[id = 7, name = NULL] predicate=(name#1 >= 'a''b')"}));
    return true;
}

bool test_query_plan_nodes() {
    const std::vector<TableMeta> catalog = users_catalog();

    std::unique_ptr<PlanNode> scan =
        std::make_unique<PlanNode>(SeqScanNode{0, users_columns()});
    std::unique_ptr<PlanNode> filter = std::make_unique<PlanNode>(FilterNode{
        compare(CmpOp::kGt, column(0), int_literal(10)),
        std::move(scan)});
    const Plan query{QueryPlan{
        std::make_unique<PlanNode>(ProjectNode{{0, 1}, std::move(filter)}),
        {QueryOutput{0, "id", Type::kInt, false},
         QueryOutput{1, "name", Type::kVarchar, true}}}};

    CHECK(check_lines(
        render_lines(query, catalog),
        {
            "QueryPlan outputs=[id:INT, name:VARCHAR]",
            "  Project [id#0, name#1]",
            "    Filter (id#0 > 10)",
            "      SeqScan users",
            "        columns=[id -> slot0, name -> slot1]",
        }));

    // Sort 与 Aggregate 的键使用原始 slot 形式，聚合输入走表达式渲染。
    const std::vector<TableMeta> orders_catalog{
        TableMeta{3, "orders", {{"dept", Type::kInt, true}, {"amount", Type::kBigInt, true}}}};
    std::unique_ptr<PlanNode> orders_scan = std::make_unique<PlanNode>(
        SeqScanNode{3, {ScanColumn{0, 0}, ScanColumn{1, 1}}});
    std::unique_ptr<PlanNode> aggregate = std::make_unique<PlanNode>(AggregateNode{
        {0},
        {AggregateCall{AggregateKind::kSum, SlotId{1}, SlotId{2}, Type::kBigInt, true}},
        std::move(orders_scan)});
    std::unique_ptr<PlanNode> sort =
        std::make_unique<PlanNode>(SortNode{{{2, SortDirection::kDesc}}, std::move(aggregate)});
    const Plan grouped{QueryPlan{
        std::make_unique<PlanNode>(ProjectNode{{0, 2}, std::move(sort)}),
        {QueryOutput{0, "dept", Type::kInt, true},
         QueryOutput{2, "SUM(amount)", Type::kBigInt, true}}}};

    CHECK(check_lines(
        render_lines(grouped, orders_catalog),
        {
            "QueryPlan outputs=[dept:INT, SUM(amount):BIGINT]",
            "  Project [dept#0, SUM(amount)#2]",
            "    Sort keys=[slot2 DESC]",
            "      Aggregate group=[slot0] calls=[SUM(amount#1) -> slot2 BIGINT]",
            "        SeqScan orders",
            "          columns=[dept -> slot0, amount -> slot1]",
        }));

    // 多表场景列名带表名限定；COUNT(*) 的输入为 *。
    const std::vector<TableMeta> join_catalog{
        TableMeta{0, "dept", {{"id", Type::kInt, true}, {"name", Type::kVarchar, true}}},
        TableMeta{1, "emp", {{"id", Type::kInt, true}, {"dept", Type::kInt, true}}}};
    std::unique_ptr<PlanNode> left =
        std::make_unique<PlanNode>(SeqScanNode{0, {ScanColumn{0, 0}, ScanColumn{1, 1}}});
    std::unique_ptr<PlanNode> right =
        std::make_unique<PlanNode>(SeqScanNode{1, {ScanColumn{0, 2}, ScanColumn{1, 3}}});
    std::unique_ptr<PlanNode> join = std::make_unique<PlanNode>(JoinNode{
        JoinKind::kInner,
        compare(CmpOp::kEq, column(0), column(3)),
        std::move(left),
        std::move(right)});
    const Plan joined{QueryPlan{
        std::make_unique<PlanNode>(ProjectNode{{1, 3}, std::move(join)}),
        {QueryOutput{1, "name", Type::kVarchar, true},
         QueryOutput{3, "dept", Type::kInt, true}}}};

    CHECK(check_lines(
        render_lines(joined, join_catalog),
        {
            "QueryPlan outputs=[name:VARCHAR, dept:INT]",
            "  Project [dept.name#1, emp.dept#3]",
            "    Join kind=INNER condition=(dept.id#0 = emp.dept#3)",
            "      left:",
            "        SeqScan dept",
            "          columns=[id -> slot0, name -> slot1]",
            "      right:",
            "        SeqScan emp",
            "          columns=[id -> slot2, dept -> slot3]",
        }));

    std::unique_ptr<PlanNode> count_scan =
        std::make_unique<PlanNode>(SeqScanNode{1, {ScanColumn{0, 2}, ScanColumn{1, 3}}});
    std::unique_ptr<PlanNode> count_aggregate = std::make_unique<PlanNode>(AggregateNode{
        {},
        {AggregateCall{AggregateKind::kCount, std::nullopt, SlotId{4}, Type::kBigInt, false}},
        std::move(count_scan)});
    const Plan counted{QueryPlan{
        std::make_unique<PlanNode>(ProjectNode{{4}, std::move(count_aggregate)}),
        {QueryOutput{4, "COUNT(*)", Type::kBigInt, false}}}};
    CHECK(check_lines(
        render_lines(counted, join_catalog),
        {
            "QueryPlan outputs=[COUNT(*):BIGINT]",
            "  Project [COUNT(*)#4]",
            "    Aggregate group=[] calls=[COUNT(*) -> slot4 BIGINT]",
            "      SeqScan emp",
            "        columns=[id -> slot2, dept -> slot3]",
        }));
    return true;
}

bool test_expression_rendering() {
    const std::vector<TableMeta> catalog = users_catalog();

    const Plan null_test_plan{DeletePlan{
        0,
        users_columns(),
        logic(
            LogicOp::kOr,
            null_test(NullTestOp::kIsNull, column(1)),
            null_test(NullTestOp::kIsNotNull, column(1)))}};
    CHECK(check_lines(
        render_lines(null_test_plan, catalog),
        {"Delete table=users predicate=((name#1 IS NULL) OR (name#1 IS NOT NULL))"}));

    const Plan negation{DeletePlan{
        0,
        users_columns(),
        logical_not(compare(CmpOp::kNe, column(0), int_literal(-3)))}};
    CHECK(check_lines(
        render_lines(negation, catalog),
        {"Delete table=users predicate=NOT ((id#0 <> -3))"}));

    const std::vector<TableMeta> typed_catalog{TableMeta{
        0,
        "t",
        {{"flag", Type::kBoolean, true},
         {"score", Type::kDouble, true},
         {"amount", Type::kBigInt, true}}}};
    const std::vector<ScanColumn> typed_columns{
        ScanColumn{0, 0}, ScanColumn{1, 1}, ScanColumn{2, 2}};
    const Plan literals{DeletePlan{
        0,
        typed_columns,
        logic(
            LogicOp::kAnd,
            compare(CmpOp::kEq, column(0), bool_literal(true)),
            logic(
                LogicOp::kOr,
                compare(CmpOp::kLe, column(1), double_literal(1.5)),
                compare(CmpOp::kGe, column(2), bigint_literal(9007199254740993LL))))}};
    CHECK(check_lines(
        render_lines(literals, typed_catalog),
        {
            "Delete table=t predicate=((flag#0 = TRUE) AND ((score#1 <= 1.5) OR "
            "(amount#2 >= 9007199254740993)))",
        }));

    const Plan null_comparison{DeletePlan{
        0,
        users_columns(),
        compare(CmpOp::kEq, column(1), null_literal())}};
    CHECK(check_lines(
        render_lines(null_comparison, catalog),
        {"Delete table=users predicate=(name#1 = NULL)"}));
    return true;
}

bool test_unresolved_names_fall_back_to_numbers() {
    // 表不在 catalog 里：表名退化成 table#N，列名退化成 slot#N。
    const Plan missing_table{DeletePlan{
        9,
        {ScanColumn{0, 0}},
        compare(CmpOp::kEq, column(0), int_literal(1))}};
    CHECK(check_lines(
        render_lines(missing_table, users_catalog()),
        {"Delete table=table#9 predicate=(slot#0 = 1)"}));

    // 列号越界：columns 行退化成 column#N，表达式槽位退化成 slot#N。
    const Plan bad_column{DeletePlan{
        0,
        {ScanColumn{7, 5}},
        compare(CmpOp::kEq, column(5), int_literal(1))}};
    CHECK(check_lines(
        render_lines(bad_column, users_catalog()),
        {"Delete table=users predicate=(slot#5 = 1)"}));

    std::unique_ptr<PlanNode> scan =
        std::make_unique<PlanNode>(SeqScanNode{9, {ScanColumn{0, 0}, ScanColumn{7, 1}}});
    const Plan query{QueryPlan{
        std::make_unique<PlanNode>(ProjectNode{{0, 1}, std::move(scan)}),
        {QueryOutput{0, "a", Type::kInt, true}}}};
    CHECK(check_lines(
        render_lines(query, users_catalog()),
        {
            "QueryPlan outputs=[a:INT]",
            "  Project [slot#0, slot#1]",
            "    SeqScan table#9",
            "      columns=[column#0 -> slot0, column#7 -> slot1]",
        }));
    return true;
}

bool test_invalid_plans_are_rejected() {
    const std::vector<TableMeta> catalog = users_catalog();

    const Plan missing_root{QueryPlan{nullptr, {QueryOutput{0, "a", Type::kInt, true}}}};
    const std::optional<Error> root_error = render_error(missing_root, catalog);
    CHECK(root_error.has_value());
    CHECK(root_error->kind == ErrorKind::kInternal);

    std::unique_ptr<PlanNode> filter =
        std::make_unique<PlanNode>(FilterNode{compare(CmpOp::kEq, column(0), int_literal(1)), nullptr});
    const Plan missing_child{QueryPlan{
        std::make_unique<PlanNode>(ProjectNode{{0}, std::move(filter)}),
        {QueryOutput{0, "a", Type::kInt, true}}}};
    const std::optional<Error> child_error = render_error(missing_child, catalog);
    CHECK(child_error.has_value());
    CHECK(child_error->kind == ErrorKind::kInternal);

    Expr missing_operand{NullTest{NullTestOp::kIsNull, nullptr}};
    const Plan broken_expression{DeletePlan{0, users_columns(), std::move(missing_operand)}};
    const std::optional<Error> expression_error = render_error(broken_expression, catalog);
    CHECK(expression_error.has_value());
    CHECK(expression_error->kind == ErrorKind::kInternal);

    // 超过核心深度上限的 Plan 以 kInternal 结束，不靠递归打穿栈。
    std::unique_ptr<PlanNode> deep =
        std::make_unique<PlanNode>(SeqScanNode{0, users_columns()});
    for (std::size_t index = 0; index < 300U; ++index) {
        deep = std::make_unique<PlanNode>(ProjectNode{{0}, std::move(deep)});
    }
    const Plan deep_plan{QueryPlan{
        std::move(deep),
        {QueryOutput{0, "id", Type::kInt, false}}}};
    const std::optional<Error> depth_error = render_error(deep_plan, catalog);
    CHECK(depth_error.has_value());
    CHECK(depth_error->kind == ErrorKind::kInternal);
    return true;
}

bool open_database(Database& database) {
    const auto result = database.open(tinydbms::core::OpenDatabaseRequest{"test-data"});
    return !result.error.has_value();
}

bool test_plan_mode_skips_storage_and_carries_result() {
    fake::reset();
    fake_compiler::reset();
    std::deque<CompileResult> results;
    results.emplace_back(Plan{CreateTablePlan{
        "events",
        std::vector<ColumnMeta>{{"id", Type::kInt}}}});
    fake_compiler::set_compile_results(std::move(results));

    Database database;
    CHECK(open_database(database));
    const std::size_t create_calls_before = fake::state().create_table_calls;
    const std::size_t open_table_calls_before = fake::state().open_table_calls;

    ExecuteScriptRequest request{"create table events (id int);"};
    request.mode = ExecutionMode::kPlanOnly;
    const auto script = database.execute_script(request);

    CHECK(!script.script_error.has_value());
    CHECK(script.statements.size() == 1);
    CHECK(script.statements.front().status() == StatementStatus::kPlanOnly);
    CHECK(fake::state().create_table_calls == create_calls_before);
    CHECK(fake::state().open_table_calls == open_table_calls_before);

    const auto& outcome = *script.statements.front().outcome();
    const QueryResult* plan = std::get_if<QueryResult>(&outcome.outcome);
    CHECK(plan != nullptr);
    CHECK(plan->columns.size() == 1);
    CHECK(plan->columns.front().name == "plan");
    CHECK(plan->columns.front().type == Type::kVarchar);
    CHECK(plan->rows.size() == 1);
    CHECK(std::get<std::string>(plan->rows.front().front().data) ==
          "CreateTable events(id INT NOT NULL)");

    const auto closed = database.close();
    CHECK(!closed.error.has_value());
    return true;
}

bool test_plan_mode_reports_compile_errors_identically() {
    const std::string text = "create table events (id int);";
    const std::string segment = fake_compiler::statement_segment(text, 0);

    const auto run = [&](const ExecutionMode mode) {
        fake::reset();
        fake_compiler::reset();
        std::deque<CompileResult> results;
        results.emplace_back(fake_compiler::make_compile_error(
            CompileStage::kSyntax,
            segment,
            1,
            7,
            "injected syntax error"));
        fake_compiler::set_compile_results(std::move(results));

        Database database;
        if (!open_database(database)) {
            return tinydbms::core::ExecuteScriptResult{};
        }
        ExecuteScriptRequest request{text, ScriptErrorPolicy::kStopOnFirstError};
        request.mode = mode;
        return database.execute_script(request);
    };

    const auto executed = run(ExecutionMode::kExecute);
    const auto planned = run(ExecutionMode::kPlanOnly);
    CHECK(executed.statements.size() == 1);
    CHECK(planned.statements.size() == 1);
    CHECK(executed.statements.front().status() == StatementStatus::kCompileError);
    CHECK(planned.statements.front().status() == StatementStatus::kCompileError);
    CHECK(executed.statements.front().source().begin.line ==
          planned.statements.front().source().begin.line);
    const auto& executed_error = *executed.statements.front().outcome();
    const auto& planned_error = *planned.statements.front().outcome();
    const Error* left = std::get_if<Error>(&executed_error.outcome);
    const Error* right = std::get_if<Error>(&planned_error.outcome);
    CHECK(left != nullptr && right != nullptr);
    CHECK(left->kind == right->kind);
    CHECK(left->message == right->message);
    CHECK(left->source.has_value() && right->source.has_value());
    CHECK(left->source->begin.byte_offset == right->source->begin.byte_offset);
    CHECK(left->source->end.byte_offset == right->source->end.byte_offset);
    return true;
}

bool test_plan_mode_with_analyze_policy_keeps_planning() {
    // 计划模式不进入执行器，因此首错之后编译成功的语句仍然产出计划（kPlanOnly），
    // 不会变成 kAnalysisOnly；首错本身与执行模式一样记 kCompileError。
    const std::string text =
        "create table broken (id int);\n"
        "create table events (id int);";
    const std::string first_segment = fake_compiler::statement_segment(text, 0);

    fake::reset();
    fake_compiler::reset();
    std::deque<CompileResult> results;
    results.emplace_back(fake_compiler::make_compile_error(
        CompileStage::kSyntax,
        first_segment,
        1,
        7,
        "injected syntax error"));
    results.emplace_back(Plan{CreateTablePlan{
        "events",
        std::vector<ColumnMeta>{{"id", Type::kInt}}}});
    fake_compiler::set_compile_results(std::move(results));

    Database database;
    CHECK(open_database(database));
    const std::size_t create_calls_before = fake::state().create_table_calls;

    ExecuteScriptRequest request{text, ScriptErrorPolicy::kAnalyzeRemaining};
    request.mode = ExecutionMode::kPlanOnly;
    const auto script = database.execute_script(request);

    CHECK(!script.script_error.has_value());
    CHECK(script.statements.size() == 2);
    CHECK(script.statements[0].status() == StatementStatus::kCompileError);
    CHECK(script.statements[1].status() == StatementStatus::kPlanOnly);
    CHECK(fake::state().create_table_calls == create_calls_before);
    const auto* plan = std::get_if<QueryResult>(&script.statements[1].outcome()->outcome);
    CHECK(plan != nullptr);
    CHECK(std::get<std::string>(plan->rows.front().front().data) ==
          "CreateTable events(id INT NOT NULL)");

    const auto closed = database.close();
    CHECK(!closed.error.has_value());
    return true;
}

}  // namespace

int main() {
    const bool passed =
        test_statement_plans() &&
        test_query_plan_nodes() &&
        test_expression_rendering() &&
        test_unresolved_names_fall_back_to_numbers() &&
        test_invalid_plans_are_rejected() &&
        test_plan_mode_skips_storage_and_carries_result() &&
        test_plan_mode_reports_compile_errors_identically() &&
        test_plan_mode_with_analyze_policy_keeps_planning();
    return passed ? 0 : 1;
}
