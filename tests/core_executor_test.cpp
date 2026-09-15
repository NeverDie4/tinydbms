#include "fakes/compiler_fake.hpp"
#include "fakes/result_helpers.hpp"
#include "fakes/storage_fake.hpp"
#include "tinydbms/core.hpp"
#include "../src/core/expression.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using tinydbms::ColumnId;
using tinydbms::ColumnMeta;
using tinydbms::SlotId;
using tinydbms::TableId;
using tinydbms::TableMeta;
using tinydbms::Type;
using tinydbms::Value;
using tinydbms::compiler::Binary;
using tinydbms::compiler::AggregateCall;
using tinydbms::compiler::AggregateKind;
using tinydbms::compiler::AggregateNode;
using tinydbms::compiler::CmpOp;
using tinydbms::compiler::ColumnRef;
using tinydbms::compiler::CompileResult;
using tinydbms::compiler::DeletePlan;
using tinydbms::compiler::Expr;
using tinydbms::compiler::InsertPlan;
using tinydbms::compiler::JoinKind;
using tinydbms::compiler::JoinNode;
using tinydbms::compiler::LogicOp;
using tinydbms::compiler::Literal;
using tinydbms::compiler::NullTest;
using tinydbms::compiler::NullTestOp;
using tinydbms::compiler::Plan;
using tinydbms::compiler::PlanNode;
using tinydbms::compiler::ProjectNode;
using tinydbms::compiler::QueryPlan;
using tinydbms::compiler::QueryOutput;
using tinydbms::compiler::ScanColumn;
using tinydbms::compiler::SeqScanNode;
using tinydbms::compiler::SortDirection;
using tinydbms::compiler::SortKey;
using tinydbms::compiler::SortNode;
using tinydbms::compiler::UpdateAssignment;
using tinydbms::compiler::UpdatePlan;
using tinydbms::core::CommandResult;
using tinydbms::core::Database;
using tinydbms::core::Error;
using tinydbms::core::ErrorKind;
using tinydbms::core::ExecuteResult;
using tinydbms::core::ExecuteScriptRequest;
using tinydbms::core::ExecuteScriptResult;
using tinydbms::core::QueryResult;
namespace fake = tinydbms::testing::fake_storage;
namespace fake_compiler = tinydbms::testing::fake_compiler;
namespace expression = tinydbms::core::internal::expression;

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " #condition \
                      << '\n';                                                              \
            return false;                                                                    \
        }                                                                                    \
    } while (false)

TableMeta users_table() {
    return TableMeta{
        1,
        "users",
        std::vector<ColumnMeta>{
            {"id", Type::kInt},
            {"name", Type::kVarchar},
            {"age", Type::kInt}}};
}

TableMeta events_table() {
    return TableMeta{
        2,
        "events",
        std::vector<ColumnMeta>{{"event_id", Type::kBigInt}}};
}

TableMeta memberships_table() {
    return TableMeta{
        5,
        "memberships",
        std::vector<ColumnMeta>{
            {"user_id", Type::kInt, true},
            {"label", Type::kVarchar},
            {"active", Type::kBoolean, true}}};
}

TableMeta measurements_table() {
    return TableMeta{
        3,
        "measurements",
        std::vector<ColumnMeta>{{"value", Type::kDouble}}};
}

TableMeta flags_table() {
    return TableMeta{
        4,
        "flags",
        std::vector<ColumnMeta>{{"active", Type::kBoolean}}};
}

Value int_value(std::int32_t value) {
    return Value{value};
}

Value text_value(std::string value) {
    return Value{std::move(value)};
}

tinydbms::storage::Record record(
    std::uint64_t rid,
    std::vector<Value> values) {
    return tinydbms::storage::Record{
        tinydbms::storage::RecordId{rid},
        std::move(values)};
}

Expr column(SlotId slot_id) {
    return Expr{ColumnRef{slot_id}};
}

Expr literal(Value value) {
    return Expr{Literal{std::move(value)}};
}

Expr compare(CmpOp op, Expr lhs, Expr rhs) {
    return Expr{Binary{
        op,
        std::make_unique<Expr>(std::move(lhs)),
        std::make_unique<Expr>(std::move(rhs))}};
}

Expr logic(LogicOp op, Expr lhs, Expr rhs) {
    return Expr{Binary{
        op,
        std::make_unique<Expr>(std::move(lhs)),
        std::make_unique<Expr>(std::move(rhs))}};
}

Expr logical_not(Expr operand) {
    return Expr{tinydbms::compiler::Unary{
        std::make_unique<Expr>(std::move(operand))}};
}

Expr null_test(NullTestOp op, Expr operand) {
    return Expr{NullTest{op, std::make_unique<Expr>(std::move(operand))}};
}

Expr deeply_nested_predicate(std::size_t depth) {
    Expr predicate = compare(CmpOp::kEq, column(0), literal(int_value(1)));
    for (std::size_t index = 0; index < depth; ++index) {
        predicate = logical_not(std::move(predicate));
    }
    return predicate;
}

std::vector<ScanColumn> default_scan_columns() {
    return {{0, 0}, {1, 1}, {2, 2}};
}

std::vector<QueryOutput> default_query_outputs() {
    return {
        {0, "id", Type::kInt, false},
        {1, "name", Type::kVarchar, false},
        {2, "age", Type::kInt, false}};
}

std::unique_ptr<PlanNode> scan(
    TableId table_id = 1,
    std::vector<ScanColumn> columns = default_scan_columns()) {
    return std::make_unique<PlanNode>(SeqScanNode{table_id, std::move(columns)});
}

Plan query(
    std::unique_ptr<PlanNode> root,
    std::vector<QueryOutput> outputs = default_query_outputs()) {
    return Plan{QueryPlan{std::move(root), std::move(outputs)}};
}

Plan insert_plan(
    std::vector<ColumnId> columns,
    std::vector<std::vector<Value>> rows,
    TableId table_id = 1) {
    return Plan{InsertPlan{table_id, std::move(columns), std::move(rows)}};
}

Plan delete_plan(
    TableId table_id,
    std::optional<Expr> predicate = std::nullopt,
    std::vector<ScanColumn> input_columns = default_scan_columns()) {
    return Plan{DeletePlan{table_id, std::move(input_columns), std::move(predicate)}};
}

Plan update_plan(
    std::vector<UpdateAssignment> assignments,
    std::optional<Expr> predicate = std::nullopt,
    std::vector<ScanColumn> input_columns = default_scan_columns()) {
    return Plan{UpdatePlan{1,std::move(input_columns),std::move(assignments),std::move(predicate)}};
}

bool open_database(Database& database) {
    const auto result = database.open(tinydbms::core::OpenDatabaseRequest{"test-data"});
    return !result.error.has_value();
}

bool close_database(Database& database) {
    return !database.close().error.has_value();
}

bool start_database(
    Database& database,
    std::vector<tinydbms::storage::Record> records = {}) {
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({users_table()});
    fake::set_records_for_table(users_table().table_id, std::move(records));
    return open_database(database);
}

ExecuteScriptResult execute_plan(Database& database, Plan plan) {
    fake_compiler::reset();
    std::deque<CompileResult> results;
    results.emplace_back(std::move(plan));
    fake_compiler::set_compile_results(std::move(results));
    return database.execute_script(ExecuteScriptRequest{"synthetic statement;"});
}

ExecuteScriptResult execute_plans(
    Database& database,
    std::deque<CompileResult> results) {
    fake_compiler::reset();
    std::string script;
    for (std::size_t index = 0; index < results.size(); ++index) {
        script += "statement_" + std::to_string(index) + ';';
    }
    fake_compiler::set_compile_results(std::move(results));
    return database.execute_script(ExecuteScriptRequest{std::move(script)});
}

const Error* error_of(const ExecuteResult& result) {
    return std::get_if<Error>(&result.outcome);
}

const CommandResult* command_of(const ExecuteResult& result) {
    return std::get_if<CommandResult>(&result.outcome);
}

const QueryResult* query_of(const ExecuteResult& result) {
    return std::get_if<QueryResult>(&result.outcome);
}

const ExecuteResult& outcome_of(const tinydbms::core::StatementResult& statement) {
    return *statement.outcome();
}

const CommandResult* command_of(const tinydbms::core::StatementResult& statement) {
    return command_of(outcome_of(statement));
}

const QueryResult* query_of(const tinydbms::core::StatementResult& statement) {
    return query_of(outcome_of(statement));
}

bool is_error(const ExecuteResult& result, ErrorKind kind) {
    const Error* error = error_of(result);
    return error != nullptr && error->kind == kind;
}

bool is_error(const tinydbms::core::StatementResult& statement, ErrorKind kind) {
    return is_error(outcome_of(statement), kind);
}

bool script_error_is(const ExecuteScriptResult& script, ErrorKind kind) {
    return script.script_error.has_value() && script.script_error->kind == kind;
}

// 契约要求 kInternal 只出现在 script_error，不允许出现在语句级 outcome。
// 执行器返回 kInternal 属于致命中止：未调用 storage 时当前语句记 kSkippedExecution，
// 已调用 storage 或进入 cleanup-pending 时记 kExecutionIndeterminate，其余全部跳过。
bool internal_script_abort(const ExecuteScriptResult& script, std::size_t index = 0) {
    if (!script_error_is(script, ErrorKind::kInternal) ||
        index >= script.statements.size()) {
        return false;
    }
    const tinydbms::core::StatementStatus status = script.statements[index].status();
    return status == tinydbms::core::StatementStatus::kSkippedExecution ||
        status == tinydbms::core::StatementStatus::kExecutionIndeterminate;
}

// 执行器未调用 storage 就返回 kInternal：当前语句起全部 skipped。
bool internal_abort_before_storage(
    const ExecuteScriptResult& script,
    std::size_t index = 0) {
    return script_error_is(script, ErrorKind::kInternal) &&
        index < script.statements.size() &&
        script.statements[index].status() ==
            tinydbms::core::StatementStatus::kSkippedExecution;
}

// 已调用 storage（或进入 cleanup-pending）后的致命中止：物理状态无法确认。
bool internal_abort_after_storage(
    const ExecuteScriptResult& script,
    std::size_t index = 0) {
    return script_error_is(script, ErrorKind::kInternal) &&
        index < script.statements.size() &&
        script.statements[index].status() ==
            tinydbms::core::StatementStatus::kExecutionIndeterminate;
}

