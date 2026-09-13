#include "runner.hpp"

#include "fakes/storage_fake.hpp"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using tinydbms::app::CliEnvironment;
using tinydbms::app::CoreSession;
using tinydbms::testing::fake_storage::State;

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " #condition \
                      << '\n';                                                              \
            return false;                                                                    \
        }                                                                                    \
    } while (false)

struct InvocationResult {
    int exit_code;
    std::string output;
    std::string error;
};

InvocationResult invoke_cli(std::string input) {
    std::vector<std::string> arguments{
        "tinydbms",
        "--data-dir",
        "compiler-integration-data"
    };
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }

    std::istringstream input_stream{std::move(input)};
    std::ostringstream output_stream;
    std::ostringstream error_stream;
    CliEnvironment environment{
        input_stream,
        output_stream,
        error_stream,
        false
    };
    CoreSession session;
    const int exit_code = tinydbms::app::run_cli(
        static_cast<int>(argv.size()),
        argv.data(),
        "0.1.0",
        session,
        environment);
    return InvocationResult{exit_code, output_stream.str(), error_stream.str()};
}

bool test_real_compiler_drives_core_and_cli() {
    tinydbms::testing::fake_storage::reset();

    const InvocationResult result = invoke_cli(
        "CREATE TABLE students (id INT, name VARCHAR, age INT);\n"
        "INSERT INTO students VALUES (1, 'alice', 20), (2, 'bob', 17);\n"
        "UPDATE students SET name='ALICE',age=21 WHERE id=1;\n"
        "SELECT name, age FROM students WHERE age >= 18;\n");

    CHECK(result.exit_code == 0);
    CHECK(result.error.empty());
    CHECK(result.output == "OK 0\nOK 2\nOK 1\nname\tage\nALICE\t21\n");

    const State& state = tinydbms::testing::fake_storage::state();
    CHECK(!state.opened);
    CHECK(state.open_calls == 1);
    CHECK(state.list_tables_calls == 1);
    CHECK(state.create_table_calls == 1);
    CHECK(state.insert_calls == 1);
    CHECK(state.update_calls == 1);
    CHECK(state.open_table_calls == 2);
    CHECK(state.scan_next_calls == 6);
    CHECK(state.close_cursor_calls == 2);
    CHECK(state.close_calls == 1);
    CHECK(state.tables.size() == 1);
    CHECK(state.tables.front().table_id == tinydbms::TableId{0});
    CHECK(state.tables.front().table_name == "students");
    CHECK(state.records.size() == 2);
    CHECK(state.records_by_table.size() == 1);
    CHECK(state.records_by_table.at(tinydbms::TableId{0}).size() == 2);
    CHECK(state.last_create_request.has_value());
    CHECK(state.last_create_request->table_id == tinydbms::TableId{0});
    CHECK(state.last_create_request->table_name == "students");
    CHECK(state.last_create_request->columns.size() == 3);
    CHECK(state.last_insert_request.has_value());
    CHECK(state.last_insert_request->table_id == tinydbms::TableId{0});
    CHECK(state.last_insert_request->rows.size() == 2);
    CHECK(state.last_open_table_request.has_value());
    CHECK(state.last_open_table_request->table_id == tinydbms::TableId{0});
    CHECK((state.call_order == std::vector<std::string>{
        "open_storage",
        "list_tables",
        "create_table",
        "insert",
        "open_table",
        "scan_next",
        "scan_next",
        "scan_next",
        "close_cursor",
        "update_rows",
        "open_table",
        "scan_next",
        "scan_next",
        "scan_next",
        "close_cursor",
        "close_storage"}));
    return true;
}

bool test_real_compiler_error_stops_before_storage_execution() {
    tinydbms::testing::fake_storage::reset();

    const InvocationResult result = invoke_cli(
        "CREATE TABLE students (id INT);\n"
        "SELECT missing FROM students;\n"
        "INSERT INTO students VALUES (1);\n");

    CHECK(result.exit_code == 1);
    CHECK(result.output == "OK 0\nANALYSIS ONLY\n");
    CHECK(result.error.rfind("ERROR compile 2:8 ", 0) == 0);

    const State& state = tinydbms::testing::fake_storage::state();
    CHECK(state.create_table_calls == 1);
    CHECK(state.insert_calls == 0);
    CHECK(state.open_table_calls == 0);
    CHECK(state.close_calls == 1);
    CHECK((state.call_order == std::vector<std::string>{
        "open_storage",
        "list_tables",
        "create_table",
        "close_storage"}));
    return true;
}

