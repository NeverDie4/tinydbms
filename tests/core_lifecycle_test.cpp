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
using tinydbms::TableId;
using tinydbms::TableMeta;
using tinydbms::Type;
using tinydbms::compiler::CreateTablePlan;
using tinydbms::compiler::CompileError;
using tinydbms::compiler::CompileErrorKind;
using tinydbms::compiler::CompileResult;
using tinydbms::compiler::Plan;
using tinydbms::core::CommandResult;
using tinydbms::core::Database;
using tinydbms::core::Error;
using tinydbms::core::ErrorKind;
using tinydbms::core::ExecuteResult;
using tinydbms::core::ExecuteScriptResult;
using tinydbms::core::ExecuteScriptRequest;
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
    CHECK(script.outcomes.size() == 1);
    const ExecuteResult& result = script.outcomes.front();
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
    CHECK(failed_script.outcomes.size() == 1);
    const ExecuteResult& failed = failed_script.outcomes.front();
    CHECK(is_error(failed, ErrorKind::kStorage));
    CHECK(fake::state().last_create_request->table_id == 0);

    fake::clear_create_table_error();
    const auto retried_script = execute_one_plan(database, create_table_plan("events"));
    CHECK(retried_script.outcomes.size() == 1);
    const ExecuteResult& retried = retried_script.outcomes.front();
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
    CHECK(script.outcomes.size() == 1);
    const ExecuteResult& result = script.outcomes.front();
    CHECK(is_error(result, ErrorKind::kExecute));
    CHECK(fake::state().create_table_calls == create_calls);
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_process_guard_and_close_failure_release() {
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

    fake::clear_close_error();
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
    CHECK(script.outcomes.size() == 1);
    CHECK(is_error(script.outcomes.front(), ErrorKind::kExecute));

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
    CHECK(unopened_script.outcomes.size() == 1);
    const ExecuteResult& unopened_plan = unopened_script.outcomes.front();
    CHECK(is_error(unopened_plan, ErrorKind::kExecute));
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
    CHECK(create_exception_script.outcomes.size() == 1);
    const ExecuteResult& create_exception = create_exception_script.outcomes.front();
    CHECK(is_error(create_exception, ErrorKind::kInternal));
    fake::set_throw_on_create_table(false);
    const auto retry_script = execute_one_plan(database, create_table_plan("events"));
    CHECK(retry_script.outcomes.size() == 1);
    const ExecuteResult& retry = retry_script.outcomes.front();
    CHECK(std::holds_alternative<CommandResult>(retry.outcome));

    fake::set_throw_on_close(true);
    const auto close_exception = database.close();
    CHECK(close_exception.error.has_value());
    CHECK(close_exception.error->kind == ErrorKind::kInternal);
    CHECK(!fake::state().opened);
    fake::set_throw_on_close(false);

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

bool test_execute_script_compiles_in_order_and_stops_on_error() {
    fake::reset();
    fake_compiler::reset();
    Database database;
    CHECK(open_database(database));

    std::deque<CompileResult> compile_results;
    compile_results.emplace_back(create_table_plan("events"));
    compile_results.emplace_back(CompileError{
        CompileErrorKind::kSyntax,
        tinydbms::SourceLocation{1, 3},
        "injected syntax error"});
    fake_compiler::set_compile_results(std::move(compile_results));

    const auto script = database.execute_script(
        ExecuteScriptRequest{"create events;\ninvalid statement;"});
    CHECK(script.outcomes.size() == 2);
    CHECK(std::holds_alternative<CommandResult>(script.outcomes[0].outcome));
    CHECK(is_error(script.outcomes[1], ErrorKind::kCompile));
    const Error& compile_error = std::get<Error>(script.outcomes[1].outcome);
    CHECK(compile_error.location.has_value());
    CHECK(compile_error.location->line == 2);
    CHECK(compile_error.location->column == 3);
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
        test_process_guard_and_close_failure_release() &&
        test_repeated_lifecycle_calls_are_rejected() &&
        test_open_recovery_failure_cleans_up() &&
        test_invalid_catalog_metadata_is_rejected() &&
        test_moved_from_is_safe_and_returns_error() &&
        test_move_assignment_transfers_open_state() &&
        test_destructor_releases_process_guard() &&
        test_invalid_open_and_unopened_plan_do_not_touch_storage() &&
        test_unexpected_storage_exceptions_are_contained() &&
        test_list_tables_exception_cleans_up() &&
        test_execute_script_compiles_in_order_and_stops_on_error();

    if (!passed) {
        return 1;
    }

    std::cout << "core lifecycle tests passed\n";
    return 0;
}