bool test_insert_reorders_rows_before_storage() {
    Database database;
    CHECK(start_database(database));

    const auto script = execute_plan(
        database,
        insert_plan(
            {2, 0, 1},
            {{int_value(30), int_value(7), text_value("alice")}}));
    CHECK(script.statements.size() == 1);
    const CommandResult* command = command_of(script.statements.front());
    CHECK(command != nullptr);
    CHECK(command->affected_rows == 1);
    CHECK(!command->error.has_value());
    CHECK(fake::state().insert_calls == 1);
    CHECK(fake::state().last_insert_request.has_value());
    const auto& rows = fake::state().last_insert_request->rows;
    CHECK(rows.size() == 1);
    CHECK(rows[0].size() == 3);
    CHECK(std::get<std::int32_t>(rows[0][0].data) == 7);
    CHECK(std::get<std::string>(rows[0][1].data) == "alice");
    CHECK(std::get<std::int32_t>(rows[0][2].data) == 30);
    CHECK(close_database(database));
    return true;
}

bool run_invalid_insert(Plan plan) {
    Database database;
    CHECK(start_database(database));
    const auto script = execute_plan(database, std::move(plan));
    CHECK(script.statements.size() == 1);
    CHECK(internal_abort_before_storage(script));
    CHECK(fake::state().insert_calls == 0);
    CHECK(close_database(database));
    return true;
}

bool test_insert_prevalidation_has_no_storage_side_effect() {
    CHECK(run_invalid_insert(insert_plan({}, {})));
    CHECK(run_invalid_insert(insert_plan({0, 0, 1}, {
        {int_value(1), text_value("alice"), int_value(20)}})));
    CHECK(run_invalid_insert(insert_plan({0, 1}, {
        {int_value(1), text_value("alice")}})));
    CHECK(run_invalid_insert(insert_plan({0, 1, 3}, {
        {int_value(1), text_value("alice"), int_value(20)}})));
    CHECK(run_invalid_insert(insert_plan({}, {
        {text_value("wrong"), text_value("alice"), int_value(20)}})));
    CHECK(run_invalid_insert(insert_plan({}, {
        {int_value(1), text_value("alice"), int_value(20)}}, 99)));
    return true;
}

bool test_bigint_validation_and_mixed_integer_comparison() {
    Database valid_database;
    fake::reset();fake_compiler::reset();fake::set_tables({events_table()});
    CHECK(open_database(valid_database));
    const auto valid = execute_plan(
        valid_database,
        insert_plan({},{{Value{std::int64_t{2147483648LL}}}},2));
    CHECK(valid.statements.size()==1);
    CHECK(command_of(valid.statements.front())!=nullptr);
    CHECK(fake::state().insert_calls==1);
    CHECK(std::get<std::int64_t>(
        fake::state().last_insert_request->rows[0][0].data)==2147483648LL);
    CHECK(close_database(valid_database));

    Database invalid_database;
    fake::reset();fake_compiler::reset();fake::set_tables({events_table()});
    CHECK(open_database(invalid_database));
    const auto invalid = execute_plan(
        invalid_database,
        insert_plan({},{{Value{std::int32_t{1}}}},2));
    CHECK(invalid.statements.size()==1);
    CHECK(internal_script_abort(invalid));
    CHECK(fake::state().insert_calls==0);
    CHECK(close_database(invalid_database));

    const expression::SlotRow row{{7,Value{std::int64_t{2147483648LL}}}};
    const auto greater = expression::evaluate(
        compare(CmpOp::kGt,column(7),literal(Value{std::int32_t{1}})),row);
    const Value* greater_value=std::get_if<Value>(&greater);
    CHECK(greater_value!=nullptr && std::get<bool>(greater_value->data));
    const auto reverse = expression::evaluate(
        compare(CmpOp::kLt,literal(Value{std::int32_t{1}}),column(7)),row);
    const Value* reverse_value=std::get_if<Value>(&reverse);
    CHECK(reverse_value!=nullptr && std::get<bool>(reverse_value->data));
    return true;
}

bool test_double_runtime_comparison() {
    const expression::SlotRow row{
        {3, Value{1.5}},
        {7, Value{std::int32_t{1}}},
        {11, Value{std::int64_t{2147483648LL}}},
        {13, Value{std::int64_t{9007199254740993LL}}}};
    const auto is_true = [&row](Expr predicate) {
        const auto result = expression::evaluate(std::move(predicate), row);
        const Value* value = std::get_if<Value>(&result);
        return value != nullptr && std::holds_alternative<bool>(value->data) &&
            std::get<bool>(value->data);
    };

    CHECK(is_true(compare(CmpOp::kEq, column(3), literal(Value{1.5}))));
    CHECK(is_true(compare(CmpOp::kLt, column(7), column(3))));
    CHECK(is_true(compare(CmpOp::kGt, column(3), column(7))));
    CHECK(is_true(compare(CmpOp::kLt, column(11), literal(Value{2147483648.5}))));
    CHECK(is_true(compare(CmpOp::kGt, literal(Value{2147483648.5}), column(11))));
    CHECK(is_true(compare(
        CmpOp::kEq,
        column(13),
        literal(Value{9007199254740992.0}))));
    return true;
}

bool test_double_physical_validation() {
    Database database;
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({measurements_table()});
    CHECK(open_database(database));

    const auto valid = execute_plan(database, insert_plan({}, {{Value{12.5}}}, 3));
    CHECK(valid.statements.size() == 1);
    CHECK(command_of(valid.statements.front()) != nullptr);
    CHECK(fake::state().insert_calls == 1);
    CHECK(std::get<double>(fake::state().last_insert_request->rows[0][0].data) == 12.5);

    for (Value invalid : {Value{std::int32_t{1}}, Value{std::int64_t{1}}}) {
        const auto result = execute_plan(
            database,
            insert_plan({}, {{std::move(invalid)}}, 3));
        CHECK(result.statements.size() == 1);
        CHECK(internal_script_abort(result));
        CHECK(fake::state().insert_calls == 1);
    }
    CHECK(close_database(database));
    return true;
}

bool test_boolean_runtime_comparison() {
    const expression::SlotRow row{
        {3, Value{true}},
        {7, Value{false}}};
    const auto equals = expression::evaluate(
        compare(CmpOp::kEq, column(3), literal(Value{true})), row);
    const auto differs = expression::evaluate(
        compare(CmpOp::kNe, column(3), column(7)), row);
    const Value* equals_value = std::get_if<Value>(&equals);
    const Value* differs_value = std::get_if<Value>(&differs);
    CHECK(equals_value != nullptr && std::get<bool>(equals_value->data));
    CHECK(differs_value != nullptr && std::get<bool>(differs_value->data));
    CHECK(std::holds_alternative<expression::Error>(expression::evaluate(
        compare(CmpOp::kLt, column(7), column(3)), row)));
    CHECK(std::holds_alternative<expression::Error>(expression::evaluate(
        compare(CmpOp::kEq, column(3), literal(Value{std::int32_t{1}})), row)));
    return true;
}

bool test_boolean_physical_validation() {
    Database database;
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({flags_table()});
    CHECK(open_database(database));
    const auto valid = execute_plan(database, insert_plan({}, {{Value{true}}}, 4));
    CHECK(valid.statements.size() == 1);
    CHECK(command_of(valid.statements.front()) != nullptr);
    CHECK(fake::state().insert_calls == 1);
    CHECK(std::get<bool>(fake::state().last_insert_request->rows[0][0].data));
    for (Value invalid : {
             Value{std::int32_t{1}}, Value{std::int64_t{1}}, Value{1.0},
             Value{std::string{"true"}}, Value{std::monostate{}}}) {
        const auto result = execute_plan(
            database, insert_plan({}, {{std::move(invalid)}}, 4));
        CHECK(result.statements.size() == 1);
        CHECK(internal_script_abort(result));
        CHECK(fake::state().insert_calls == 1);
    }
    CHECK(close_database(database));
    return true;
}

bool test_insert_storage_errors_and_script_stop() {
    Database database;
    CHECK(start_database(database));
    fake::set_insert_result(tinydbms::storage::InsertResult{
        {tinydbms::storage::RecordId{10}},
        tinydbms::storage::StorageError{
            tinydbms::storage::StorageErrorKind::kIoError,
            "partial insert"}});

    std::deque<CompileResult> results;
    results.emplace_back(insert_plan(
        {},
        {{int_value(1), text_value("alice"), int_value(20)},
         {int_value(2), text_value("bob"), int_value(30)}}));
    results.emplace_back(tinydbms::compiler::CreateTablePlan{
        "must_not_run",
        {ColumnMeta{"id", Type::kInt}}});
    results.emplace_back(insert_plan(
        {}, {{int_value(9), text_value("shadow"), int_value(40)}}));
    results.emplace_back(update_plan(
        {UpdateAssignment{1, text_value("changed")}}));
    results.emplace_back(delete_plan(1));
    results.emplace_back(query(
        std::make_unique<PlanNode>(ProjectNode{
            std::vector<SlotId>{0, 1, 2}, scan()})));
    const auto script = execute_plans(database, std::move(results));
    CHECK(script.statements.size() == 6);
    CHECK(script.statements[0].status() == tinydbms::core::StatementStatus::kExecutionError);
    for (std::size_t index = 1; index < script.statements.size(); ++index) {
        CHECK(
            script.statements[index].status() ==
            tinydbms::core::StatementStatus::kSkippedExecution);
        CHECK(!script.statements[index].outcome().has_value());
    }
    CHECK(tinydbms::testing::first_error_index(script) == std::optional<std::size_t>{0});
    CHECK(tinydbms::testing::executed_count(script) == 1);
    const CommandResult* command = command_of(script.statements.front());
    CHECK(command != nullptr);
    CHECK(command->affected_rows == 1);
    CHECK(command->error.has_value());
    CHECK(command->error->kind == ErrorKind::kStorage);
    CHECK(fake::state().create_table_calls == 0);
    CHECK(fake::state().insert_calls == 1);
    CHECK(fake::state().update_calls == 0);
    CHECK(fake::state().delete_calls == 0);
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(database));

    Database mismatch_database;
    CHECK(start_database(mismatch_database));
    fake::set_insert_result(tinydbms::storage::InsertResult{
        {tinydbms::storage::RecordId{11}},
        std::nullopt});
    const auto mismatch = execute_plan(
        mismatch_database,
        insert_plan(
            {},
            {{int_value(1), text_value("alice"), int_value(20)},
             {int_value(2), text_value("bob"), int_value(30)}}));
    CHECK(mismatch.statements.size() == 1);
    CHECK(internal_script_abort(mismatch));
    CHECK(close_database(mismatch_database));

    Database too_many_ids;
    CHECK(start_database(too_many_ids));
    fake::set_insert_result(tinydbms::storage::InsertResult{
        {tinydbms::storage::RecordId{12}, tinydbms::storage::RecordId{13},
         tinydbms::storage::RecordId{14}},
        std::nullopt});
    const auto too_many = execute_plan(
        too_many_ids,
        insert_plan({}, {{int_value(1), text_value("alice"), int_value(20)}}));
    CHECK(too_many.statements.size() == 1);
    CHECK(internal_script_abort(too_many));
    CHECK(close_database(too_many_ids));
    return true;
}

