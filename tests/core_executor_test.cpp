#include "fakes/compiler_fake.hpp"
#include "fakes/storage_fake.hpp"
#include "tinydbms/core.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using tinydbms::ColumnId;
using tinydbms::ColumnMeta;
using tinydbms::TableId;
using tinydbms::TableMeta;
using tinydbms::Type;
using tinydbms::Value;
using tinydbms::compiler::Binary;
using tinydbms::compiler::CmpOp;
using tinydbms::compiler::ColumnRef;
using tinydbms::compiler::CompileResult;
using tinydbms::compiler::DeletePlan;
using tinydbms::compiler::Expr;
using tinydbms::compiler::InsertPlan;
using tinydbms::compiler::LogicOp;
using tinydbms::compiler::Literal;
using tinydbms::compiler::Plan;
using tinydbms::compiler::PlanNode;
using tinydbms::compiler::ProjectNode;
using tinydbms::compiler::QueryPlan;
using tinydbms::compiler::SeqScanNode;
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

Expr column(ColumnId column_id) {
    return Expr{ColumnRef{column_id}};
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

Expr deeply_nested_predicate(std::size_t depth) {
    Expr predicate = compare(CmpOp::kEq, column(0), literal(int_value(1)));
    for (std::size_t index = 0; index < depth; ++index) {
        predicate = logical_not(std::move(predicate));
    }
    return predicate;
}

std::unique_ptr<PlanNode> scan(TableId table_id = 1) {
    return std::make_unique<PlanNode>(SeqScanNode{table_id});
}

Plan query(std::unique_ptr<PlanNode> root) {
    return Plan{QueryPlan{std::move(root)}};
}

Plan insert_plan(
    std::vector<ColumnId> columns,
    std::vector<std::vector<Value>> rows,
    TableId table_id = 1) {
    return Plan{InsertPlan{table_id, std::move(columns), std::move(rows)}};
}

Plan delete_plan(TableId table_id, std::optional<Expr> predicate = std::nullopt) {
    return Plan{DeletePlan{table_id, std::move(predicate)}};
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
    fake::set_records(std::move(records));
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
    fake_compiler::set_compile_results(std::move(results));
    return database.execute_script(ExecuteScriptRequest{"first;second;"});
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

bool is_error(const ExecuteResult& result, ErrorKind kind) {
    const Error* error = error_of(result);
    return error != nullptr && error->kind == kind;
}

bool test_insert_reorders_rows_before_storage() {
    Database database;
    CHECK(start_database(database));

    const auto script = execute_plan(
        database,
        insert_plan(
            {2, 0, 1},
            {{int_value(30), int_value(7), text_value("alice")}}));
    CHECK(script.outcomes.size() == 1);
    const CommandResult* command = command_of(script.outcomes.front());
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
    CHECK(script.outcomes.size() == 1);
    CHECK(is_error(script.outcomes.front(), ErrorKind::kInternal));
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
        "must-not-run",
        {ColumnMeta{"id", Type::kInt}}});
    const auto script = execute_plans(database, std::move(results));
    CHECK(script.outcomes.size() == 1);
    const CommandResult* command = command_of(script.outcomes.front());
    CHECK(command != nullptr);
    CHECK(command->affected_rows == 1);
    CHECK(command->error.has_value());
    CHECK(command->error->kind == ErrorKind::kStorage);
    CHECK(fake::state().create_table_calls == 0);
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
    CHECK(mismatch.outcomes.size() == 1);
    CHECK(is_error(mismatch.outcomes.front(), ErrorKind::kInternal));
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
    CHECK(too_many.outcomes.size() == 1);
    CHECK(is_error(too_many.outcomes.front(), ErrorKind::kInternal));
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
    CHECK(full_scan.outcomes.size() == 1);
    const QueryResult* full_result = query_of(full_scan.outcomes.front());
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
    CHECK(filtered.outcomes.size() == 1);
    const QueryResult* filtered_result = query_of(filtered.outcomes.front());
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
    CHECK(disjunction_result.outcomes.size() == 1);
    const QueryResult* disjunction_query = query_of(disjunction_result.outcomes.front());
    CHECK(disjunction_query != nullptr);
    CHECK(disjunction_query->rows.size() == 2);
    CHECK(std::get<std::int32_t>(disjunction_query->rows[0][0].data) == 1);
    CHECK(std::get<std::int32_t>(disjunction_query->rows[1][0].data) == 3);

    auto project_root = std::make_unique<PlanNode>(ProjectNode{
        {1, 1, 0},
        scan()});
    const auto projected = execute_plan(database, query(std::move(project_root)));
    CHECK(projected.outcomes.size() == 1);
    const QueryResult* projected_result = query_of(projected.outcomes.front());
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
    const auto project_filter = execute_plan(database, query(std::move(project_filter_root)));
    CHECK(project_filter.outcomes.size() == 1);
    const QueryResult* project_filter_result = query_of(project_filter.outcomes.front());
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
    CHECK(deleted.outcomes.size() == 1);
    const CommandResult* deleted_command = command_of(deleted.outcomes.front());
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
    CHECK(no_match.outcomes.size() == 1);
    const CommandResult* no_match_command = command_of(no_match.outcomes.front());
    CHECK(no_match_command != nullptr);
    CHECK(no_match_command->affected_rows == 0);
    CHECK(!no_match_command->error.has_value());
    CHECK(fake::state().last_delete_request.has_value());
    CHECK(fake::state().last_delete_request->rids.empty());
    CHECK(fake::state().delete_calls == 2);

    const auto delete_all = execute_plan(database, delete_plan(1));
    CHECK(delete_all.outcomes.size() == 1);
    const CommandResult* delete_all_command = command_of(delete_all.outcomes.front());
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
    CHECK(invalid_delete.outcomes.size() == 1);
    CHECK(is_error(invalid_delete.outcomes.front(), ErrorKind::kInternal));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(fake::state().delete_calls == 0);

    auto invalid_filter = std::make_unique<PlanNode>(tinydbms::compiler::FilterNode{
        compare(CmpOp::kEq, column(0), literal(text_value("not an int"))),
        scan()});
    const auto invalid_query = execute_plan(database, query(std::move(invalid_filter)));
    CHECK(invalid_query.outcomes.size() == 1);
    CHECK(is_error(invalid_query.outcomes.front(), ErrorKind::kInternal));
    CHECK(fake::state().open_table_calls == 0);

    const auto invalid_varchar_order = execute_plan(
        database,
        delete_plan(
            1,
            std::optional<Expr>{compare(
                CmpOp::kLt,
                column(1),
                literal(text_value("z")))}));
    CHECK(invalid_varchar_order.outcomes.size() == 1);
    CHECK(is_error(invalid_varchar_order.outcomes.front(), ErrorKind::kInternal));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(fake::state().delete_calls == 0);
    CHECK(close_database(database));
    return true;
}