bool test_real_compiler_diagnostics_reach_cli() {
    tinydbms::testing::fake_storage::reset();

    const InvocationResult keyword = invoke_cli(
        "CREATE TABLE students (id INT);\n"
        "SELETC id FROM students;\n");
    CHECK(keyword.exit_code == 1);
    CHECK(keyword.output == "OK 0\n");
    CHECK(keyword.error ==
        "ERROR compile 2:1 expected CREATE, INSERT, SELECT, DELETE, or UPDATE\n"
        "suggestion: did you mean 'SELECT'?\n"
        "fix-it: replace [2:1,2:7) with \"SELECT\"\n");

    tinydbms::testing::fake_storage::reset();
    const InvocationResult table = invoke_cli(
        "CREATE TABLE students (id INT);\n"
        "SELECT * FROM studnets;\n");
    CHECK(table.exit_code == 1);
    CHECK(table.output == "OK 0\n");
    CHECK(table.error ==
        "ERROR compile 2:15 table 'studnets' does not exist\n"
        "suggestion: did you mean 'students'?\n");

    tinydbms::testing::fake_storage::reset();
    const InvocationResult semicolon = invoke_cli("SELECT id FROM nowhere");
    CHECK(semicolon.exit_code == 1);
    CHECK(semicolon.error ==
        "ERROR compile 1:23 expected ';' after SELECT statement\n"
        "fix-it: insert \";\" at 1:23\n");
    return true;
}

bool test_real_compiler_keeps_table_records_isolated() {
    tinydbms::testing::fake_storage::reset();

    const InvocationResult result = invoke_cli(
        "CREATE TABLE students (id INT);\n"
        "CREATE TABLE teachers (id INT);\n"
        "INSERT INTO students VALUES (1);\n"
        "INSERT INTO teachers VALUES (2);\n"
        "SELECT * FROM students;\n"
        "SELECT * FROM teachers;\n");

    CHECK(result.exit_code == 0);
    CHECK(result.error.empty());
    CHECK(result.output == "OK 0\nOK 0\nOK 1\nOK 1\nid\n1\nid\n2\n");

    const State& state = tinydbms::testing::fake_storage::state();
    CHECK(state.tables.size() == 2);
    CHECK(state.records_by_table.size() == 2);
    CHECK(state.records_by_table.at(tinydbms::TableId{0}).size() == 1);
    CHECK(state.records_by_table.at(tinydbms::TableId{1}).size() == 1);
    CHECK(std::get<std::int32_t>(
        state.records_by_table.at(tinydbms::TableId{0}).front().values.front().data) == 1);
    CHECK(std::get<std::int32_t>(
        state.records_by_table.at(tinydbms::TableId{1}).front().values.front().data) == 2);
    CHECK(state.open_table_calls == 2);
    CHECK(state.scan_next_calls == 4);
    CHECK(state.close_cursor_calls == 2);
    CHECK(state.close_calls == 1);
    return true;
}

bool test_order_by_reaches_cli_without_storage_sort_state() {
    tinydbms::testing::fake_storage::reset();

    const InvocationResult result = invoke_cli(
        "CREATE TABLE students (id INT, name VARCHAR, age INT);\n"
        "INSERT INTO students VALUES "
        "(1,'bob',17),(2,'carol',20),(3,'alice',20);\n"
        "SELECT name FROM students WHERE id >= 1 ORDER BY age DESC,name;\n");

    CHECK(result.exit_code == 0);
    CHECK(result.error.empty());
    CHECK(result.output == "OK 0\nOK 3\nname\nalice\ncarol\nbob\n");
    const State& state = tinydbms::testing::fake_storage::state();
    CHECK(state.open_table_calls == 1);
    CHECK(state.scan_next_calls == 4);
    CHECK(state.close_cursor_calls == 1);
    CHECK(state.insert_calls == 1);
    CHECK(state.update_calls == 0);
    CHECK(state.delete_calls == 0);
    return true;
}