bool test_query_topologies_projection_and_expression() {
    Database database;
    CHECK(start_database(database, {
        record(1, {int_value(1), text_value("alice"), int_value(30)}),
        record(2, {int_value(2), text_value("bob"), int_value(20)}),
        record(3, {int_value(3), text_value("alice"), int_value(40)})}));

    const auto full_scan = execute_plan(database, query(scan()));
    CHECK(full_scan.statements.size() == 1);
    const QueryResult* full_result = query_of(full_scan.statements.front());
    CHECK(full_result != nullptr);
    CHECK(full_result->columns.size() == 3);
    CHECK(full_result->columns[0].name == "id");
    CHECK(full_result->columns[1].type == Type::kVarchar);
    CHECK(full_result->rows.size() == 3);

    Expr predicate = logic(
        LogicOp::kAnd,
        compare(CmpOp::kGt, column(2), literal(int_value(25))),
        compare(CmpOp::kEq, column(1), literal(text_value("alice"))));
    auto filter_root = std::make_unique<PlanNode>(tinydbms::compiler::FilterNode{
        std::move(predicate),
        scan()});
    const auto filtered = execute_plan(database, query(std::move(filter_root)));
    CHECK(filtered.statements.size() == 1);
    const QueryResult* filtered_result = query_of(filtered.statements.front());
    CHECK(filtered_result != nullptr);
    CHECK(filtered_result->rows.size() == 2);
    CHECK(std::get<std::int32_t>(filtered_result->rows[0][0].data) == 1);
    CHECK(std::get<std::int32_t>(filtered_result->rows[1][0].data) == 3);

    Expr disjunction = logic(
        LogicOp::kOr,
        compare(CmpOp::kEq, column(0), literal(int_value(1))),
        compare(CmpOp::kEq, column(0), literal(int_value(3))));
    auto disjunction_root = std::make_unique<PlanNode>(tinydbms::compiler::FilterNode{
        std::move(disjunction),
        scan()});
    const auto disjunction_result = execute_plan(
        database,
        query(std::move(disjunction_root)));
    CHECK(disjunction_result.statements.size() == 1);
    const QueryResult* disjunction_query = query_of(disjunction_result.statements.front());
    CHECK(disjunction_query != nullptr);
    CHECK(disjunction_query->rows.size() == 2);
    CHECK(std::get<std::int32_t>(disjunction_query->rows[0][0].data) == 1);
    CHECK(std::get<std::int32_t>(disjunction_query->rows[1][0].data) == 3);

    auto project_root = std::make_unique<PlanNode>(ProjectNode{
        {1, 1, 0},
        scan()});
    const auto projected = execute_plan(
        database,
        query(
            std::move(project_root),
            {{1, "name", Type::kVarchar, false},
             {1, "name", Type::kVarchar, false},
             {0, "id", Type::kInt, false}}));
    CHECK(projected.statements.size() == 1);
    const QueryResult* projected_result = query_of(projected.statements.front());
    CHECK(projected_result != nullptr);
    CHECK(projected_result->columns.size() == 3);
    CHECK(projected_result->columns[0].name == "name");
    CHECK(projected_result->columns[1].name == "name");
    CHECK(projected_result->columns[2].name == "id");
    CHECK(projected_result->rows.size() == 3);
    CHECK(std::get<std::string>(projected_result->rows[1][0].data) == "bob");
    CHECK(std::get<std::string>(projected_result->rows[1][1].data) == "bob");
    CHECK(std::get<std::int32_t>(projected_result->rows[1][2].data) == 2);

    Expr final_predicate = logical_not(compare(
        CmpOp::kEq,
        column(1),
        literal(text_value("bob"))));
    auto filtered_child = std::make_unique<PlanNode>(tinydbms::compiler::FilterNode{
        std::move(final_predicate),
        scan()});
    auto project_filter_root = std::make_unique<PlanNode>(ProjectNode{
        {0, 2},
        std::move(filtered_child)});
    const auto project_filter = execute_plan(
        database,
        query(
            std::move(project_filter_root),
            {{0, "id", Type::kInt, false},
             {2, "age", Type::kInt, false}}));
    CHECK(project_filter.statements.size() == 1);
    const QueryResult* project_filter_result = query_of(project_filter.statements.front());
    CHECK(project_filter_result != nullptr);
    CHECK(project_filter_result->rows.size() == 2);
    CHECK(std::get<std::int32_t>(project_filter_result->rows[0][0].data) == 1);
    CHECK(std::get<std::int32_t>(project_filter_result->rows[0][1].data) == 30);
    CHECK(std::get<std::int32_t>(project_filter_result->rows[1][0].data) == 3);
    CHECK(std::get<std::int32_t>(project_filter_result->rows[1][1].data) == 40);

    CHECK(fake::state().open_table_calls == 5);
    CHECK(fake::state().close_cursor_calls == 5);
    CHECK(close_database(database));
    return true;
}

bool test_non_positional_slot_plumbing_and_invalid_mappings() {
    const std::vector<ScanColumn> reordered_mapping{{1, 3}, {0, 7}, {2, 11}};

    Database query_database;
    CHECK(start_database(query_database, {
        record(1, {int_value(42), text_value("alice"), int_value(30)}),
        record(2, {int_value(99), text_value("bob"), int_value(20)})}));
    auto filtered_scan = scan(1, reordered_mapping);
    auto filtered_child = std::make_unique<PlanNode>(tinydbms::compiler::FilterNode{
        compare(CmpOp::kEq, column(7), literal(int_value(42))),
        std::move(filtered_scan)});
    auto projected_root = std::make_unique<PlanNode>(ProjectNode{
        {3, 7},
        std::move(filtered_child)});
    const auto projected = execute_plan(
        query_database,
        query(
            std::move(projected_root),
            {{3, "person_name", Type::kVarchar, false},
             {7, "identifier", Type::kInt, false}}));
    CHECK(projected.statements.size() == 1);
    const QueryResult* query_result = query_of(projected.statements.front());
    CHECK(query_result != nullptr);
    CHECK(query_result->columns.size() == 2);
    CHECK(query_result->columns[0].name == "person_name");
    CHECK(query_result->columns[0].type == Type::kVarchar);
    CHECK(query_result->columns[1].name == "identifier");
    CHECK(query_result->columns[1].type == Type::kInt);
    CHECK(query_result->rows.size() == 1);
    CHECK(std::get<std::string>(query_result->rows[0][0].data) == "alice");
    CHECK(std::get<std::int32_t>(query_result->rows[0][1].data) == 42);

    auto duplicate_root = std::make_unique<PlanNode>(ProjectNode{
        {7, 7},
        scan(1, reordered_mapping)});
    const auto duplicate = execute_plan(
        query_database,
        query(
            std::move(duplicate_root),
            {{7, "id", Type::kInt, false},
             {7, "id", Type::kInt, false}}));
    CHECK(duplicate.statements.size() == 1);
    const QueryResult* duplicate_result = query_of(duplicate.statements.front());
    CHECK(duplicate_result != nullptr);
    CHECK(duplicate_result->columns.size() == 2);
    CHECK(duplicate_result->columns[0].name == "id");
    CHECK(duplicate_result->columns[1].name == "id");
    CHECK(duplicate_result->rows.size() == 2);
    CHECK(std::get<std::int32_t>(duplicate_result->rows[0][0].data) == 42);
    CHECK(std::get<std::int32_t>(duplicate_result->rows[0][1].data) == 42);
    CHECK(close_database(query_database));

    Database delete_database;
    CHECK(start_database(delete_database, {
        record(1, {int_value(42), text_value("alice"), int_value(30)}),
        record(2, {int_value(99), text_value("bob"), int_value(20)})}));
    const auto deleted = execute_plan(
        delete_database,
        delete_plan(
            1,
            std::optional<Expr>{compare(
                CmpOp::kEq,
                column(7),
                literal(int_value(42)))},
            reordered_mapping));
    CHECK(deleted.statements.size() == 1);
    const CommandResult* deleted_result = command_of(deleted.statements.front());
    CHECK(deleted_result != nullptr);
    CHECK(deleted_result->affected_rows == 1);
    CHECK(fake::state().records.size() == 1);
    CHECK(std::get<std::int32_t>(fake::state().records[0].values[0].data) == 99);
    CHECK(close_database(delete_database));

    Database invalid_column;
    CHECK(start_database(invalid_column));
    const auto invalid_column_result = execute_plan(
        invalid_column,
        query(scan(1, {{9, 3}, {0, 7}, {2, 11}})));
    CHECK(invalid_column_result.statements.size() == 1);
    CHECK(internal_script_abort(invalid_column_result));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(invalid_column));

    Database duplicate_column;
    CHECK(start_database(duplicate_column));
    const auto duplicate_column_result = execute_plan(
        duplicate_column,
        query(scan(1, {{0, 7}, {0, 3}, {2, 11}})));
    CHECK(duplicate_column_result.statements.size() == 1);
    CHECK(internal_script_abort(duplicate_column_result));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(duplicate_column));

    Database duplicate_slot;
    CHECK(start_database(duplicate_slot));
    const auto duplicate_slot_result = execute_plan(
        duplicate_slot,
        query(scan(1, {{0, 7}, {1, 7}, {2, 11}})));
    CHECK(duplicate_slot_result.statements.size() == 1);
    CHECK(internal_script_abort(duplicate_slot_result));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(duplicate_slot));

    Database missing_project_slot;
    CHECK(start_database(missing_project_slot));
    auto missing_slot_root = std::make_unique<PlanNode>(ProjectNode{
        {9},
        scan(1, reordered_mapping)});
    const auto missing_slot_result = execute_plan(
        missing_project_slot,
        query(
            std::move(missing_slot_root),
            {{9, "missing", Type::kInt, false}}));
    CHECK(missing_slot_result.statements.size() == 1);
    CHECK(internal_script_abort(missing_slot_result));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(missing_project_slot));
    return true;
}

