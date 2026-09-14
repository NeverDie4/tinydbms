#include "runner.hpp"

#include "tinydbms/core.hpp"

#include <cstdint>
#include <deque>
#include <iostream>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using tinydbms::CompileStage;
using tinydbms::FixIt;
using tinydbms::SourceLocation;
using tinydbms::SourceRange;
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
using tinydbms::core::QueryResult;
using tinydbms::core::ScriptErrorPolicy;
using tinydbms::core::StatementResult;

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
    std::vector<ScriptErrorPolicy> execute_policies;
    std::string opened_data_dir;
    bool throw_on_execute = false;
    bool throw_bad_alloc_on_execute = false;
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
        execute_policies.push_back(request.error_policy);
        if (throw_bad_alloc_on_execute) {
            throw std::bad_alloc{};
        }
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

SourceRange range(
    int begin_line,
    int begin_column,
    int end_line,
    int end_column) {
    return SourceRange{
        SourceLocation{begin_line, begin_column, 0},
        SourceLocation{end_line, end_column, 0}};
}

Error make_error(ErrorKind kind, std::string message) {
    return Error{
        kind,
        std::nullopt,
        std::nullopt,
        std::move(message),
        std::nullopt,
        std::nullopt};
}

Error make_compile_error(
    CompileStage stage,
    SourceRange source,
    std::string message,
    std::optional<std::string> suggestion = std::nullopt,
    std::optional<FixIt> fix_it = std::nullopt) {
    return Error{
        ErrorKind::kCompile,
        stage,
        std::optional<SourceRange>{source},
        std::move(message),
        std::move(suggestion),
        std::move(fix_it)};
}

ExecuteScriptResult make_script(std::vector<StatementResult> statements) {
    ExecuteScriptResult result;
    result.statements = std::move(statements);
    return result;
}

