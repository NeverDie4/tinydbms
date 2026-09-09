#include "runner.hpp"

#include "tinydbms/core.hpp"

#include <cstdint>
#include <deque>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using tinydbms::Type;
using tinydbms::Value;
using tinydbms::app::CliEnvironment;
using tinydbms::app::Session;
using tinydbms::core::CloseDatabaseResult;
using tinydbms::core::CommandResult;
using tinydbms::core::Error;
using tinydbms::core::ErrorKind;
using tinydbms::core::ExecuteResult;
using tinydbms::core::ExecuteScriptRequest;
using tinydbms::core::ExecuteScriptResult;
using tinydbms::core::OpenDatabaseResult;

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " #condition \
                      << '\n';                                                              \
            return false;                                                                    \
        }                                                                                    \
    } while (false)

class FakeSession final : public Session {
public:
    OpenDatabaseResult open_result{std::nullopt};
    CloseDatabaseResult close_result{std::nullopt};
    std::deque<ExecuteScriptResult> execute_results;
    std::vector<std::string> calls;
    std::vector<std::string> execute_texts;
    std::string opened_data_dir;
    bool throw_on_execute = false;
    bool throw_on_close = false;

    OpenDatabaseResult open(
        const tinydbms::core::OpenDatabaseRequest& request) override {
        calls.emplace_back("open");
        opened_data_dir = request.data_dir;
        return open_result;
    }

    ExecuteScriptResult execute_script(const ExecuteScriptRequest& request) override {
        calls.emplace_back("execute");
        execute_texts.push_back(request.text);
        if (throw_on_execute) {
            throw std::runtime_error("fake execute failure");
        }
        if (execute_results.empty()) {
            return ExecuteScriptResult{};
        }
        ExecuteScriptResult result = std::move(execute_results.front());
        execute_results.pop_front();
        return result;
    }

    CloseDatabaseResult close() override {
        calls.emplace_back("close");
        if (throw_on_close) {
            throw std::runtime_error("fake close failure");
        }
        return close_result;
    }
};

int invoke(
    FakeSession& session,
    std::vector<std::string> arguments,
    std::string input_text,
    bool interactive,
    std::string& output_text,
    std::string& error_text) {
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }

    std::istringstream input{std::move(input_text)};
    std::ostringstream output;
    std::ostringstream error;
    CliEnvironment environment{input, output, error, interactive};
    const int result = tinydbms::app::run_cli(
        static_cast<int>(argv.size()),
        argv.data(),
        "0.1.0",
        session,
        environment);
    output_text = output.str();
    error_text = error.str();
    return result;
}

Error make_error(ErrorKind kind, std::string message) {
    return Error{kind, std::nullopt, std::move(message)};
}

ExecuteScriptResult make_script(std::initializer_list<ExecuteResult> outcomes) {
    ExecuteScriptResult result;
    result.outcomes.assign(outcomes.begin(), outcomes.end());
    return result;
}

std::size_t count_occurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

bool test_help_and_version_do_not_open() {
    FakeSession session;
    std::string output;
    std::string error;

    CHECK(invoke(session, {"tinydbms", "--help"}, "", false, output, error) == 0);
    CHECK(output.find("Usage: tinydbms") != std::string::npos);
    CHECK(error.empty());
    CHECK(session.calls.empty());

    output.clear();
    error.clear();
    CHECK(invoke(session, {"tinydbms", "--version"}, "", false, output, error) == 0);
    CHECK(output == "tinydbms 0.1.0\n");
    CHECK(error.empty());
    CHECK(session.calls.empty());
    return true;
}

bool test_default_data_dir_and_empty_batch() {
    FakeSession session;
    std::string output;
    std::string error;
    CHECK(invoke(session, {"tinydbms"}, "", false, output, error) == 0);
    CHECK(session.opened_data_dir == "./tinydbms-data");
    CHECK((session.execute_texts == std::vector<std::string>{""}));
    CHECK((session.calls == std::vector<std::string>{"open", "execute", "close"}));
    CHECK(output.empty());
    CHECK(error.empty());
    return true;
}