bool test_query_output_metadata_must_match_project_positions() {
    Database count_mismatch;
    CHECK(start_database(count_mismatch));
    auto count_mismatch_root = std::make_unique<PlanNode>(ProjectNode{
        {1, 0},
        scan()});
    const auto count_mismatch_result = execute_plan(
        count_mismatch,
        query(
            std::move(count_mismatch_root),
            {{1, "name", Type::kVarchar, false}}));
    CHECK(count_mismatch_result.statements.size() == 1);
    CHECK(internal_script_abort(count_mismatch_result));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(count_mismatch));

    Database slot_mismatch;
    CHECK(start_database(slot_mismatch));
    auto slot_mismatch_root = std::make_unique<PlanNode>(ProjectNode{
        {1, 0},
        scan()});
    const auto slot_mismatch_result = execute_plan(
        slot_mismatch,
        query(
            std::move(slot_mismatch_root),
            {{1, "name", Type::kVarchar, false},
             {2, "age", Type::kInt, false}}));
    CHECK(slot_mismatch_result.statements.size() == 1);
    CHECK(internal_script_abort(slot_mismatch_result));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(slot_mismatch));
    return true;
}

bool test_inner_join_uses_non_positional_slots() {
    const std::vector<ScanColumn> user_mapping{{1, 3}, {0, 7}, {2, 11}};
    const std::vector<ScanColumn> membership_mapping{{1, 15}, {0, 20}, {2, 25}};

    Database database;
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({users_table(), memberships_table()});
    fake::set_records_for_table(1, {
        record(1, {int_value(1), text_value("alice"), int_value(30)}),
        record(2, {int_value(2), text_value("bob"), int_value(20)}),
        record(3, {int_value(3), text_value("carol"), int_value(40)})});
    fake::set_records_for_table(5, {
        record(10, {int_value(1), text_value("reader"), Value{true}}),
        record(11, {int_value(1), text_value("admin"), Value{false}}),
        record(12, {int_value(2), text_value("writer"), Value{true}}),
        record(13, {Value{std::monostate{}}, text_value("unknown"),
                    Value{std::monostate{}}})});
    CHECK(open_database(database));

    auto joined = std::make_unique<PlanNode>(JoinNode{
        JoinKind::kInner,
        compare(CmpOp::kEq, column(7), column(20)),
        scan(1, user_mapping),
        scan(5, membership_mapping)});
    auto sorted = std::make_unique<PlanNode>(SortNode{
        {{15, SortDirection::kDesc}},
        std::move(joined)});
    auto projected = std::make_unique<PlanNode>(ProjectNode{
        {3, 7},
        std::move(sorted)});
    const auto result = execute_plan(
        database,
        query(
            std::move(projected),
            {{3, "person_name", Type::kVarchar, false},
             {7, "identifier", Type::kInt, false}}));
    CHECK(result.statements.size() == 1);
    const QueryResult* rows = query_of(result.statements.front());
    CHECK(rows != nullptr && rows->rows.size() == 3);
    CHECK(rows->columns[0].name == "person_name");
    CHECK(std::get<std::string>(rows->rows[0][0].data) == "bob");
    CHECK(std::get<std::string>(rows->rows[1][0].data) == "alice");
    CHECK(std::get<std::string>(rows->rows[2][0].data) == "alice");
    CHECK(std::get<std::int32_t>(rows->rows[0][1].data) == 2);
    CHECK(fake::state().open_table_calls == 2);

    auto false_join = std::make_unique<PlanNode>(JoinNode{
        JoinKind::kInner,
        literal(Value{false}),
        scan(1, user_mapping),
        scan(5, membership_mapping)});
    auto false_project = std::make_unique<PlanNode>(ProjectNode{{7}, std::move(false_join)});
    const auto no_matches = execute_plan(
        database,
        query(std::move(false_project), {{7, "id", Type::kInt, false}}));
    const QueryResult* empty = query_of(no_matches.statements.front());
    CHECK(empty != nullptr && empty->rows.empty());

    auto varchar_join = std::make_unique<PlanNode>(JoinNode{
        JoinKind::kInner,
        compare(CmpOp::kEq, column(3), column(15)),
        scan(1, user_mapping),
        scan(5, membership_mapping)});
    auto varchar_project =
        std::make_unique<PlanNode>(ProjectNode{{7}, std::move(varchar_join)});
    const auto varchar_result = execute_plan(
        database,
        query(std::move(varchar_project), {{7, "id", Type::kInt, false}}));
    const QueryResult* varchar_rows = query_of(varchar_result.statements.front());
    CHECK(varchar_rows != nullptr && varchar_rows->rows.empty());

    auto boolean_join = std::make_unique<PlanNode>(JoinNode{
        JoinKind::kInner,
        compare(CmpOp::kEq, literal(Value{true}), column(25)),
        scan(1, user_mapping),
        scan(5, membership_mapping)});
    auto boolean_project =
        std::make_unique<PlanNode>(ProjectNode{{7}, std::move(boolean_join)});
    const auto boolean_result = execute_plan(
        database,
        query(std::move(boolean_project), {{7, "id", Type::kInt, false}}));
    const QueryResult* boolean_rows = query_of(boolean_result.statements.front());
    CHECK(boolean_rows != nullptr && boolean_rows->rows.size() == 6);

    fake::set_records_for_table(5, {});
    auto empty_right_join = std::make_unique<PlanNode>(JoinNode{
        JoinKind::kInner,
        compare(CmpOp::kEq, column(7), column(20)),
        scan(1, user_mapping),
        scan(5, membership_mapping)});
    auto empty_right_project =
        std::make_unique<PlanNode>(ProjectNode{{7}, std::move(empty_right_join)});
    const auto empty_right_result = execute_plan(
        database,
        query(std::move(empty_right_project), {{7, "id", Type::kInt, false}}));
    const QueryResult* empty_right_rows = query_of(empty_right_result.statements.front());
    CHECK(empty_right_rows != nullptr && empty_right_rows->rows.empty());
    CHECK(close_database(database));

    Database duplicate_slot;
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({users_table(), memberships_table()});
    CHECK(open_database(duplicate_slot));
    auto duplicate_join = std::make_unique<PlanNode>(JoinNode{
        JoinKind::kInner,
        literal(Value{true}),
        scan(1, user_mapping),
        scan(5, {{0, 7}, {1, 15}})});
    auto duplicate_project =
        std::make_unique<PlanNode>(ProjectNode{{7}, std::move(duplicate_join)});
    const auto duplicate_result = execute_plan(
        duplicate_slot,
        query(std::move(duplicate_project), {{7, "id", Type::kInt, false}}));
    CHECK(internal_script_abort(duplicate_result));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(duplicate_slot));

    Database missing_slot;
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({users_table(), memberships_table()});
    CHECK(open_database(missing_slot));
    auto missing_join = std::make_unique<PlanNode>(JoinNode{
        JoinKind::kInner,
        compare(CmpOp::kEq, column(7), column(99)),
        scan(1, user_mapping),
        scan(5, membership_mapping)});
    auto missing_project =
        std::make_unique<PlanNode>(ProjectNode{{7}, std::move(missing_join)});
    const auto missing_result = execute_plan(
        missing_slot,
        query(std::move(missing_project), {{7, "id", Type::kInt, false}}));
    CHECK(internal_script_abort(missing_result));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(missing_slot));
    return true;
}

bool test_sort_uses_slot_bindings_and_rejects_invalid_plans() {
    const std::vector<ScanColumn> mapping{{1, 3}, {0, 7}, {2, 11}};

    Database ordered;
    CHECK(start_database(ordered, {
        record(1, {int_value(42), text_value("alice"), int_value(30)}),
        record(2, {int_value(99), text_value("bob"), int_value(20)}),
        record(3, {int_value(50), text_value("carol"), int_value(30)})}));
    auto sorted = std::make_unique<PlanNode>(SortNode{
        {{11, SortDirection::kDesc}},
        scan(1, mapping)});
    auto projected = std::make_unique<PlanNode>(ProjectNode{{3, 7}, std::move(sorted)});
    const auto result = execute_plan(
        ordered,
        query(
            std::move(projected),
            {{3, "name", Type::kVarchar, false},
             {7, "id", Type::kInt, false}}));
    CHECK(result.statements.size() == 1);
    const QueryResult* rows = query_of(result.statements.front());
    CHECK(rows != nullptr && rows->rows.size() == 3);
    CHECK(std::get<std::string>(rows->rows[0][0].data) == "alice");
    CHECK(std::get<std::string>(rows->rows[1][0].data) == "carol");
    CHECK(std::get<std::string>(rows->rows[2][0].data) == "bob");
    CHECK(std::get<std::int32_t>(rows->rows[0][1].data) == 42);
    CHECK(std::get<std::int32_t>(rows->rows[1][1].data) == 50);
    CHECK(close_database(ordered));

    Database duplicate_keys;
    CHECK(start_database(duplicate_keys, {
        record(1, {int_value(2), text_value("two"), int_value(0)}),
        record(2, {int_value(1), text_value("one"), int_value(0)})}));
    auto duplicate_sort = std::make_unique<PlanNode>(SortNode{
        {{7, SortDirection::kAsc}, {7, SortDirection::kDesc}},
        scan(1, mapping)});
    auto duplicate_project = std::make_unique<PlanNode>(ProjectNode{{7}, std::move(duplicate_sort)});
    const auto duplicate_result = execute_plan(
        duplicate_keys,
        query(std::move(duplicate_project), {{7, "id", Type::kInt, false}}));
    const QueryResult* duplicate_rows = query_of(duplicate_result.statements.front());
    CHECK(duplicate_rows != nullptr && duplicate_rows->rows.size() == 2);
    CHECK(std::get<std::int32_t>(duplicate_rows->rows[0][0].data) == 1);
    CHECK(std::get<std::int32_t>(duplicate_rows->rows[1][0].data) == 2);
    CHECK(close_database(duplicate_keys));

    Database empty_keys;
    CHECK(start_database(empty_keys));
    auto empty_sort = std::make_unique<PlanNode>(SortNode{{}, scan(1, mapping)});
    auto empty_project = std::make_unique<PlanNode>(ProjectNode{{7}, std::move(empty_sort)});
    const auto empty_result = execute_plan(
        empty_keys,
        query(std::move(empty_project), {{7, "id", Type::kInt, false}}));
    CHECK(internal_script_abort(empty_result));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(empty_keys));

    Database missing_slot;
    CHECK(start_database(missing_slot));
    auto missing_sort = std::make_unique<PlanNode>(SortNode{
        {{99, SortDirection::kAsc}},
        scan(1, mapping)});
    auto missing_project = std::make_unique<PlanNode>(ProjectNode{{7}, std::move(missing_sort)});
    const auto missing_result = execute_plan(
        missing_slot,
        query(std::move(missing_project), {{7, "id", Type::kInt, false}}));
    CHECK(internal_script_abort(missing_result));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(missing_slot));

    Database cross_family;
    CHECK(start_database(cross_family, {
        record(1, {int_value(1), text_value("valid"), int_value(10)}),
        record(2, {int_value(2), text_value("invalid"), text_value("wrong-family")})}));
    auto cross_family_sort = std::make_unique<PlanNode>(SortNode{
        {{11, SortDirection::kAsc}},
        scan(1, mapping)});
    auto cross_family_project = std::make_unique<PlanNode>(ProjectNode{
        {7},
        std::move(cross_family_sort)});
    const auto cross_family_result = execute_plan(
        cross_family,
        query(std::move(cross_family_project), {{7, "id", Type::kInt, false}}));
    CHECK(internal_script_abort(cross_family_result));
    CHECK(close_database(cross_family));
    return true;
}