bool test_expression_depth_is_bounded() {
    Database database;
    CHECK(start_database(database));

    auto deeply_nested_filter = std::make_unique<PlanNode>(tinydbms::compiler::FilterNode{
        deeply_nested_predicate(300),
        scan()});
    const auto result = execute_plan(database, query(std::move(deeply_nested_filter)));
    CHECK(result.outcomes.size() == 1);
    CHECK(is_error(result.outcomes.front(), ErrorKind::kInternal));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(database));
    return true;
}

bool test_invalid_query_plans_have_no_storage_side_effect() {
    Database null_root;
    CHECK(start_database(null_root));
    const auto null_plan = execute_plan(null_root, query(nullptr));
    CHECK(null_plan.outcomes.size() == 1);
    CHECK(is_error(null_plan.outcomes.front(), ErrorKind::kInternal));
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
    CHECK(null_filter_result.outcomes.size() == 1);
    CHECK(is_error(null_filter_result.outcomes.front(), ErrorKind::kInternal));
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
    CHECK(null_project_result.outcomes.size() == 1);
    CHECK(is_error(null_project_result.outcomes.front(), ErrorKind::kInternal));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(null_project_child));

    Database empty_project;
    CHECK(start_database(empty_project));
    auto empty_project_root = std::make_unique<PlanNode>(ProjectNode{{}, scan()});
    const auto empty_project_result = execute_plan(
        empty_project,
        query(std::move(empty_project_root)));
    CHECK(empty_project_result.outcomes.size() == 1);
    CHECK(is_error(empty_project_result.outcomes.front(), ErrorKind::kInternal));
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
    CHECK(invalid_topology_result.outcomes.size() == 1);
    CHECK(is_error(invalid_topology_result.outcomes.front(), ErrorKind::kInternal));
    CHECK(fake::state().open_table_calls == 0);
    CHECK(close_database(invalid_topology));

    Database invalid_column;
    CHECK(start_database(invalid_column));
    auto invalid_column_root = std::make_unique<PlanNode>(ProjectNode{{3}, scan()});
    const auto invalid_column_result = execute_plan(
        invalid_column,
        query(std::move(invalid_column_root)));
    CHECK(invalid_column_result.outcomes.size() == 1);
    CHECK(is_error(invalid_column_result.outcomes.front(), ErrorKind::kInternal));
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
    CHECK(malformed_open_result.outcomes.size() == 1);
    CHECK(is_error(malformed_open_result.outcomes.front(), ErrorKind::kInternal));
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
    CHECK(malformed_delete_result.outcomes.size() == 1);
    CHECK(is_error(malformed_delete_result.outcomes.front(), ErrorKind::kInternal));
    CHECK(fake::state().close_cursor_calls == 1);
    CHECK(fake::state().delete_calls == 0);
    CHECK(close_database(malformed_delete_open));

    Database missing_cursor;
    CHECK(start_database(missing_cursor));
    fake::set_open_table_result(tinydbms::storage::OpenTableResult{
        std::nullopt,
        std::nullopt});
    const auto missing_cursor_result = execute_plan(missing_cursor, query(scan()));
    CHECK(missing_cursor_result.outcomes.size() == 1);
    CHECK(is_error(missing_cursor_result.outcomes.front(), ErrorKind::kInternal));
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
    CHECK(malformed_scan_result.outcomes.size() == 1);
    CHECK(is_error(malformed_scan_result.outcomes.front(), ErrorKind::kInternal));
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
    CHECK(malformed_row_result.outcomes.size() == 1);
    CHECK(is_error(malformed_row_result.outcomes.front(), ErrorKind::kInternal));
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
    CHECK(scan_error_result.outcomes.size() == 1);
    CHECK(is_error(scan_error_result.outcomes.front(), ErrorKind::kStorage));
    CHECK(fake::state().close_cursor_calls == 1);
    CHECK(close_database(scan_error));

    Database close_error;
    CHECK(start_database(close_error));
    fake::set_close_cursor_error({
        tinydbms::storage::StorageErrorKind::kIoError,
        "close failed"});
    const auto close_error_result = execute_plan(close_error, query(scan()));
    CHECK(close_error_result.outcomes.size() == 1);
    CHECK(is_error(close_error_result.outcomes.front(), ErrorKind::kStorage));
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
    CHECK(delete_result.outcomes.size() == 1);
    CHECK(is_error(delete_result.outcomes.front(), ErrorKind::kStorage));
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
    CHECK(open_result.outcomes.size() == 1);
    CHECK(is_error(open_result.outcomes.front(), ErrorKind::kStorage));
    CHECK(fake::state().close_cursor_calls == 0);
    CHECK(close_database(open_error));

    Database scan_exception;
    CHECK(start_database(scan_exception));
    fake::set_throw_on_scan_next(true);
    const auto scan_result = execute_plan(scan_exception, query(scan()));
    CHECK(scan_result.outcomes.size() == 1);
    CHECK(is_error(scan_result.outcomes.front(), ErrorKind::kInternal));
    CHECK(fake::state().close_cursor_calls == 1);
    CHECK(close_database(scan_exception));

    Database post_open_table_exception;
    CHECK(start_database(post_open_table_exception));
    fake::set_throw_after_open_table(true);
    const auto post_open_table_result = execute_plan(
        post_open_table_exception,
        query(scan()));
    CHECK(post_open_table_result.outcomes.size() == 1);
    CHECK(is_error(post_open_table_result.outcomes.front(), ErrorKind::kInternal));
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
    CHECK(post_open_delete_result.outcomes.size() == 1);
    CHECK(is_error(post_open_delete_result.outcomes.front(), ErrorKind::kInternal));
    CHECK(fake::state().close_cursor_calls == 0);
    CHECK(fake::state().close_calls == 1);
    CHECK(close_database(post_open_delete_exception));

    Database insert_exception;
    CHECK(start_database(insert_exception));
    fake::set_throw_on_insert(true);
    const auto insert_result = execute_plan(
        insert_exception,
        insert_plan({}, {{int_value(1), text_value("alice"), int_value(20)}}));
    CHECK(insert_result.outcomes.size() == 1);
    CHECK(is_error(insert_result.outcomes.front(), ErrorKind::kInternal));
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
    CHECK(insert_storage_error.outcomes.size() == 1);
    CHECK(is_error(insert_storage_error.outcomes.front(), ErrorKind::kStorage));
    CHECK(close_database(insert_error));

    Database delete_scan_exception;
    CHECK(start_database(delete_scan_exception));
    fake::set_throw_on_scan_next(true);
    const auto delete_scan_result = execute_plan(
        delete_scan_exception,
        delete_plan(1));
    CHECK(delete_scan_result.outcomes.size() == 1);
    CHECK(is_error(delete_scan_result.outcomes.front(), ErrorKind::kInternal));
    CHECK(fake::state().close_cursor_calls == 1);
    CHECK(fake::state().delete_calls == 0);
    CHECK(close_database(delete_scan_exception));

    Database delete_exception;
    CHECK(start_database(delete_exception));
    fake::set_throw_on_delete(true);
    const auto delete_result = execute_plan(delete_exception, delete_plan(1));
    CHECK(delete_result.outcomes.size() == 1);
    CHECK(is_error(delete_result.outcomes.front(), ErrorKind::kInternal));
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
    CHECK(post_create_result.outcomes.size() == 1);
    CHECK(is_error(post_create_result.outcomes.front(), ErrorKind::kInternal));
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
    CHECK(post_insert_result.outcomes.size() == 1);
    CHECK(is_error(post_insert_result.outcomes.front(), ErrorKind::kInternal));
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
    CHECK(post_delete_result.outcomes.size() == 1);
    CHECK(is_error(post_delete_result.outcomes.front(), ErrorKind::kInternal));
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
    CHECK(partial_delete_result.outcomes.size() == 1);
    const CommandResult* partial_command = command_of(partial_delete_result.outcomes.front());
    CHECK(partial_command != nullptr);
    CHECK(partial_command->affected_rows == 1);
    CHECK(partial_command->error.has_value());
    CHECK(partial_command->error->kind == ErrorKind::kStorage);
    CHECK(close_database(partial_delete));

    Database invalid_delete_result;
    CHECK(start_database(invalid_delete_result));
    fake::set_delete_result(tinydbms::storage::DeleteResult{1, std::nullopt});
    const auto invalid_delete = execute_plan(invalid_delete_result, delete_plan(1));
    CHECK(invalid_delete.outcomes.size() == 1);
    CHECK(is_error(invalid_delete.outcomes.front(), ErrorKind::kInternal));
    CHECK(close_database(invalid_delete_result));
    return true;
}

bool test_close_exception_is_not_retried() {
    Database database;
    CHECK(start_database(database));
    fake::set_throw_on_close_cursor(true);
    const auto result = execute_plan(database, query(scan()));
    CHECK(result.outcomes.size() == 1);
    CHECK(is_error(result.outcomes.front(), ErrorKind::kInternal));
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
        test_insert_storage_errors_and_script_stop() &&
        test_query_topologies_projection_and_expression() &&
        test_delete_collects_ids_and_allows_empty_delete() &&
        test_invalid_predicate_is_rejected_before_open_table() &&
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