bool test_argument_errors_do_not_open() {
    const std::vector<std::vector<std::string>> invalid_arguments{
        {"tinydbms", "--unknown"},
        {"tinydbms", "--data-dir"},
        {"tinydbms", "--data-dir", ""},
        {"tinydbms", "--data-dir", "--help"},
        {"tinydbms", "--data-dir", "one", "--data-dir", "two"},
        {"tinydbms", "--help", "--data-dir", "one"},
        {"tinydbms", "--data-dir=one"},
    };

    for (const auto& arguments : invalid_arguments) {
        FakeSession session;
        std::string output;
        std::string error;
        CHECK(invoke(session, arguments, "", false, output, error) == 2);
        CHECK(session.calls.empty());
        CHECK(output.empty());
        CHECK(error.find("argument error:") != std::string::npos);
        CHECK(error.find("Usage: tinydbms") != std::string::npos);
    }
    return true;
}

bool test_batch_rendering_and_lifecycle() {
    FakeSession session;
    session.execute_results.push_back(make_script({
        ExecuteResult{CommandResult{2, std::nullopt}},
        ExecuteResult{tinydbms::core::QueryResult{
            {tinydbms::core::ColumnHeader{"name", Type::kVarchar},
             tinydbms::core::ColumnHeader{"count", Type::kInt}},
            {{Value{std::string{"a\tb\n\\\r"}}, Value{std::int32_t{-3}}}}}},
        ExecuteResult{Error{
            ErrorKind::kCompile,
            tinydbms::SourceLocation{2, 3},
            "bad\nmessage"}},
    }));

    std::string output;
    std::string error;
    CHECK(invoke(
              session,
              {"tinydbms", "--data-dir", "测试目录"},
              "first;\nsecond;",
              false,
              output,
              error) == 1);
    CHECK(session.opened_data_dir == "测试目录");
    CHECK(session.execute_texts.size() == 1);
    CHECK(session.execute_texts.front() == "first;\nsecond;");
    CHECK((session.calls == std::vector<std::string>{"open", "execute", "close"}));
    std::string expected_output = "OK 2\nname\tcount\na";
    expected_output += "\\t";
    expected_output += "b\\n";
    expected_output.append(3, '\\');
    expected_output += "r\t-3\n";
    CHECK(output == expected_output);
    CHECK(error == "ERROR compile 2:3 bad\\nmessage\n");
    CHECK(error.find("tinydbms>") == std::string::npos);
    return true;
}

bool test_repl_continues_after_sql_error() {
    FakeSession session;
    session.execute_results.push_back(make_script({
        ExecuteResult{make_error(ErrorKind::kExecute, "first failure")}}));
    session.execute_results.push_back(make_script({
        ExecuteResult{CommandResult{1, std::nullopt}}}));

    std::string output;
    std::string error;
    CHECK(invoke(session, {"tinydbms"}, "first\nsecond\n", true, output, error) == 1);
    CHECK((session.execute_texts == std::vector<std::string>{"first", "second"}));
    CHECK((session.calls == std::vector<std::string>{"open", "execute", "execute", "close"}));
    CHECK(output == "OK 1\n");
    CHECK(count_occurrences(error, "tinydbms> ") == 3);
    CHECK(error.find("ERROR execute first failure\n") != std::string::npos);
    return true;
}

bool test_open_and_close_failures() {
    FakeSession opening_session;
    opening_session.open_result.error = make_error(ErrorKind::kStorage, "open failed");
    std::string output;
    std::string error;
    CHECK(invoke(opening_session, {"tinydbms"}, "", false, output, error) == 1);
    CHECK((opening_session.calls == std::vector<std::string>{"open"}));
    CHECK(error == "ERROR storage open failed\n");

    FakeSession closing_session;
    closing_session.execute_results.push_back(make_script({
        ExecuteResult{CommandResult{0, std::nullopt}}}));
    closing_session.close_result.error = make_error(ErrorKind::kStorage, "close failed");
    output.clear();
    error.clear();
    CHECK(invoke(closing_session, {"tinydbms"}, "", false, output, error) == 1);
    CHECK((closing_session.calls == std::vector<std::string>{"open", "execute", "close"}));
    CHECK(error == "ERROR storage close failed\n");
    return true;
}