bool test_aggregate_uses_derived_slots_and_validates_plans() {
    const std::vector<ScanColumn> mapping{{1, 3}, {0, 7}, {2, 11}};

    Database database;
    CHECK(start_database(database, {
        record(1, {int_value(1), text_value("alice"), int_value(30)}),
        record(2, {int_value(2), text_value("bob"), int_value(20)}),
        record(3, {int_value(3), text_value("carol"), int_value(40)})}));
    auto aggregate = std::make_unique<PlanNode>(AggregateNode{
        {},
        {{AggregateKind::kCount, std::nullopt, 20, Type::kBigInt, false},
         {AggregateKind::kSum, 11, 21, Type::kBigInt, true},
         {AggregateKind::kAvg, 11, 22, Type::kDouble, true},
         {AggregateKind::kMin, 3, 23, Type::kVarchar, true},
         {AggregateKind::kMax, 11, 24, Type::kInt, true}},
        scan(1, mapping)});
    auto projected = std::make_unique<PlanNode>(ProjectNode{
        {20, 21, 22, 23, 24}, std::move(aggregate)});
    const auto result = execute_plan(
        database,
        query(
            std::move(projected),
            {{20, "COUNT(*)", Type::kBigInt, false},
             {21, "SUM(age)", Type::kBigInt, true},
             {22, "AVG(age)", Type::kDouble, true},
             {23, "MIN(name)", Type::kVarchar, true},
             {24, "MAX(age)", Type::kInt, true}}));
    CHECK(result.statements.size() == 1);
    const QueryResult* rows = query_of(result.statements.front());
    CHECK(rows != nullptr && rows->rows.size() == 1 && rows->rows[0].size() == 5);
    CHECK(std::get<std::int64_t>(rows->rows[0][0].data) == 3);
    CHECK(std::get<std::int64_t>(rows->rows[0][1].data) == 90);
    CHECK(std::get<double>(rows->rows[0][2].data) == 30.0);
    CHECK(std::get<std::string>(rows->rows[0][3].data) == "alice");
    CHECK(std::get<std::int32_t>(rows->rows[0][4].data) == 40);
    CHECK(close_database(database));

    Database empty;
    CHECK(start_database(empty));
    auto empty_aggregate = std::make_unique<PlanNode>(AggregateNode{
        {},
        {{AggregateKind::kCount, std::nullopt, 20, Type::kBigInt, false},
         {AggregateKind::kSum, 11, 21, Type::kBigInt, true}},
        scan(1, mapping)});
    auto empty_project = std::make_unique<PlanNode>(ProjectNode{
        {20, 21}, std::move(empty_aggregate)});
    const auto empty_result = execute_plan(
        empty,
        query(
            std::move(empty_project),
            {{20, "COUNT(*)", Type::kBigInt, false},
             {21, "SUM(age)", Type::kBigInt, true}}));
    const QueryResult* empty_rows = query_of(empty_result.statements.front());
    CHECK(empty_rows != nullptr && empty_rows->rows.size() == 1);
    CHECK(std::get<std::int64_t>(empty_rows->rows[0][0].data) == 0);
    CHECK(std::holds_alternative<std::monostate>(empty_rows->rows[0][1].data));

    auto grouped = std::make_unique<PlanNode>(AggregateNode{
        {3}, {}, scan(1, mapping)});
    auto grouped_project = std::make_unique<PlanNode>(ProjectNode{{3}, std::move(grouped)});
    const auto grouped_result = execute_plan(
        empty,
        query(std::move(grouped_project), {{3, "name", Type::kVarchar, false}}));
    const QueryResult* grouped_rows = query_of(grouped_result.statements.front());
    CHECK(grouped_rows != nullptr && grouped_rows->rows.empty());
    CHECK(close_database(empty));

    Database nullable;
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({memberships_table()});
    fake::set_records_for_table(5, {
        record(1, {int_value(1), text_value("a"), Value{true}}),
        record(2, {Value{std::monostate{}}, text_value("b"), Value{true}}),
        record(3, {int_value(2), text_value("c"), Value{std::monostate{}}}),
        record(4, {Value{std::monostate{}}, text_value("d"), Value{std::monostate{}}})});
    CHECK(open_database(nullable));
    auto nullable_aggregate = std::make_unique<PlanNode>(AggregateNode{
        {11},
        {{AggregateKind::kCount, std::nullopt, 20, Type::kBigInt, false},
         {AggregateKind::kCount, 7, 21, Type::kBigInt, false}},
        scan(5, {{1, 3}, {0, 7}, {2, 11}})});
    auto nullable_project = std::make_unique<PlanNode>(ProjectNode{
        {11, 20, 21}, std::move(nullable_aggregate)});
    const auto nullable_result = execute_plan(
        nullable,
        query(
            std::move(nullable_project),
            {{11, "active", Type::kBoolean, true},
             {20, "COUNT(*)", Type::kBigInt, false},
             {21, "COUNT(user_id)", Type::kBigInt, false}}));
    const QueryResult* nullable_rows = query_of(nullable_result.statements.front());
    CHECK(nullable_rows != nullptr && nullable_rows->rows.size() == 2);
    CHECK(std::get<bool>(nullable_rows->rows[0][0].data));
    CHECK(std::get<std::int64_t>(nullable_rows->rows[0][1].data) == 2);
    CHECK(std::get<std::int64_t>(nullable_rows->rows[0][2].data) == 1);
    CHECK(std::holds_alternative<std::monostate>(nullable_rows->rows[1][0].data));
    CHECK(std::get<std::int64_t>(nullable_rows->rows[1][1].data) == 2);
    CHECK(std::get<std::int64_t>(nullable_rows->rows[1][2].data) == 1);
    CHECK(close_database(nullable));

    Database integer_overflow;
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({events_table()});
    fake::set_records_for_table(2, {
        record(1, {Value{std::numeric_limits<std::int64_t>::max()}}),
        record(2, {Value{std::int64_t{1}}})});
    CHECK(open_database(integer_overflow));
    auto overflowing_aggregate = std::make_unique<PlanNode>(AggregateNode{
        {},
        {{AggregateKind::kSum, 7, 20, Type::kBigInt, true}},
        scan(2, {{0, 7}})});
    auto overflowing_project = std::make_unique<PlanNode>(ProjectNode{
        {20}, std::move(overflowing_aggregate)});
    std::deque<CompileResult> overflow_plans;
    overflow_plans.emplace_back(query(
        std::move(overflowing_project),
        {{20, "SUM(event_id)", Type::kBigInt, true}}));
    overflow_plans.emplace_back(query(
        std::make_unique<PlanNode>(ProjectNode{
            std::vector<SlotId>{7}, scan(2, {{0, 7}})}),
        {{7, "event_id", Type::kBigInt, false}}));
    const auto overflow_result = execute_plans(integer_overflow, std::move(overflow_plans));
    CHECK(overflow_result.statements.size() == 2);
    CHECK(overflow_result.statements[0].status() ==
          tinydbms::core::StatementStatus::kExecutionError);
    CHECK(is_error(overflow_result.statements[0], ErrorKind::kExecute));
    CHECK(overflow_result.statements[1].status() ==
          tinydbms::core::StatementStatus::kSkippedExecution);
    CHECK(!overflow_result.statements[1].outcome().has_value());
    CHECK(tinydbms::testing::first_error_index(overflow_result) ==
          std::optional<std::size_t>{0});
    CHECK(tinydbms::testing::executed_count(overflow_result) == 1);
    CHECK(close_database(integer_overflow));

    Database double_overflow;
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({measurements_table()});
    fake::set_records_for_table(3, {
        record(1, {Value{std::numeric_limits<double>::max()}}),
        record(2, {Value{std::numeric_limits<double>::max()}})});
    CHECK(open_database(double_overflow));
    auto nonfinite_aggregate = std::make_unique<PlanNode>(AggregateNode{
        {},
        {{AggregateKind::kSum, 7, 20, Type::kDouble, true}},
        scan(3, {{0, 7}})});
    auto nonfinite_project = std::make_unique<PlanNode>(ProjectNode{
        {20}, std::move(nonfinite_aggregate)});
    const auto nonfinite_result = execute_plan(
        double_overflow,
        query(
            std::move(nonfinite_project),
            {{20, "SUM(value)", Type::kDouble, true}}));
    CHECK(is_error(nonfinite_result.statements.front(), ErrorKind::kExecute));
    CHECK(close_database(double_overflow));

    Database signed_zero;
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({measurements_table()});
    fake::set_records_for_table(3, {
        record(1, {Value{-0.0}}), record(2, {Value{0.0}}), record(3, {Value{1.0}})});
    CHECK(open_database(signed_zero));
    auto zero_aggregate = std::make_unique<PlanNode>(AggregateNode{
        {7},
        {{AggregateKind::kCount, std::nullopt, 20, Type::kBigInt, false}},
        scan(3, {{0, 7}})});
    auto zero_project = std::make_unique<PlanNode>(ProjectNode{
        {7, 20}, std::move(zero_aggregate)});
    const auto zero_result = execute_plan(
        signed_zero,
        query(
            std::move(zero_project),
            {{7, "value", Type::kDouble, false},
             {20, "COUNT(*)", Type::kBigInt, false}}));
    const QueryResult* zero_rows = query_of(zero_result.statements.front());
    CHECK(zero_rows != nullptr && zero_rows->rows.size() == 2);
    CHECK(std::get<double>(zero_rows->rows[0][0].data) == 0.0);
    CHECK(std::get<std::int64_t>(zero_rows->rows[0][1].data) == 2);
    CHECK(std::get<double>(zero_rows->rows[1][0].data) == 1.0);
    CHECK(std::get<std::int64_t>(zero_rows->rows[1][1].data) == 1);
    CHECK(close_database(signed_zero));

    const auto malformed = [&](AggregateNode node, std::vector<QueryOutput> outputs) {
        Database invalid;
        CHECK(start_database(invalid));
        std::vector<SlotId> projected_slots;
        projected_slots.reserve(outputs.size());
        for (const QueryOutput& output : outputs) projected_slots.push_back(output.slot_id);
        auto root = std::make_unique<PlanNode>(ProjectNode{
            std::move(projected_slots),
            std::make_unique<PlanNode>(std::move(node))});
        const auto invalid_result = execute_plan(invalid, query(std::move(root), std::move(outputs)));
        CHECK(internal_script_abort(invalid_result));
        CHECK(fake::state().open_table_calls == 0);
        CHECK(close_database(invalid));
        return true;
    };
    CHECK(malformed(
        AggregateNode{{99}, {}, scan(1, mapping)},
        {{99, "missing", Type::kInt, false}}));
    CHECK(malformed(
        AggregateNode{
            {},
            {{AggregateKind::kSum, 99, 20, Type::kBigInt, true}},
            scan(1, mapping)},
        {{20, "SUM(missing)", Type::kBigInt, true}}));
    CHECK(malformed(
        AggregateNode{
            {},
            {{AggregateKind::kCount, std::nullopt, 20, Type::kBigInt, false},
             {AggregateKind::kCount, std::nullopt, 20, Type::kBigInt, false}},
            scan(1, mapping)},
        {{20, "COUNT(*)", Type::kBigInt, false}}));
    CHECK(malformed(
        AggregateNode{
            {},
            {{AggregateKind::kCount, std::nullopt, 7, Type::kBigInt, false}},
            scan(1, mapping)},
        {{7, "COUNT(*)", Type::kBigInt, false}}));
    CHECK(malformed(
        AggregateNode{
            {},
            {{AggregateKind::kSum, 3, 20, Type::kBigInt, true}},
            scan(1, mapping)},
        {{20, "SUM(name)", Type::kBigInt, true}}));
    CHECK(malformed(
        AggregateNode{
            {},
            {{static_cast<AggregateKind>(999), 7, 20, Type::kInt, true}},
            scan(1, mapping)},
        {{20, "INVALID(id)", Type::kInt, true}}));
    return true;
}