bool test_inner_join_reaches_cli_without_storage_join_state() {
    tinydbms::testing::fake_storage::reset();

    const InvocationResult result = invoke_cli(
        "CREATE TABLE students (id INT, name VARCHAR);\n"
        "CREATE TABLE roles (id INT, role VARCHAR, rank INT);\n"
        "INSERT INTO students VALUES (1,'alice'),(2,'bob');\n"
        "INSERT INTO roles VALUES (1,'reader',2),(1,'admin',1),(2,'writer',3);\n"
        "SELECT students.name,roles.role FROM students JOIN roles "
        "ON students.id=roles.id ORDER BY roles.rank DESC;\n");

    CHECK(result.exit_code == 0);
    CHECK(result.error.empty());
    CHECK(result.output ==
        "OK 0\nOK 0\nOK 2\nOK 3\nname\trole\nbob\twriter\nalice\treader\nalice\tadmin\n");
    const State& state = tinydbms::testing::fake_storage::state();
    CHECK(state.tables.size() == 2);
    CHECK(state.open_table_calls == 2);
    CHECK(state.scan_next_calls == 7);
    CHECK(state.close_cursor_calls == 2);
    CHECK(state.update_calls == 0);
    CHECK(state.delete_calls == 0);
    return true;
}

bool test_grouped_aggregate_reaches_cli_without_storage_aggregate_state() {
    tinydbms::testing::fake_storage::reset();

    const InvocationResult result = invoke_cli(
        "CREATE TABLE sales (dept VARCHAR NULL, amount INT NULL);\n"
        "INSERT INTO sales VALUES ('A',10),('A',20),('B',NULL),(NULL,30);\n"
        "SELECT dept,COUNT(*),COUNT(amount),SUM(amount),AVG(amount) "
        "FROM sales GROUP BY dept ORDER BY dept;\n");

    CHECK(result.exit_code == 0);
    CHECK(result.error.empty());
    CHECK(result.output ==
        "OK 0\nOK 4\ndept\tCOUNT(*)\tCOUNT(amount)\tSUM(amount)\tAVG(amount)\n"
        "A\t2\t2\t30\t15.0\nB\t1\t0\tNULL\tNULL\nNULL\t1\t1\t30\t30.0\n");
    const State& state = tinydbms::testing::fake_storage::state();
    CHECK(state.open_table_calls == 1);
    CHECK(state.scan_next_calls == 5);
    CHECK(state.close_cursor_calls == 1);
    CHECK(state.update_calls == 0);
    CHECK(state.delete_calls == 0);
    return true;
}

bool test_core_session_retries_failed_close() {
    tinydbms::testing::fake_storage::reset();

    CoreSession session;
    CHECK(!session.open(tinydbms::core::OpenDatabaseRequest{"compiler-integration-data"}).error);
    tinydbms::testing::fake_storage::set_close_error({
        tinydbms::storage::StorageErrorKind::kIoError,
        "injected close failure"});

    const auto failed = session.close();
    CHECK(failed.error.has_value());
    CHECK(failed.error->kind == tinydbms::core::ErrorKind::kStorage);
    CHECK(tinydbms::testing::fake_storage::state().close_calls == 1);

    tinydbms::testing::fake_storage::clear_close_error();
    const auto retried = session.close();
    CHECK(!retried.error.has_value());
    CHECK(tinydbms::testing::fake_storage::state().close_calls == 2);
    CHECK(!tinydbms::testing::fake_storage::state().opened);

    const auto after_close =
        session.execute_script(tinydbms::core::ExecuteScriptRequest{"SELECT * FROM students;"});
    CHECK(after_close.statements.empty());
    CHECK(after_close.script_error.has_value());
    CHECK(after_close.script_error->kind == tinydbms::core::ErrorKind::kExecute);
    CHECK(after_close.script_error->message == "session is not open");
    return true;
}

}  // namespace

int main() {
    return test_real_compiler_drives_core_and_cli() &&
            test_real_compiler_error_stops_before_storage_execution() &&
            test_real_compiler_diagnostics_reach_cli() &&
            test_real_compiler_keeps_table_records_isolated() &&
            test_order_by_reaches_cli_without_storage_sort_state() &&
            test_inner_join_reaches_cli_without_storage_join_state() &&
            test_grouped_aggregate_reaches_cli_without_storage_aggregate_state() &&
            test_core_session_retries_failed_close()
        ? 0
        : 1;
}
