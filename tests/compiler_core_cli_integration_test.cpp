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
        "SELECT name, age FROM students WHERE age >= 18;\n");

    CHECK(result.exit_code == 0);
    CHECK(result.error.empty());
    CHECK(result.output == "OK 0\nOK 2\nname\tage\nalice\t20\n");

    const State& state = tinydbms::testing::fake_storage::state();
    CHECK(!state.opened);
    CHECK(state.open_calls == 1);
    CHECK(state.list_tables_calls == 1);
    CHECK(state.create_table_calls == 1);
    CHECK(state.insert_calls == 1);
    CHECK(state.open_table_calls == 1);
    CHECK(state.scan_next_calls == 3);
    CHECK(state.close_cursor_calls == 1);
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
    CHECK(result.output == "OK 0\n");
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

}  // namespace

int main() {
    return test_real_compiler_drives_core_and_cli() &&
            test_real_compiler_error_stops_before_storage_execution() &&
            test_real_compiler_keeps_table_records_isolated()
        ? 0
        : 1;
}