ExecuteScriptResult make_fatal_script(
    std::vector<StatementResult> statements,
    Error script_error) {
    ExecuteScriptResult result;
    result.statements = std::move(statements);
    result.script_error = std::move(script_error);
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
    CHECK(output.find("--error-policy") != std::string::npos);
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
    CHECK((session.execute_policies ==
           std::vector<ScriptErrorPolicy>{ScriptErrorPolicy::kStopOnFirstError}));
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
        {"tinydbms", "--error-policy"},
        {"tinydbms", "--error-policy", ""},
        {"tinydbms", "--error-policy", "--help"},
        {"tinydbms", "--error-policy", "sometimes"},
        {"tinydbms", "--error-policy=analyze"},
        {"tinydbms", "--error-policy", "stop", "--error-policy", "analyze"},
        {"tinydbms", "--error-policy", "stop", "--error-policy"},
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

bool test_error_policy_is_forwarded() {
    FakeSession default_session;
    std::string output;
    std::string error;
    CHECK(invoke(default_session, {"tinydbms"}, "", false, output, error) == 0);
    CHECK((default_session.execute_policies ==
           std::vector<ScriptErrorPolicy>{ScriptErrorPolicy::kStopOnFirstError}));

    FakeSession analyze_session;
    output.clear();
    error.clear();
    CHECK(invoke(
              analyze_session,
              {"tinydbms", "--error-policy", "analyze"},
              "",
              false,
              output,
              error) == 0);
    CHECK((analyze_session.execute_policies ==
           std::vector<ScriptErrorPolicy>{ScriptErrorPolicy::kAnalyzeRemaining}));

    FakeSession stop_session;
    output.clear();
    error.clear();
    CHECK(invoke(
              stop_session,
              {"tinydbms", "--error-policy", "stop"},
              "",
              false,
              output,
              error) == 0);
    CHECK((stop_session.execute_policies ==
           std::vector<ScriptErrorPolicy>{ScriptErrorPolicy::kStopOnFirstError}));
    return true;
}

bool test_batch_stop_policy_rendering() {
    FakeSession session;
    session.execute_results.push_back(make_script({
        StatementResult::executed(
            0,
            range(1, 1, 1, 6),
            ExecuteResult{CommandResult{2, std::nullopt}}),
        StatementResult::compile_error(
            1,
            range(2, 1, 2, 18),
            make_compile_error(
                CompileStage::kSyntax,
                range(2, 1, 2, 8),
                "bad\nmessage",
                std::string{"did you mean \"insert\"?"},
                FixIt{range(2, 1, 2, 7), "insert"})),
        StatementResult::skipped(2, range(3, 1, 3, 4)),
    }));

    std::string output;
    std::string error;
    CHECK(invoke(
              session,
              {"tinydbms", "--data-dir", "测试目录"},
              "first;\nsecond;\nthird;\n",
              false,
              output,
              error) == 1);
    CHECK(session.opened_data_dir == "测试目录");
    CHECK(session.execute_texts.size() == 1);
    CHECK(session.execute_texts.front() == "first;\nsecond;\nthird;\n");
    CHECK((session.calls == std::vector<std::string>{"open", "execute", "close"}));
    CHECK(output == "OK 2\n");
    CHECK(
        error ==
        "ERROR syntax 2:1-2:8 bad\\nmessage\n"
        "SUGGESTION did you mean \"insert\"?\n"
        "FIX 2:1-2:7 insert\n"
        "SKIPPED 3:1-3:4 policy\n");
    return true;
}

bool test_batch_fatal_abort_rendering() {
    FakeSession session;
    session.execute_results.push_back(make_fatal_script(
        {
            StatementResult::executed(
                0,
                range(1, 1, 1, 6),
                ExecuteResult{CommandResult{2, std::nullopt}}),
            StatementResult::execution_error(
                1,
                range(2, 1, 2, 12),
                ExecuteResult{CommandResult{
                    1,
                    make_error(ErrorKind::kStorage, "partial delete")}}),
            StatementResult::execution_indeterminate(2, range(3, 1, 3, 9)),
            StatementResult::skipped(3, range(4, 1, 4, 5)),
        },
        Error{
            ErrorKind::kInternal,
            std::nullopt,
            std::optional<SourceRange>{range(3, 1, 3, 9)},
            "storage threw during execution",
            std::nullopt,
            std::nullopt}));

    std::string output;
    std::string error;
    CHECK(invoke(session, {"tinydbms"}, "first;\n", false, output, error) == 1);
    CHECK(output == "OK 2\nOK 1\n");
    CHECK(
        error ==
        "ERROR storage 2:1-2:12 partial delete\n"
        "INDETERMINATE 3:1-3:9\n"
        "ERROR internal 3:1-3:9 storage threw during execution\n"
        "SKIPPED 4:1-4:5 aborted\n");
    return true;
}

bool test_batch_analyze_rendering() {
    FakeSession session;
    session.execute_results.push_back(make_script({
        StatementResult::executed(
            0,
            range(1, 1, 1, 7),
            ExecuteResult{CommandResult{0, std::nullopt}}),
        StatementResult::analysis_error(
            1,
            range(2, 1, 2, 9),
            make_error(ErrorKind::kAnalysis, "unknown table events")),
        StatementResult::analysis_only(2, range(3, 1, 3, 6)),
    }));

    std::string output;
    std::string error;
    CHECK(invoke(
              session,
              {"tinydbms", "--error-policy", "analyze"},
              "first;\n",
              false,
              output,
              error) == 1);
    CHECK(output == "OK 0\n");
    CHECK(
        error ==
        "ERROR analysis 2:1-2:9 unknown table events\n"
        "ANALYZED 3:1-3:6\n");
    return true;
}

bool test_script_level_error_labels() {
    FakeSession compile_session;
    compile_session.execute_results.push_back(make_fatal_script(
        {},
        Error{
            ErrorKind::kCompile,
            std::optional<CompileStage>{CompileStage::kLex},
            std::optional<SourceRange>{range(1, 1, 1, 1)},
            "SQL text exceeds maximum length",
            std::nullopt,
            std::nullopt}));
    std::string output;
    std::string error;
    CHECK(invoke(compile_session, {"tinydbms"}, "x;", false, output, error) == 1);
    CHECK(output.empty());
    CHECK(error == "ERROR compile 1:1-1:1 SQL text exceeds maximum length\n");

    FakeSession internal_session;
    internal_session.execute_results.push_back(make_fatal_script(
        {},
        Error{
            ErrorKind::kInternal,
            std::nullopt,
            std::optional<SourceRange>{range(1, 1, 1, 1)},
            "compiler raised an exception: boom",
            std::nullopt,
            std::nullopt}));
    output.clear();
    error.clear();
    CHECK(invoke(internal_session, {"tinydbms"}, "x;", false, output, error) == 1);
    CHECK(output.empty());
    CHECK(error == "ERROR internal 1:1-1:1 compiler raised an exception: boom\n");
    return true;
}

bool test_repl_continues_after_sql_error() {
    FakeSession session;
    session.execute_results.push_back(make_script({
        StatementResult::execution_error(
            0,
            range(1, 1, 1, 5),
            ExecuteResult{make_error(ErrorKind::kExecute, "first failure")})}));
    session.execute_results.push_back(make_script({
        StatementResult::executed(
            0,
            range(1, 1, 1, 6),
            ExecuteResult{CommandResult{1, std::nullopt}})}));

    std::string output;
    std::string error;
    CHECK(invoke(session, {"tinydbms"}, "first\nsecond\n", true, output, error) == 1);
    CHECK((session.execute_texts == std::vector<std::string>{"first", "second"}));
    CHECK((session.calls == std::vector<std::string>{"open", "execute", "execute", "close"}));
    CHECK(output == "OK 1\n");
    CHECK(count_occurrences(error, "tinydbms> ") == 3);
    CHECK(error.find("ERROR execute 1:1-1:5 first failure\n") != std::string::npos);
    return true;
}

bool test_repl_stops_after_script_error() {
    FakeSession session;
    session.execute_results.push_back(make_script({
        StatementResult::execution_error(
            0,
            range(1, 1, 1, 6),
            ExecuteResult{make_error(ErrorKind::kExecute, "structured failure")})}));
    session.execute_results.push_back(make_fatal_script(
        {StatementResult::execution_indeterminate(0, range(1, 1, 1, 7))},
        Error{
            ErrorKind::kInternal,
            std::nullopt,
            std::optional<SourceRange>{range(1, 1, 1, 7)},
            "storage threw",
            std::nullopt,
            std::nullopt}));
    session.execute_results.push_back(make_script({
        StatementResult::executed(
            0,
            range(1, 1, 1, 6),
            ExecuteResult{CommandResult{1, std::nullopt}})}));

    std::string output;
    std::string error;
    CHECK(invoke(session, {"tinydbms"}, "first\nsecond\nthird\n", true, output, error) == 1);
    CHECK((session.execute_texts == std::vector<std::string>{"first", "second"}));
    CHECK((session.calls == std::vector<std::string>{"open", "execute", "execute", "close"}));
    CHECK(output.empty());
    CHECK(count_occurrences(error, "tinydbms> ") == 2);
    CHECK(
        error.find(
            "INDETERMINATE 1:1-1:7\n"
            "ERROR internal 1:1-1:7 storage threw\n") != std::string::npos);
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
        StatementResult::executed(
            0,
            range(1, 1, 1, 6),
            ExecuteResult{CommandResult{0, std::nullopt}})}));
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

    // bad_alloc 越过公共 API 时同样按致命错误处理并 best-effort close。
    FakeSession bad_alloc_session;
    bad_alloc_session.throw_bad_alloc_on_execute = true;
    output.clear();
    error.clear();
    CHECK(invoke(bad_alloc_session, {"tinydbms"}, "statement", false, output, error) == 1);
    CHECK((bad_alloc_session.calls == std::vector<std::string>{"open", "execute", "close"}));
    CHECK(error.find("ERROR internal application exception: std::bad_alloc\n") !=
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
        StatementResult::executed(
            0,
            range(1, 1, 1, 6),
            ExecuteResult{CommandResult{1, std::nullopt}})}));

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
    session.execute_results.push_back(make_script({
        StatementResult::executed(
            0,
            range(1, 1, 1, 6),
            ExecuteResult{QueryResult{
                {tinydbms::core::ColumnHeader{"id", Type::kInt}},
                {{}}}})}));

    std::string output;
    std::string error;
    CHECK(invoke(session, {"tinydbms"}, "query", false, output, error) == 1);
    CHECK(output == "id\n");
    CHECK(error == "ERROR internal query result row width does not match column count\n");
    CHECK((session.calls == std::vector<std::string>{"open", "execute", "close"}));
    return true;
}

