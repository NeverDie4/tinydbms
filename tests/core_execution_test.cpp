#include "tinydbms/core.hpp"
#include "storage_test_access.h"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <variant>

namespace {

std::filesystem::path temporary_database_path() {
    return std::filesystem::temp_directory_path() /
        ("tinydbms-core-execution-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

const tinydbms::core::QueryResult& query_of(const tinydbms::core::ExecuteResult& result) {
    return std::get<tinydbms::core::QueryResult>(result.outcome);
}

const tinydbms::core::CommandResult& command_of(const tinydbms::core::ExecuteResult& result) {
    return std::get<tinydbms::core::CommandResult>(result.outcome);
}

const tinydbms::core::ExecuteResult& outcome_of(
    const tinydbms::core::ExecuteScriptResult& script,
    std::size_t index = 0) {
    return *script.statements.at(index).outcome();
}

}  // namespace

int main() {
    using namespace tinydbms;
    using namespace tinydbms::core;

    const auto data_dir = temporary_database_path();
    Database database;
    assert(!database.open({data_dir.string()}).error);

    Database contender;
    const auto contested = contender.open({data_dir.string()});
    assert(contested.error && contested.error->kind == ErrorKind::kExecute);

    const auto created = database.execute_script(
        {"CREATE TABLE student (id INT, name VARCHAR);"});
    assert(created.statements.size() == 1);
    assert(created.statements[0].status() == StatementStatus::kExecuted);
    assert(!created.script_error.has_value());
    assert(!command_of(outcome_of(created)).error);

    const auto invalid_create = database.execute_script({"CREATE TABLE invalid (id UNKNOWN);"});
    assert(invalid_create.statements.size() == 1);
    assert(invalid_create.statements[0].status() == StatementStatus::kCompileError);
    assert(std::holds_alternative<Error>(outcome_of(invalid_create).outcome));
    const auto valid_after_failure = database.execute_script({"CREATE TABLE recovered (id INT);"});
    assert(valid_after_failure.statements.size() == 1 &&
           !command_of(outcome_of(valid_after_failure)).error);

    const auto oversized = database.execute_script({std::string(kMaxSqlBytes + 1, ' ')});
    assert(oversized.statements.empty());
    assert(oversized.script_error.has_value());
    assert(oversized.script_error->kind == ErrorKind::kCompile);
    assert(oversized.script_error->compile_stage.has_value() &&
           *oversized.script_error->compile_stage == CompileStage::kLex);
    assert(oversized.script_error->source.has_value());
    assert(oversized.script_error->source->begin.line == 1 &&
           oversized.script_error->source->begin.column == 1 &&
           oversized.script_error->source->begin_offset == 0 &&
           oversized.script_error->source->end_offset == 0);
    const auto valid_after_oversized = database.execute_script({"SELECT * FROM student;"});
    assert(valid_after_oversized.statements.size() == 1);
    assert(std::holds_alternative<QueryResult>(outcome_of(valid_after_oversized).outcome));

    const auto inserted = database.execute_script(
        {"INSERT INTO student VALUES (1, 'Alice'), (2, 'Bob');"});
    assert(inserted.statements.size() == 1);
    assert(command_of(outcome_of(inserted)).affected_rows == 2);

    const auto reordered = database.execute_script(
        {"INSERT INTO student(name, id) VALUES ('Carol', 3);"});
    assert(reordered.statements.size() == 1 &&
           command_of(outcome_of(reordered)).affected_rows == 1);

    const auto selected = database.execute_script(
        {"SELECT name FROM student WHERE id = 1;"});
    assert(selected.statements.size() == 1);
    const auto& rows = query_of(outcome_of(selected));
    assert(rows.columns.size() == 1 && rows.columns[0].name == "name");
    assert(rows.rows.size() == 1 && std::get<std::string>(rows.rows[0][0].data) == "Alice");

    const auto logical = database.execute_script(
        {"SELECT id FROM student WHERE NOT id = 2 AND (name = 'Alice' OR name = 'Nobody');"});
    assert(logical.statements.size() == 1 && query_of(outcome_of(logical)).rows.size() == 1);

    const auto deleted = database.execute_script(
        {"DELETE FROM student WHERE id = 2; SELECT * FROM student;"});
    assert(deleted.statements.size() == 2);
    assert(deleted.statements[0].status() == StatementStatus::kExecuted);
    assert(deleted.statements[1].status() == StatementStatus::kExecuted);
    assert(command_of(outcome_of(deleted, 0)).affected_rows == 1);
    const auto& remaining = query_of(outcome_of(deleted, 1));
    assert(remaining.rows.size() == 2);
    assert(std::get<std::int32_t>(remaining.rows[0][0].data) == 1);

    assert(!database.close().error);
    assert(!contender.open({data_dir.string()}).error);
    assert(!contender.close().error);
    Database reopened;
    assert(!reopened.open({data_dir.string()}).error);
    const auto persistent = reopened.execute_script({"SELECT * FROM student;"});
    assert(persistent.statements.size() == 1 &&
           query_of(outcome_of(persistent)).rows.size() == 2);
    assert(!reopened.close().error);

    const auto cleanup_dir = temporary_database_path();
    Database cleanup_owner;
    assert(!cleanup_owner.open({cleanup_dir.string()}).error);
    tinydbms::storage::internal::StorageTestAccess::fail_next_file_close();
    assert(cleanup_owner.close().error.has_value());
    Database cleanup_blocked;
    const auto blocked_while_cleaning = cleanup_blocked.open({cleanup_dir.string()});
    assert(blocked_while_cleaning.error && blocked_while_cleaning.error->kind == ErrorKind::kExecute);
    assert(!cleanup_owner.close().error);
    assert(!cleanup_blocked.open({cleanup_dir.string()}).error);
    assert(!cleanup_blocked.close().error);

    std::filesystem::remove_all(data_dir);
    std::filesystem::remove_all(cleanup_dir);
    return 0;
}