bool test_delete_collects_ids_and_allows_empty_delete() {
    Database database;
    CHECK(start_database(database, {
        record(1, {int_value(1), text_value("alice"), int_value(30)}),
        record(2, {int_value(2), text_value("bob"), int_value(20)}),
        record(3, {int_value(3), text_value("alice"), int_value(40)})}));

    const auto deleted = execute_plan(
        database,
        delete_plan(
            1,
            std::optional<Expr>{logical_not(compare(
                CmpOp::kEq,
                column(0),
                literal(int_value(2))))}));
    CHECK(deleted.statements.size() == 1);
    const CommandResult* deleted_command = command_of(deleted.statements.front());
    CHECK(deleted_command != nullptr);
    CHECK(deleted_command->affected_rows == 2);
    CHECK(!deleted_command->error.has_value());
    CHECK(fake::state().last_delete_request.has_value());
    CHECK(fake::state().last_delete_request->rids.size() == 2);
    CHECK(fake::state().last_delete_request->rids[0].value == 1);
    CHECK(fake::state().last_delete_request->rids[1].value == 3);
    CHECK(fake::state().records.size() == 1);
    CHECK(fake::state().records.front().rid.value == 2);

    const auto no_match = execute_plan(
        database,
        delete_plan(
            1,
            std::optional<Expr>{compare(
                CmpOp::kEq,
                column(0),
                literal(int_value(999)))}));
    CHECK(no_match.statements.size() == 1);
    const CommandResult* no_match_command = command_of(no_match.statements.front());
    CHECK(no_match_command != nullptr);
    CHECK(no_match_command->affected_rows == 0);
    CHECK(!no_match_command->error.has_value());
    CHECK(fake::state().last_delete_request.has_value());
    CHECK(fake::state().last_delete_request->rids.empty());
    CHECK(fake::state().delete_calls == 2);

    const auto delete_all = execute_plan(database, delete_plan(1));
    CHECK(delete_all.statements.size() == 1);
    const CommandResult* delete_all_command = command_of(delete_all.statements.front());
    CHECK(delete_all_command != nullptr);
    CHECK(delete_all_command->affected_rows == 1);
    CHECK(!delete_all_command->error.has_value());
    CHECK(fake::state().last_delete_request.has_value());
    CHECK(fake::state().last_delete_request->rids.size() == 1);
    CHECK(fake::state().delete_calls == 3);
    CHECK(fake::state().records.empty());
    CHECK(close_database(database));
    return true;
}

bool test_update_uses_original_rows_and_slot_mapping() {
    Database database;
    CHECK(start_database(database,{
        record(1,{int_value(1),text_value("alice"),int_value(20)}),
        record(2,{int_value(2),text_value("bob"),int_value(30)})}));
    const std::vector<ScanColumn> mapping{{1,3},{0,7},{2,11}};
    const auto result=execute_plan(
        database,
        update_plan(
            {{1,text_value("updated")}},
            std::optional<Expr>{compare(CmpOp::kGe,column(11),literal(int_value(30)))},
            mapping));
    CHECK(result.statements.size()==1);
    const CommandResult* command=command_of(result.statements.front());
    CHECK(command!=nullptr && command->affected_rows==1 && !command->error.has_value());
    CHECK(fake::state().update_calls==1 && fake::state().last_update_request.has_value());
    CHECK(fake::state().last_update_request->rows.size()==1);
    CHECK(fake::state().last_update_request->rows[0].rid.value==2);
    CHECK(std::get<std::string>(fake::state().records[1].values[1].data)=="updated");

    const auto invalid=execute_plan(database,update_plan({
        {1,text_value("x")},{1,text_value("y")}}));
    CHECK(invalid.statements.size()==1);
    CHECK(internal_script_abort(invalid));
    CHECK(fake::state().update_calls==1);
    const auto out_of_range=execute_plan(database,update_plan({{9,int_value(1)}}));
    CHECK(out_of_range.statements.size()==1 &&
          internal_script_abort(out_of_range));
    const auto wrong_type=execute_plan(database,update_plan({{0,text_value("wrong")}}));
    CHECK(wrong_type.statements.size()==1 &&
          internal_script_abort(wrong_type));
    const auto null_violation=execute_plan(database,update_plan({
        {0,Value{std::monostate{}}}}));
    CHECK(null_violation.statements.size()==1 &&
          internal_script_abort(null_violation));
    CHECK(fake::state().update_calls==1);
    CHECK(close_database(database));
    return true;
}

bool test_invalid_predicate_is_rejected_before_open_table() {
    Database database;
    CHECK(start_database(database));
    const auto invalid_delete = execute_plan(
        database,
        delete_plan(
            1,
            std::optional<Expr>{compare(
                CmpOp::kEq,
                column(0),
                literal(text_value("not an int")))}));
    CHECK(invalid_delete.statements.size() == 1);
    CHECK(internal_script_abort(invalid_delete));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(fake::state().delete_calls == 0);

    auto invalid_filter = std::make_unique<PlanNode>(tinydbms::compiler::FilterNode{
        compare(CmpOp::kEq, column(0), literal(text_value("not an int"))),
        scan()});
    const auto invalid_query = execute_plan(database, query(std::move(invalid_filter)));
    CHECK(invalid_query.statements.size() == 1);
    CHECK(internal_script_abort(invalid_query));
    CHECK(fake::state().open_table_calls == 0);

    const auto invalid_varchar_order = execute_plan(
        database,
        delete_plan(
            1,
            std::optional<Expr>{compare(
                CmpOp::kLt,
                column(1),
                literal(text_value("z")))}));
    CHECK(invalid_varchar_order.statements.size() == 1);
    CHECK(internal_script_abort(invalid_varchar_order));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(fake::state().delete_calls == 0);
    CHECK(close_database(database));
    return true;
}

