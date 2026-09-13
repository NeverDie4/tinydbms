#include "fakes/compiler_fake.hpp"
#include "fakes/storage_fake.hpp"
#include "tinydbms/core.hpp"

#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using tinydbms::ColumnMeta;
using tinydbms::CompileStage;
using tinydbms::TableId;
using tinydbms::TableMeta;
using tinydbms::Type;
using tinydbms::compiler::CreateTablePlan;
using tinydbms::compiler::CompileError;
using tinydbms::compiler::CompileResult;
using tinydbms::compiler::Plan;
using tinydbms::core::CommandResult;
using tinydbms::core::Database;
using tinydbms::core::Error;
using tinydbms::core::ErrorKind;
using tinydbms::core::ExecuteResult;
using tinydbms::core::ExecuteScriptResult;
using tinydbms::core::ExecuteScriptRequest;
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

Plan create_table_plan(std::string table_name) {
    return Plan{CreateTablePlan{
        std::move(table_name),
        std::vector<ColumnMeta>{{"id", Type::kInt}}}};
}

ExecuteScriptResult execute_one_plan(Database& database, Plan plan) {
    fake_compiler::reset();
    std::deque<CompileResult> compile_results;
    compile_results.emplace_back(std::move(plan));
    fake_compiler::set_compile_results(std::move(compile_results));
    return database.execute_script(ExecuteScriptRequest{"synthetic statement;"});
}

const ExecuteResult& first_outcome(const ExecuteScriptResult& script) {
    return *script.statements.front().outcome();
}

std::size_t statement_count(const ExecuteScriptResult& script) {
    return script.statements.size();
}

tinydbms::core::StatementStatus status_of(
    const ExecuteScriptResult& script,
    std::size_t index = 0) {
    return script.statements.at(index).status();
}

bool script_error_is(const ExecuteScriptResult& script, ErrorKind kind) {
    return script.script_error.has_value() && script.script_error->kind == kind;
}

bool is_empty_insertion_point(const tinydbms::SourceRange& range) {
    return range.begin.line == 1 && range.begin.column == 1 &&
        range.end.line == 1 && range.end.column == 1 &&
        range.begin_offset == 0 && range.end_offset == 0;
}

bool is_error(const ExecuteResult& result, ErrorKind expected) {
    const Error* error = std::get_if<Error>(&result.outcome);
    return error != nullptr && error->kind == expected;
}

bool open_database(Database& database) {
    const auto result = database.open(tinydbms::core::OpenDatabaseRequest{"test-data"});
    return !result.error.has_value();
}

bool test_open_recovers_catalog_and_allocates_next_id() {
    fake::reset();
    fake::set_tables({
        TableMeta{2, "users", {{"id", Type::kInt}}},
        TableMeta{7, "orders", {{"id", Type::kInt}}},
    });

    Database database;
    CHECK(open_database(database));
    CHECK(fake::state().list_tables_calls == 1);

    const auto script = execute_one_plan(database, create_table_plan("events"));
    CHECK(!script.script_error.has_value());
    CHECK(statement_count(script) == 1);
    CHECK(status_of(script) == StatementStatus::kExecuted);
    const ExecuteResult& result = first_outcome(script);
    const CommandResult* command = std::get_if<CommandResult>(&result.outcome);
    CHECK(command != nullptr);
    CHECK(!command->error.has_value());
    CHECK(fake::state().last_create_request.has_value());
    CHECK(fake::state().last_create_request->table_id == 8);

    const auto closed = database.close();
    CHECK(!closed.error.has_value());
    CHECK(!fake::state().opened);
    return true;
}