bool test_query_result_escaping() {
    FakeSession session;
    session.execute_results.push_back(make_script({
        StatementResult::executed(
            0,
            range(1, 1, 1, 6),
            ExecuteResult{QueryResult{
                {tinydbms::core::ColumnHeader{"name", Type::kVarchar},
                 tinydbms::core::ColumnHeader{"count", Type::kInt}},
                {{Value{std::string{"a\tb\n\\\r"}}, Value{std::int32_t{-3}}}}}})}));

    std::string output;
    std::string error;
    CHECK(invoke(session, {"tinydbms"}, "query", false, output, error) == 0);
    std::string expected_output = "name\tcount\na";
    expected_output += "\\t";
    expected_output += "b\\n";
    expected_output.append(3, '\\');
    expected_output += "r\t-3\n";
    CHECK(output == expected_output);
    CHECK(error.empty());
    return true;
}

}  // namespace

int main() {
    const bool passed =
        test_help_and_version_do_not_open() &&
        test_default_data_dir_and_empty_batch() &&
        test_argument_errors_do_not_open() &&
        test_error_policy_is_forwarded() &&
        test_batch_stop_policy_rendering() &&
        test_batch_fatal_abort_rendering() &&
        test_batch_analyze_rendering() &&
        test_script_level_error_labels() &&
        test_repl_continues_after_sql_error() &&
        test_repl_stops_after_script_error() &&
        test_open_and_close_failures() &&
        test_exceptions_close_once_and_input_failure_closes() &&
        test_output_failure_still_closes() &&
        test_malformed_query_result_is_rejected() &&
        test_query_result_escaping();
    return passed ? 0 : 1;
}