bool test_exceptions_close_once_and_input_failure_closes() {
    FakeSession throwing_session;
    throwing_session.throw_on_execute = true;
    std::string output;
    std::string error;
    CHECK(invoke(throwing_session, {"tinydbms"}, "statement", false, output, error) == 1);
    CHECK((throwing_session.calls == std::vector<std::string>{"open", "execute", "close"}));
    CHECK(error.find("ERROR internal application exception: fake execute failure\n") !=
          std::string::npos);

    FakeSession closing_session;
    closing_session.throw_on_close = true;
    output.clear();
    error.clear();
    CHECK(invoke(closing_session, {"tinydbms"}, "statement", false, output, error) == 1);
    CHECK((closing_session.calls == std::vector<std::string>{"open", "execute", "close"}));
    CHECK(error.find("ERROR internal close exception: fake close failure\n") !=
          std::string::npos);

    FakeSession input_error_session;
    std::istringstream input;
    input.setstate(std::ios::badbit | std::ios::eofbit);
    std::ostringstream output_stream;
    std::ostringstream error_stream;
    CliEnvironment environment{input, output_stream, error_stream, false};
    std::vector<std::string> arguments{"tinydbms"};
    std::vector<char*> argv{arguments[0].data()};
    CHECK(tinydbms::app::run_cli(
              1,
              argv.data(),
              "0.1.0",
              input_error_session,
              environment) == 1);
    CHECK((input_error_session.calls == std::vector<std::string>{"open", "close"}));
    CHECK(error_stream.str().find("ERROR internal input error:") != std::string::npos);
    return true;
}

bool test_output_failure_still_closes() {
    FakeSession session;
    session.execute_results.push_back(make_script({
        ExecuteResult{CommandResult{1, std::nullopt}}}));

    std::istringstream input{"statement"};
    std::ostream output{nullptr};
    std::ostringstream error;
    CliEnvironment environment{input, output, error, false};
    std::vector<std::string> arguments{"tinydbms"};
    std::vector<char*> argv{arguments[0].data()};
    CHECK(tinydbms::app::run_cli(
              1,
              argv.data(),
              "0.1.0",
              session,
              environment) == 1);
    CHECK((session.calls == std::vector<std::string>{"open", "execute", "close"}));
    return true;
}

bool test_malformed_query_result_is_rejected() {
    FakeSession session;
    ExecuteScriptResult result;
    result.outcomes.emplace_back(ExecuteResult{tinydbms::core::QueryResult{
        {tinydbms::core::ColumnHeader{"id", Type::kInt}},
        {{}}}});
    session.execute_results.push_back(std::move(result));

    std::string output;
    std::string error;
    CHECK(invoke(session, {"tinydbms"}, "query", false, output, error) == 1);
    CHECK(output == "id\n");
    CHECK(error == "ERROR internal query result row width does not match column count\n");
    CHECK((session.calls == std::vector<std::string>{"open", "execute", "close"}));
    return true;
}

}  // namespace

int main() {
    const bool passed =
        test_help_and_version_do_not_open() &&
        test_default_data_dir_and_empty_batch() &&
        test_argument_errors_do_not_open() &&
        test_batch_rendering_and_lifecycle() &&
        test_repl_continues_after_sql_error() &&
        test_open_and_close_failures() &&
        test_exceptions_close_once_and_input_failure_closes() &&
        test_output_failure_still_closes() &&
        test_malformed_query_result_is_rejected();
    return passed ? 0 : 1;
}