bool test_truth_value_foundation() {
    using expression::TruthValue;

    const auto true_truth = expression::to_truth_value(Value{true});
    const auto false_truth = expression::to_truth_value(Value{false});
    const auto unknown_truth = expression::to_truth_value(Value{std::monostate{}});
    const auto invalid_truth = expression::to_truth_value(int_value(1));
    CHECK(std::get<TruthValue>(true_truth) == TruthValue::kTrue);
    CHECK(std::get<TruthValue>(false_truth) == TruthValue::kFalse);
    CHECK(std::get<TruthValue>(unknown_truth) == TruthValue::kUnknown);
    CHECK(std::holds_alternative<expression::Error>(invalid_truth));

    CHECK(expression::truth_not(TruthValue::kTrue) == TruthValue::kFalse);
    CHECK(expression::truth_not(TruthValue::kFalse) == TruthValue::kTrue);
    CHECK(expression::truth_not(TruthValue::kUnknown) == TruthValue::kUnknown);
    CHECK(expression::truth_and(TruthValue::kTrue, TruthValue::kUnknown) ==
          TruthValue::kUnknown);
    CHECK(expression::truth_and(TruthValue::kFalse, TruthValue::kUnknown) ==
          TruthValue::kFalse);
    CHECK(expression::truth_and(TruthValue::kUnknown, TruthValue::kUnknown) ==
          TruthValue::kUnknown);
    CHECK(expression::truth_or(TruthValue::kTrue, TruthValue::kUnknown) ==
          TruthValue::kTrue);
    CHECK(expression::truth_or(TruthValue::kFalse, TruthValue::kUnknown) ==
          TruthValue::kUnknown);
    CHECK(expression::truth_or(TruthValue::kUnknown, TruthValue::kUnknown) ==
          TruthValue::kUnknown);

    CHECK(std::get<bool>(expression::truth_value_to_value(TruthValue::kTrue).data));
    CHECK(!std::get<bool>(expression::truth_value_to_value(TruthValue::kFalse).data));
    CHECK(std::holds_alternative<std::monostate>(
        expression::truth_value_to_value(TruthValue::kUnknown).data));

    const expression::SlotRow row{
        {0, int_value(1)},
        {1, text_value("alice")},
        {2, int_value(30)}};
    const auto comparison = expression::evaluate(
        compare(CmpOp::kEq, column(0), literal(int_value(1))), row);
    const Value* comparison_value = std::get_if<Value>(&comparison);
    CHECK(comparison_value != nullptr);
    CHECK(std::get<bool>(comparison_value->data));

    for (CmpOp op : {CmpOp::kEq, CmpOp::kNe, CmpOp::kLt,
                     CmpOp::kLe, CmpOp::kGt, CmpOp::kGe}) {
        const auto null_comparison = expression::evaluate(
            compare(op, literal(Value{std::monostate{}}),
                    literal(Value{std::monostate{}})), row);
        const Value* null_value = std::get_if<Value>(&null_comparison);
        CHECK(null_value != nullptr);
        CHECK(std::holds_alternative<std::monostate>(null_value->data));
    }
    const auto int_null_comparison = expression::evaluate(
        compare(CmpOp::kEq, column(0), literal(Value{std::monostate{}})), row);
    CHECK(std::holds_alternative<std::monostate>(
        std::get<Value>(int_null_comparison).data));
    const auto varchar_null_order = expression::validate(
        compare(CmpOp::kLt, column(1), literal(Value{std::monostate{}})), row);
    CHECK(std::holds_alternative<expression::Error>(varchar_null_order));

    const auto is_null = expression::evaluate(
        null_test(NullTestOp::kIsNull, literal(Value{std::monostate{}})), row);
    const auto is_not_null = expression::evaluate(
        null_test(NullTestOp::kIsNotNull, column(1)), row);
    CHECK(std::get<bool>(std::get<Value>(is_null).data));
    CHECK(std::get<bool>(std::get<Value>(is_not_null).data));

    const auto logical_true = expression::evaluate(
        logic(LogicOp::kAnd, literal(Value{true}), literal(Value{true})), row);
    const Value* logical_true_value = std::get_if<Value>(&logical_true);
    CHECK(logical_true_value != nullptr);
    CHECK(std::get<bool>(logical_true_value->data));

    const auto logical_unknown = expression::evaluate(
        logic(
            LogicOp::kAnd,
            literal(Value{true}),
            literal(Value{std::monostate{}})),
        row);
    const Value* logical_unknown_value = std::get_if<Value>(&logical_unknown);
    CHECK(logical_unknown_value != nullptr);
    CHECK(std::holds_alternative<std::monostate>(logical_unknown_value->data));

    const auto unknown_predicate = expression::evaluate_predicate(
        literal(Value{std::monostate{}}), row);
    CHECK(std::holds_alternative<bool>(unknown_predicate));
    CHECK(!std::get<bool>(unknown_predicate));

    const auto invalid_predicate = expression::evaluate_predicate(
        literal(int_value(1)), row);
    CHECK(std::holds_alternative<expression::Error>(invalid_predicate));

    Database unknown_filter;
    CHECK(start_database(unknown_filter, {
        record(1, {int_value(1), text_value("alice"), int_value(30)})}));
    auto unknown_filter_root = std::make_unique<PlanNode>(tinydbms::compiler::FilterNode{
        literal(Value{std::monostate{}}),
        scan()});
    const auto filtered = execute_plan(unknown_filter, query(std::move(unknown_filter_root)));
    CHECK(filtered.statements.size() == 1);
    const QueryResult* filtered_query = query_of(filtered.statements.front());
    CHECK(filtered_query != nullptr);
    CHECK(filtered_query->rows.empty());
    CHECK(close_database(unknown_filter));
    return true;
}

bool test_expression_depth_is_bounded() {
    Database database;
    CHECK(start_database(database));

    auto deeply_nested_filter = std::make_unique<PlanNode>(tinydbms::compiler::FilterNode{
        deeply_nested_predicate(300),
        scan()});
    const auto result = execute_plan(database, query(std::move(deeply_nested_filter)));
    CHECK(result.statements.size() == 1);
    CHECK(internal_script_abort(result));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(database));
    return true;
}

bool test_invalid_query_plans_have_no_storage_side_effect() {
    Database null_root;
    CHECK(start_database(null_root));
    const auto null_plan = execute_plan(null_root, query(nullptr));
    CHECK(null_plan.statements.size() == 1);
    CHECK(internal_script_abort(null_plan));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(null_root));

    Database null_filter_child;
    CHECK(start_database(null_filter_child));
    auto null_filter_root = std::make_unique<PlanNode>(tinydbms::compiler::FilterNode{
        compare(CmpOp::kEq, column(0), literal(int_value(1))),
        std::unique_ptr<PlanNode>{}});
    const auto null_filter_result = execute_plan(
        null_filter_child,
        query(std::move(null_filter_root)));
    CHECK(null_filter_result.statements.size() == 1);
    CHECK(internal_script_abort(null_filter_result));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(null_filter_child));

    Database null_project_child;
    CHECK(start_database(null_project_child));
    auto null_project_root = std::make_unique<PlanNode>(ProjectNode{
        {0},
        std::unique_ptr<PlanNode>{}});
    const auto null_project_result = execute_plan(
        null_project_child,
        query(std::move(null_project_root)));
    CHECK(null_project_result.statements.size() == 1);
    CHECK(internal_script_abort(null_project_result));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(null_project_child));

    Database empty_project;
    CHECK(start_database(empty_project));
    auto empty_project_root = std::make_unique<PlanNode>(ProjectNode{{}, scan()});
    const auto empty_project_result = execute_plan(
        empty_project,
        query(std::move(empty_project_root)));
    CHECK(empty_project_result.statements.size() == 1);
    CHECK(internal_script_abort(empty_project_result));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(empty_project));

    Database invalid_topology;
    CHECK(start_database(invalid_topology));
    auto nested_project = std::make_unique<PlanNode>(ProjectNode{{0}, scan()});
    auto invalid_root = std::make_unique<PlanNode>(tinydbms::compiler::FilterNode{
        compare(CmpOp::kEq, column(0), literal(int_value(1))),
        std::move(nested_project)});
    const auto invalid_topology_result = execute_plan(
        invalid_topology,
        query(std::move(invalid_root)));
    CHECK(invalid_topology_result.statements.size() == 1);
    CHECK(internal_script_abort(invalid_topology_result));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(invalid_topology));

    Database invalid_column;
    CHECK(start_database(invalid_column));
    auto invalid_column_root = std::make_unique<PlanNode>(ProjectNode{{3}, scan()});
    const auto invalid_column_result = execute_plan(
        invalid_column,
        query(std::move(invalid_column_root)));
    CHECK(invalid_column_result.statements.size() == 1);
    CHECK(internal_script_abort(invalid_column_result));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(invalid_column));
    return true;
}

bool test_open_and_scan_result_invariants_close_cursor_once() {
    Database malformed_open;
    CHECK(start_database(malformed_open));
    fake::set_open_table_result(tinydbms::storage::OpenTableResult{
        std::optional<tinydbms::storage::CursorId>{77},
        std::optional<tinydbms::storage::StorageError>{
            tinydbms::storage::StorageError{
                tinydbms::storage::StorageErrorKind::kIoError,
                "error and cursor"}}});
    const auto malformed_open_result = execute_plan(malformed_open, query(scan()));
    CHECK(malformed_open_result.statements.size() == 1);
    CHECK(internal_script_abort(malformed_open_result));
    CHECK(fake::state().close_cursor_calls == 1);
    CHECK(close_database(malformed_open));

    Database malformed_delete_open;
    CHECK(start_database(malformed_delete_open));
    fake::set_open_table_result(tinydbms::storage::OpenTableResult{
        std::optional<tinydbms::storage::CursorId>{78},
        std::optional<tinydbms::storage::StorageError>{
            tinydbms::storage::StorageError{
                tinydbms::storage::StorageErrorKind::kIoError,
                "error and cursor"}}});
    const auto malformed_delete_result =
        execute_plan(malformed_delete_open, delete_plan(1));
    CHECK(malformed_delete_result.statements.size() == 1);
    CHECK(internal_script_abort(malformed_delete_result));
    CHECK(fake::state().close_cursor_calls == 1);
    CHECK(fake::state().delete_calls == 0);
    CHECK(close_database(malformed_delete_open));

    Database missing_cursor;
    CHECK(start_database(missing_cursor));
    fake::set_open_table_result(tinydbms::storage::OpenTableResult{
        std::nullopt,
        std::nullopt});
    const auto missing_cursor_result = execute_plan(missing_cursor, query(scan()));
    CHECK(missing_cursor_result.statements.size() == 1);
    CHECK(internal_script_abort(missing_cursor_result));
    CHECK(fake::state().close_cursor_calls == 0);
    CHECK(close_database(missing_cursor));

    Database malformed_scan;
    CHECK(start_database(malformed_scan));
    fake::set_scan_results({tinydbms::storage::ScanNextResult{
        record(1, {int_value(1), text_value("alice"), int_value(20)}),
        tinydbms::storage::StorageError{
            tinydbms::storage::StorageErrorKind::kCorrupt,
            "record and error"}}});
    const auto malformed_scan_result = execute_plan(malformed_scan, query(scan()));
    CHECK(malformed_scan_result.statements.size() == 1);
    CHECK(internal_script_abort(malformed_scan_result));
    CHECK(fake::state().close_cursor_calls == 1);
    CHECK(close_database(malformed_scan));
    return true;
}

bool test_scan_row_and_close_failures_discard_query_and_delete() {
    Database malformed_row;
    CHECK(start_database(malformed_row));
    fake::set_scan_results({tinydbms::storage::ScanNextResult{
        record(1, {int_value(1)}),
        std::nullopt}});
    const auto malformed_row_result = execute_plan(malformed_row, query(scan()));
    CHECK(malformed_row_result.statements.size() == 1);
    CHECK(internal_script_abort(malformed_row_result));
    CHECK(fake::state().close_cursor_calls == 1);
    CHECK(close_database(malformed_row));

    Database scan_error;
    CHECK(start_database(scan_error));
    fake::set_scan_results({tinydbms::storage::ScanNextResult{
        std::nullopt,
        tinydbms::storage::StorageError{
            tinydbms::storage::StorageErrorKind::kIoError,
            "scan failed"}}});
    const auto scan_error_result = execute_plan(scan_error, query(scan()));
    CHECK(scan_error_result.statements.size() == 1);
    CHECK(is_error(scan_error_result.statements.front(), ErrorKind::kStorage));
    CHECK(fake::state().close_cursor_calls == 1);
    CHECK(close_database(scan_error));

    Database close_error;
    CHECK(start_database(close_error));
    fake::set_close_cursor_error({
        tinydbms::storage::StorageErrorKind::kIoError,
        "close failed"});
    const auto close_error_result = execute_plan(close_error, query(scan()));
    CHECK(close_error_result.statements.size() == 1);
    CHECK(is_error(close_error_result.statements.front(), ErrorKind::kStorage));
    CHECK(fake::state().close_cursor_calls == 1);
    CHECK(close_database(close_error));

    Database delete_scan_error;
    CHECK(start_database(delete_scan_error));
    fake::set_scan_results({tinydbms::storage::ScanNextResult{
        std::nullopt,
        tinydbms::storage::StorageError{
            tinydbms::storage::StorageErrorKind::kIoError,
            "scan failed"}}});
    const auto delete_result = execute_plan(delete_scan_error, delete_plan(1));
    CHECK(delete_result.statements.size() == 1);
    CHECK(is_error(delete_result.statements.front(), ErrorKind::kStorage));
    CHECK(fake::state().close_cursor_calls == 1);
    CHECK(fake::state().delete_calls == 0);
    CHECK(close_database(delete_scan_error));
    return true;
}