bool test_create_failure_does_not_consume_id() {
    fake::reset();
    Database database;
    CHECK(open_database(database));

    fake::set_create_table_error({
        tinydbms::storage::StorageErrorKind::kIoError,
        "injected create failure"});
    const auto failed_script = execute_one_plan(database, create_table_plan("events"));
    CHECK(statement_count(failed_script) == 1);
    CHECK(status_of(failed_script) == StatementStatus::kExecutionError);
    const ExecuteResult& failed = first_outcome(failed_script);
    CHECK(is_error(failed, ErrorKind::kStorage));
    CHECK(fake::state().last_create_request->table_id == 0);

    fake::clear_create_table_error();
    const auto retried_script = execute_one_plan(database, create_table_plan("events"));
    CHECK(statement_count(retried_script) == 1);
    CHECK(status_of(retried_script) == StatementStatus::kExecuted);
    const ExecuteResult& retried = first_outcome(retried_script);
    const CommandResult* command = std::get_if<CommandResult>(&retried.outcome);
    CHECK(command != nullptr);
    CHECK(!command->error.has_value());
    CHECK(fake::state().last_create_request->table_id == 0);

    CHECK(!database.close().error.has_value());
    return true;
}

bool test_table_id_exhaustion_does_not_call_storage() {
    fake::reset();
    fake::set_tables({
        TableMeta{std::numeric_limits<TableId>::max(), "last_table", {{"id", Type::kInt}}},
    });

    Database database;
    CHECK(open_database(database));
    const std::size_t create_calls = fake::state().create_table_calls;
    const auto script = execute_one_plan(database, create_table_plan("events"));
    CHECK(statement_count(script) == 1);
    CHECK(status_of(script) == StatementStatus::kExecutionError);
    const ExecuteResult& result = first_outcome(script);
    CHECK(is_error(result, ErrorKind::kExecute));
    CHECK(fake::state().create_table_calls == create_calls);
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_process_guard_and_close_failure_retry() {
    fake::reset();
    Database first;
    Database second;
    CHECK(open_database(first));

    const std::size_t open_calls = fake::state().open_calls;
    const auto duplicate = second.open(tinydbms::core::OpenDatabaseRequest{"other-data"});
    CHECK(duplicate.error.has_value());
    CHECK(duplicate.error->kind == ErrorKind::kExecute);
    CHECK(fake::state().open_calls == open_calls);

    fake::set_close_error({
        tinydbms::storage::StorageErrorKind::kIoError,
        "injected close failure"});
    const auto close_result = first.close();
    CHECK(close_result.error.has_value());
    CHECK(close_result.error->kind == ErrorKind::kStorage);
    CHECK(!fake::state().opened);

    const std::size_t open_calls_after_failure = fake::state().open_calls;
    const auto blocked_while_cleaning = second.open(
        tinydbms::core::OpenDatabaseRequest{"other-data"});
    CHECK(blocked_while_cleaning.error.has_value());
    CHECK(blocked_while_cleaning.error->kind == ErrorKind::kExecute);
    CHECK(fake::state().open_calls == open_calls_after_failure);

    fake::clear_close_error();
    CHECK(!first.close().error.has_value());
    CHECK(open_database(second));
    CHECK(!second.close().error.has_value());
    return true;
}

bool test_repeated_lifecycle_calls_are_rejected() {
    fake::reset();
    Database database;
    CHECK(open_database(database));

    const auto duplicate_open = database.open(tinydbms::core::OpenDatabaseRequest{"other-data"});
    CHECK(duplicate_open.error.has_value());
    CHECK(duplicate_open.error->kind == ErrorKind::kExecute);
    CHECK(fake::state().open_calls == 1);

    CHECK(!database.close().error.has_value());
    const auto duplicate_close = database.close();
    CHECK(duplicate_close.error.has_value());
    CHECK(duplicate_close.error->kind == ErrorKind::kExecute);
    CHECK(fake::state().close_calls == 1);
    return true;
}

bool test_open_recovery_failure_cleans_up() {
    fake::reset();
    fake::set_list_tables_error({
        tinydbms::storage::StorageErrorKind::kCorrupt,
        "injected catalog failure"});

    Database database;
    const auto failed = database.open(tinydbms::core::OpenDatabaseRequest{"test-data"});
    CHECK(failed.error.has_value());
    CHECK(failed.error->kind == ErrorKind::kStorage);
    CHECK(fake::state().close_calls == 1);
    CHECK(!fake::state().opened);

    fake::state().list_tables_error.reset();
    CHECK(open_database(database));
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_invalid_catalog_metadata_is_rejected() {
    fake::reset();
    fake::set_tables({
        TableMeta{
            1,
            "broken",
            {{"value", static_cast<Type>(99)}}}});

    Database database;
    const auto failed = database.open(tinydbms::core::OpenDatabaseRequest{"test-data"});
    CHECK(failed.error.has_value());
    CHECK(failed.error->kind == ErrorKind::kInternal);
    CHECK(fake::state().close_calls == 1);
    CHECK(!fake::state().opened);

    fake::set_tables({
        TableMeta{1, "Users", {{"id", Type::kInt}}}});
    Database invalid_name;
    const auto invalid_name_result = invalid_name.open(
        tinydbms::core::OpenDatabaseRequest{"test-data"});
    CHECK(invalid_name_result.error.has_value());
    CHECK(invalid_name_result.error->kind == ErrorKind::kInternal);
    CHECK(!fake::state().opened);

    fake::set_tables({
        TableMeta{
            1,
            "users",
            {ColumnMeta{"id", Type::kInt}, ColumnMeta{"id", Type::kInt}}}});
    Database duplicate_columns;
    const auto duplicate_columns_result = duplicate_columns.open(
        tinydbms::core::OpenDatabaseRequest{"test-data"});
    CHECK(duplicate_columns_result.error.has_value());
    CHECK(duplicate_columns_result.error->kind == ErrorKind::kInternal);
    CHECK(!fake::state().opened);

    fake::set_tables({});
    CHECK(open_database(database));
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_moved_from_is_safe_and_returns_error() {
    fake::reset();
    Database source;
    CHECK(open_database(source));

    Database destination = std::move(source);
    const auto moved_from_open = source.open(tinydbms::core::OpenDatabaseRequest{"other-data"});
    CHECK(moved_from_open.error.has_value());
    CHECK(moved_from_open.error->kind == ErrorKind::kExecute);
    const auto moved_from_close = source.close();
    CHECK(moved_from_close.error.has_value());
    CHECK(moved_from_close.error->kind == ErrorKind::kExecute);

    const auto script = source.execute_script(ExecuteScriptRequest{""});
    CHECK(statement_count(script) == 0);
    CHECK(script_error_is(script, ErrorKind::kExecute));

    CHECK(!destination.close().error.has_value());
    return true;
}

bool test_move_assignment_transfers_open_state() {
    fake::reset();
    Database source;
    CHECK(open_database(source));

    Database destination;
    destination = std::move(source);

    const auto moved_from_open = source.open(tinydbms::core::OpenDatabaseRequest{"other-data"});
    CHECK(moved_from_open.error.has_value());
    CHECK(moved_from_open.error->kind == ErrorKind::kExecute);
    CHECK(fake::state().open_calls == 1);

    CHECK(!destination.close().error.has_value());
    CHECK(!fake::state().opened);
    return true;
}

bool test_move_assignment_preserves_cleanup_handle_on_exception() {
    fake::reset();
    Database destination;
    CHECK(open_database(destination));

    fake::set_throw_before_close(true);
    const auto close_exception = destination.close();
    CHECK(close_exception.error.has_value());
    CHECK(close_exception.error->kind == ErrorKind::kInternal);

    Database source;
    destination = std::move(source);
    fake::set_throw_before_close(false);
    CHECK(!destination.close().error.has_value());
    CHECK(open_database(source));
    CHECK(!source.close().error.has_value());
    return true;
}

bool test_destructor_releases_process_guard() {
    fake::reset();
    {
        Database database;
        CHECK(open_database(database));
    }

    CHECK(!fake::state().opened);
    Database next;
    CHECK(open_database(next));
    CHECK(!next.close().error.has_value());
    return true;
}

bool test_invalid_open_and_unopened_plan_do_not_touch_storage() {
    fake::reset();
    Database database;

    const auto empty_path = database.open(tinydbms::core::OpenDatabaseRequest{});
    CHECK(empty_path.error.has_value());
    CHECK(empty_path.error->kind == ErrorKind::kExecute);
    CHECK(fake::state().open_calls == 0);

    const auto unopened_script = execute_one_plan(database, create_table_plan("events"));
    CHECK(statement_count(unopened_script) == 0);
    CHECK(script_error_is(unopened_script, ErrorKind::kExecute));
    CHECK(fake::state().create_table_calls == 0);
    return true;
}

bool test_unexpected_storage_exceptions_are_contained() {
    fake::reset();
    Database database;

    fake::set_throw_on_open(true);
    const auto open_exception = database.open(tinydbms::core::OpenDatabaseRequest{"test-data"});
    CHECK(open_exception.error.has_value());
    CHECK(open_exception.error->kind == ErrorKind::kInternal);
    CHECK(!fake::state().opened);

    fake::set_throw_on_open(false);

    fake::set_throw_after_open(true);
    const auto post_open_exception = database.open(tinydbms::core::OpenDatabaseRequest{"test-data"});
    CHECK(post_open_exception.error.has_value());
    CHECK(post_open_exception.error->kind == ErrorKind::kInternal);
    CHECK(fake::state().close_calls == 2);
    CHECK(!fake::state().opened);
    fake::set_throw_after_open(false);

    CHECK(open_database(database));

    fake::set_throw_on_create_table(true);
    const auto create_exception_script = execute_one_plan(database, create_table_plan("events"));
    CHECK(statement_count(create_exception_script) == 1);
    CHECK(status_of(create_exception_script) == StatementStatus::kExecutionIndeterminate);
    CHECK(script_error_is(create_exception_script, ErrorKind::kInternal));
    CHECK(create_exception_script.script_error->source.has_value());
    CHECK(create_exception_script.script_error->source->begin.line == 1);
    CHECK(create_exception_script.script_error->source->begin.column == 1);
    fake::set_throw_on_create_table(false);
    CHECK(!database.close().error.has_value());
    CHECK(open_database(database));
    const auto retry_script = execute_one_plan(database, create_table_plan("events"));
    CHECK(statement_count(retry_script) == 1);
    CHECK(status_of(retry_script) == StatementStatus::kExecuted);
    const ExecuteResult& retry = first_outcome(retry_script);
    CHECK(std::holds_alternative<CommandResult>(retry.outcome));

    fake::set_throw_on_close(true);
    const auto close_exception = database.close();
    CHECK(close_exception.error.has_value());
    CHECK(close_exception.error->kind == ErrorKind::kInternal);
    CHECK(!fake::state().opened);
    fake::set_throw_on_close(false);

    CHECK(!database.close().error.has_value());
    CHECK(open_database(database));
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_storage_cleanup_is_retryable_after_pre_close_exception() {
    fake::reset();
    Database database;
    CHECK(open_database(database));

    fake::set_throw_before_close(true);
    const auto close_exception = database.close();
    CHECK(close_exception.error.has_value());
    CHECK(close_exception.error->kind == ErrorKind::kInternal);
    CHECK(fake::state().opened);

    fake::set_throw_before_close(false);
    Database competing_database;
    const std::size_t open_calls = fake::state().open_calls;
    const auto competing_open = competing_database.open(
        tinydbms::core::OpenDatabaseRequest{"other-data"});
    CHECK(competing_open.error.has_value());
    CHECK(competing_open.error->kind == ErrorKind::kExecute);
    CHECK(fake::state().open_calls == open_calls);

    const auto reopen_before_cleanup = database.open(
        tinydbms::core::OpenDatabaseRequest{"test-data"});
    CHECK(reopen_before_cleanup.error.has_value());
    CHECK(reopen_before_cleanup.error->kind == ErrorKind::kExecute);

    CHECK(!database.close().error.has_value());
    CHECK(!fake::state().opened);
    CHECK(open_database(database));
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_cleanup_pending_execute_script_is_rejected_without_storage() {
    fake::reset();
    Database database;
    CHECK(open_database(database));

    fake::set_throw_before_close(true);
    const auto close_exception = database.close();
    CHECK(close_exception.error.has_value());
    CHECK(close_exception.error->kind == ErrorKind::kInternal);

    // cleanup-pending：execute_script 沿用未打开语义返回 kExecute script_error，
    // 不调用 compiler/storage，也不破坏后续 close 重试。
    fake_compiler::reset();
    std::deque<CompileResult> compile_results;
    compile_results.emplace_back(create_table_plan("events"));
    fake_compiler::set_compile_results(std::move(compile_results));
    const std::size_t split_calls = fake_compiler::state().split_calls;
    const std::size_t compile_calls = fake_compiler::state().compile_calls;
    const std::size_t create_table_calls = fake::state().create_table_calls;

    const auto script = database.execute_script(
        ExecuteScriptRequest{"create table events (id int);"});
    CHECK(script.statements.empty());
    CHECK(script_error_is(script, ErrorKind::kExecute));
    CHECK(script.script_error.has_value());
    CHECK(!script.script_error->source.has_value());
    CHECK(fake_compiler::state().split_calls == split_calls);
    CHECK(fake_compiler::state().compile_calls == compile_calls);
    CHECK(fake::state().create_table_calls == create_table_calls);

    fake::set_throw_before_close(false);
    CHECK(!database.close().error.has_value());
    CHECK(open_database(database));
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_destructor_releases_guard_after_cleanup_retry_failure() {
    fake::reset();
    {
        Database database;
        CHECK(open_database(database));
        fake::set_throw_before_close(true);
        const auto close_exception = database.close();
        CHECK(close_exception.error.has_value());
        CHECK(close_exception.error->kind == ErrorKind::kInternal);
    }

    fake::set_throw_before_close(false);
    CHECK(!tinydbms::storage::close_storage(tinydbms::storage::CloseStorageRequest{}).error.has_value());
    Database next;
    CHECK(open_database(next));
    CHECK(!next.close().error.has_value());
    return true;
}

bool test_open_cleanup_is_retryable_after_pre_close_exception() {
    fake::reset();
    fake::set_throw_on_list_tables(true);
    fake::set_throw_before_close(true);
    Database database;

    const auto open_exception = database.open(
        tinydbms::core::OpenDatabaseRequest{"test-data"});
    CHECK(open_exception.error.has_value());
    CHECK(open_exception.error->kind == ErrorKind::kInternal);
    CHECK(fake::state().opened);

    Database competing_database;
    const std::size_t open_calls = fake::state().open_calls;
    const auto competing_open = competing_database.open(
        tinydbms::core::OpenDatabaseRequest{"other-data"});
    CHECK(competing_open.error.has_value());
    CHECK(competing_open.error->kind == ErrorKind::kExecute);
    CHECK(fake::state().open_calls == open_calls);

    fake::set_throw_on_list_tables(false);
    fake::set_throw_before_close(false);
    CHECK(database.open(tinydbms::core::OpenDatabaseRequest{"test-data"}).error.has_value());
    CHECK(!database.close().error.has_value());
    CHECK(!fake::state().opened);
    CHECK(open_database(database));
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_list_tables_exception_cleans_up() {
    fake::reset();
    fake::set_throw_on_list_tables(true);
    Database database;

    const auto failed = database.open(tinydbms::core::OpenDatabaseRequest{"test-data"});
    CHECK(failed.error.has_value());
    CHECK(failed.error->kind == ErrorKind::kInternal);
    CHECK(fake::state().close_calls == 1);
    CHECK(!fake::state().opened);

    fake::set_throw_on_list_tables(false);
    CHECK(open_database(database));
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_split_error_does_not_touch_storage() {
    fake::reset();
    fake_compiler::reset();
    Database database;
    CHECK(open_database(database));

    const auto oversized = database.execute_script(
        ExecuteScriptRequest{std::string(tinydbms::kMaxSqlBytes + 1, ' ')});
    CHECK(statement_count(oversized) == 0);
    CHECK(script_error_is(oversized, ErrorKind::kCompile));
    CHECK(oversized.script_error->compile_stage.has_value());
    CHECK(*oversized.script_error->compile_stage == CompileStage::kLex);
    CHECK(oversized.script_error->source.has_value());
    CHECK(is_empty_insertion_point(*oversized.script_error->source));
    CHECK(fake_compiler::state().split_calls == 1);
    CHECK(fake_compiler::state().compile_calls == 0);
    CHECK(fake::state().close_calls == 0);
    CHECK(fake::state().opened);

    CHECK(!database.close().error.has_value());
    CHECK(fake::state().close_calls == 1);
    return true;
}

bool test_execute_script_compiles_in_order_and_stops_on_error() {
    fake::reset();
    fake_compiler::reset();
    Database database;
    CHECK(open_database(database));

    std::deque<CompileResult> compile_results;
    compile_results.emplace_back(create_table_plan("events"));
    compile_results.emplace_back(fake_compiler::make_compile_error(
        CompileStage::kSyntax,
        "invalid statement;",
        0,
        7,
        "injected syntax error"));
    fake_compiler::set_compile_results(std::move(compile_results));

    const auto script = database.execute_script(
        ExecuteScriptRequest{"create events;\ninvalid statement;"});
    CHECK(statement_count(script) == 2);
    CHECK(status_of(script, 0) == StatementStatus::kExecuted);
    CHECK(std::holds_alternative<CommandResult>(first_outcome(script).outcome));
    CHECK(status_of(script, 1) == StatementStatus::kCompileError);
    CHECK(!script.script_error.has_value());
    const Error& compile_error = std::get<Error>(script.statements[1].outcome()->outcome);
    CHECK(compile_error.kind == ErrorKind::kCompile);
    CHECK(compile_error.compile_stage.has_value());
    CHECK(*compile_error.compile_stage == CompileStage::kSyntax);
    CHECK(compile_error.source.has_value());
    CHECK(compile_error.source->begin.line == 2);
    CHECK(compile_error.source->begin.column == 1);
    CHECK(compile_error.source->end.line == 2);
    CHECK(compile_error.source->end.column == 8);
    CHECK(fake_compiler::state().split_calls == 1);
    CHECK(fake_compiler::state().compile_calls == 2);
    CHECK(fake_compiler::state().catalog_sizes.size() == 2);
    CHECK(fake_compiler::state().catalog_sizes[0] == 0);
    CHECK(fake_compiler::state().catalog_sizes[1] == 1);
    CHECK(fake::state().create_table_calls == 1);

    CHECK(!database.close().error.has_value());
    return true;
}

}  // namespace

int main() {
    const bool passed =
        test_open_recovers_catalog_and_allocates_next_id() &&
        test_create_failure_does_not_consume_id() &&
        test_table_id_exhaustion_does_not_call_storage() &&
        test_process_guard_and_close_failure_retry() &&
        test_repeated_lifecycle_calls_are_rejected() &&
        test_open_recovery_failure_cleans_up() &&
        test_invalid_catalog_metadata_is_rejected() &&
        test_moved_from_is_safe_and_returns_error() &&
        test_move_assignment_transfers_open_state() &&
        test_move_assignment_preserves_cleanup_handle_on_exception() &&
        test_destructor_releases_process_guard() &&
        test_invalid_open_and_unopened_plan_do_not_touch_storage() &&
        test_unexpected_storage_exceptions_are_contained() &&
        test_storage_cleanup_is_retryable_after_pre_close_exception() &&
        test_cleanup_pending_execute_script_is_rejected_without_storage() &&
        test_destructor_releases_guard_after_cleanup_retry_failure() &&
        test_open_cleanup_is_retryable_after_pre_close_exception() &&
        test_list_tables_exception_cleans_up() &&
        test_split_error_does_not_touch_storage() &&
        test_execute_script_compiles_in_order_and_stops_on_error();

    if (!passed) {
        return 1;
    }

    std::cout << "core lifecycle tests passed\n";
    return 0;
}