bool test_storage_errors_and_exceptions_are_contained() {
    Database open_error;
    CHECK(start_database(open_error));
    fake::set_open_table_error({
        tinydbms::storage::StorageErrorKind::kIoError,
        "open failed"});
    const auto open_result = execute_plan(open_error, query(scan()));
    CHECK(open_result.statements.size() == 1);
    CHECK(is_error(open_result.statements.front(), ErrorKind::kStorage));
    CHECK(fake::state().close_cursor_calls == 0);
    CHECK(close_database(open_error));

    Database scan_exception;
    CHECK(start_database(scan_exception));
    fake::set_throw_on_scan_next(true);
    const auto scan_result = execute_plan(scan_exception, query(scan()));
    CHECK(scan_result.statements.size() == 1);
    CHECK(internal_abort_after_storage(scan_result));
    CHECK(fake::state().close_cursor_calls == 1);
    CHECK(close_database(scan_exception));

    Database post_open_table_exception;
    CHECK(start_database(post_open_table_exception));
    fake::set_throw_after_open_table(true);
    const auto post_open_table_result = execute_plan(
        post_open_table_exception,
        query(scan()));
    CHECK(post_open_table_result.statements.size() == 1);
    CHECK(internal_abort_after_storage(post_open_table_result));
    CHECK(fake::state().close_cursor_calls == 0);
    CHECK(fake::state().close_calls == 1);
    CHECK(!fake::state().cursor_opened);
    const auto reopen_before_close = post_open_table_exception.open(
        tinydbms::core::OpenDatabaseRequest{"test-data"});
    CHECK(reopen_before_close.error.has_value());
    CHECK(reopen_before_close.error->kind == ErrorKind::kExecute);
    CHECK(close_database(post_open_table_exception));

    Database post_open_delete_exception;
    CHECK(start_database(post_open_delete_exception));
    fake::set_throw_after_open_table(true);
    const auto post_open_delete_result = execute_plan(
        post_open_delete_exception,
        delete_plan(1));
    CHECK(post_open_delete_result.statements.size() == 1);
    CHECK(internal_abort_after_storage(post_open_delete_result));
    CHECK(fake::state().close_cursor_calls == 0);
    CHECK(fake::state().close_calls == 1);
    CHECK(close_database(post_open_delete_exception));

    Database insert_exception;
    CHECK(start_database(insert_exception));
    fake::set_throw_on_insert(true);
    const auto insert_result = execute_plan(
        insert_exception,
        insert_plan({}, {{int_value(1), text_value("alice"), int_value(20)}}));
    CHECK(insert_result.statements.size() == 1);
    CHECK(internal_abort_after_storage(insert_result));
    CHECK(fake::state().insert_calls == 1);
    CHECK(close_database(insert_exception));

    Database insert_error;
    CHECK(start_database(insert_error));
    fake::set_insert_error({
        tinydbms::storage::StorageErrorKind::kValueTooLarge,
        "value too large"});
    const auto insert_storage_error = execute_plan(
        insert_error,
        insert_plan({}, {{int_value(1), text_value("alice"), int_value(20)}}));
    CHECK(insert_storage_error.statements.size() == 1);
    CHECK(is_error(insert_storage_error.statements.front(), ErrorKind::kStorage));
    CHECK(close_database(insert_error));

    Database delete_scan_exception;
    CHECK(start_database(delete_scan_exception));
    fake::set_throw_on_scan_next(true);
    const auto delete_scan_result = execute_plan(
        delete_scan_exception,
        delete_plan(1));
    CHECK(delete_scan_result.statements.size() == 1);
    CHECK(internal_abort_after_storage(delete_scan_result));
    CHECK(fake::state().close_cursor_calls == 1);
    CHECK(fake::state().delete_calls == 0);
    CHECK(close_database(delete_scan_exception));

    Database delete_exception;
    CHECK(start_database(delete_exception));
    fake::set_throw_on_delete(true);
    const auto delete_result = execute_plan(delete_exception, delete_plan(1));
    CHECK(delete_result.statements.size() == 1);
    CHECK(internal_abort_after_storage(delete_result));
    CHECK(fake::state().close_cursor_calls == 1);
    CHECK(fake::state().delete_calls == 1);
    CHECK(close_database(delete_exception));

    Database post_create_exception;
    CHECK(start_database(post_create_exception));
    fake::set_throw_after_create_table(true);
    const auto post_create_result = execute_plan(
        post_create_exception,
        Plan{tinydbms::compiler::CreateTablePlan{
            "events",
            {ColumnMeta{"id", Type::kInt}}}});
    CHECK(post_create_result.statements.size() == 1);
    CHECK(internal_abort_after_storage(post_create_result));
    CHECK(fake::state().tables.size() == 2);
    CHECK(fake::state().close_calls == 1);
    CHECK(close_database(post_create_exception));
    fake::set_throw_after_create_table(false);
    CHECK(open_database(post_create_exception));
    CHECK(close_database(post_create_exception));

    Database post_insert_exception;
    CHECK(start_database(post_insert_exception));
    fake::set_throw_after_insert(true);
    const auto post_insert_result = execute_plan(
        post_insert_exception,
        insert_plan({}, {{int_value(1), text_value("alice"), int_value(20)}}));
    CHECK(post_insert_result.statements.size() == 1);
    CHECK(internal_abort_after_storage(post_insert_result));
    CHECK(fake::state().records.size() == 1);
    CHECK(fake::state().close_calls == 1);
    CHECK(close_database(post_insert_exception));
    fake::set_throw_after_insert(false);
    CHECK(open_database(post_insert_exception));
    CHECK(close_database(post_insert_exception));

    Database post_delete_exception;
    CHECK(start_database(post_delete_exception, {
        record(1, {int_value(1), text_value("alice"), int_value(20)}),
        record(2, {int_value(2), text_value("bob"), int_value(30)})}));
    fake::set_throw_after_delete(true);
    const auto post_delete_result = execute_plan(post_delete_exception, delete_plan(1));
    CHECK(post_delete_result.statements.size() == 1);
    CHECK(internal_abort_after_storage(post_delete_result));
    CHECK(fake::state().records.empty());
    CHECK(fake::state().close_calls == 1);
    CHECK(close_database(post_delete_exception));
    fake::set_throw_after_delete(false);
    CHECK(open_database(post_delete_exception));
    CHECK(close_database(post_delete_exception));

    Database partial_delete;
    CHECK(start_database(partial_delete, {
        record(1, {int_value(1), text_value("alice"), int_value(20)}),
        record(2, {int_value(2), text_value("bob"), int_value(30)})}));
    fake::set_delete_result(tinydbms::storage::DeleteResult{
        1,
        tinydbms::storage::StorageError{
            tinydbms::storage::StorageErrorKind::kIoError,
            "partial delete"}});
    const auto partial_delete_result = execute_plan(partial_delete, delete_plan(1));
    CHECK(partial_delete_result.statements.size() == 1);
    const CommandResult* partial_command = command_of(partial_delete_result.statements.front());
    CHECK(partial_command != nullptr);
    CHECK(partial_command->affected_rows == 1);
    CHECK(partial_command->error.has_value());
    CHECK(partial_command->error->kind == ErrorKind::kStorage);
    CHECK(close_database(partial_delete));

    Database invalid_delete_result;
    CHECK(start_database(invalid_delete_result));
    fake::set_delete_result(tinydbms::storage::DeleteResult{1, std::nullopt});
    const auto invalid_delete = execute_plan(invalid_delete_result, delete_plan(1));
    CHECK(invalid_delete.statements.size() == 1);
    CHECK(internal_script_abort(invalid_delete));
    CHECK(close_database(invalid_delete_result));
    return true;
}

bool test_close_exception_is_not_retried() {
    Database database;
    CHECK(start_database(database));
    fake::set_throw_on_close_cursor(true);
    const auto result = execute_plan(database, query(scan()));
    CHECK(result.statements.size() == 1);
    CHECK(internal_script_abort(result));
    CHECK(fake::state().close_cursor_calls == 1);
    CHECK(!fake::state().cursor_opened);
    CHECK(close_database(database));
    return true;
}

}  // namespace

int main() {
    const bool passed =
        test_insert_reorders_rows_before_storage() &&
        test_insert_prevalidation_has_no_storage_side_effect() &&
        test_bigint_validation_and_mixed_integer_comparison() &&
        test_double_runtime_comparison() &&
        test_double_physical_validation() &&
        test_boolean_runtime_comparison() &&
        test_boolean_physical_validation() &&
        test_insert_storage_errors_and_script_stop() &&
        test_query_topologies_projection_and_expression() &&
        test_non_positional_slot_plumbing_and_invalid_mappings() &&
        test_query_output_metadata_must_match_project_positions() &&
        test_inner_join_uses_non_positional_slots() &&
        test_sort_uses_slot_bindings_and_rejects_invalid_plans() &&
        test_aggregate_uses_derived_slots_and_validates_plans() &&
        test_delete_collects_ids_and_allows_empty_delete() &&
        test_update_uses_original_rows_and_slot_mapping() &&
        test_invalid_predicate_is_rejected_before_open_table() &&
        test_truth_value_foundation() &&
        test_expression_depth_is_bounded() &&
        test_invalid_query_plans_have_no_storage_side_effect() &&
        test_open_and_scan_result_invariants_close_cursor_once() &&
        test_scan_row_and_close_failures_discard_query_and_delete() &&
        test_storage_errors_and_exceptions_are_contained() &&
        test_close_exception_is_not_retried();

    if (!passed) {
        return 1;
    }

    std::cout << "core executor tests passed\n";
    return 0;
}
