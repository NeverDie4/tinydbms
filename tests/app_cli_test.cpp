#include "runner.hpp"

#include "json_check.hpp"
#include "tinydbms/core.hpp"

#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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
using tinydbms::core::ExecutionMode;
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
    std::vector<ExecutionMode> execute_modes;
    std::vector<std::size_t> execute_max_query_rows;
    // 每次 execute_script 进入时看到的取消状态；用来验证入口的令牌生命周期。
    std::vector<bool> execute_cancel_requested;
    // 非 0 时，第 N 次 execute_script 会请求该次请求的令牌（模拟信号处理器），
    // 并返回一条被取消的语句。
    std::size_t request_cancel_on_call = 0;
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
        execute_modes.push_back(request.mode);
        execute_max_query_rows.push_back(request.max_query_rows);
        execute_cancel_requested.push_back(request.cancel.cancel_requested());
        if (request_cancel_on_call != 0 &&
            request_cancel_on_call == execute_texts.size()) {
            request.cancel.request_cancel();
            ExecuteScriptResult cancelled_result;
            cancelled_result.statements.push_back(StatementResult::cancelled(
                0,
                SourceRange{SourceLocation{1, 1, 0}, SourceLocation{1, 1, 0}}));
            return cancelled_result;
        }
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
    std::string& error_text,
    bool output_is_terminal = false) {
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }

    std::istringstream input{std::move(input_text)};
    std::ostringstream output;
    std::ostringstream error;
    CliEnvironment environment{input, output, error, interactive, output_is_terminal};
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

Error make_error(
    ErrorKind kind,
    std::string message,
    std::string suggestion) {
    return Error{
        kind,
        std::nullopt,
        std::nullopt,
        std::move(message),
        std::optional<std::string>{std::move(suggestion)},
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

// ---- U2(--format json) 与 U3(--plan) 的入口测试 ----

using tinydbms::testing::JsonNode;
using tinydbms::testing::json_integer;
using tinydbms::testing::json_lines;
using tinydbms::testing::json_member;
using tinydbms::testing::json_parse;
using tinydbms::testing::json_string;

bool parse_json_lines(const std::string& text, std::vector<JsonNode>& nodes) {
    for (const std::string_view line : json_lines(text)) {
        std::size_t offset = 0;
        std::optional<JsonNode> parsed = json_parse(line, offset);
        if (!parsed.has_value()) {
            std::cerr << "JSON parse failed at offset " << offset << ": " << line << '\n';
            return false;
        }
        nodes.push_back(std::move(*parsed));
    }
    return true;
}

std::optional<std::string> member_string(const JsonNode& node, const std::string_view key) {
    const JsonNode* member = json_member(node, key);
    if (member == nullptr) {
        return std::nullopt;
    }
    return json_string(*member);
}

std::optional<std::int64_t> member_integer(const JsonNode& node, const std::string_view key) {
    const JsonNode* member = json_member(node, key);
    if (member == nullptr) {
        return std::nullopt;
    }
    return json_integer(*member);
}

bool test_format_and_plan_argument_errors() {
    const std::vector<std::vector<std::string>> invalid_arguments{
        {"tinydbms", "--format"},
        {"tinydbms", "--format", ""},
        {"tinydbms", "--format", "--plan"},
        {"tinydbms", "--format", "yaml"},
        {"tinydbms", "--format=json"},
        {"tinydbms", "--format", "json", "--format", "table"},
        {"tinydbms", "--plan", "--plan"},
        {"tinydbms", "--plan", "--help"},
        {"tinydbms", "--plan", "--version"},
    };

    for (const auto& arguments : invalid_arguments) {
        FakeSession session;
        std::string output;
        std::string error;
        CHECK(invoke(session, arguments, "", false, output, error) == 2);
        CHECK(session.calls.empty());
        CHECK(output.empty());
        // 参数错误保持纯文本，即使在 --format json 之后也不输出 JSON 对象。
        CHECK(error.find("argument error:") != std::string::npos);
        CHECK(error.find('{') == std::string::npos);
    }

    FakeSession session;
    std::string output;
    std::string error;
    CHECK(invoke(session, {"tinydbms", "--help"}, "", false, output, error) == 0);
    CHECK(output.find("--format") != std::string::npos);
    CHECK(output.find("--plan") != std::string::npos);
    return true;
}

bool test_plan_flag_is_forwarded_as_mode() {
    FakeSession default_session;
    std::string output;
    std::string error;
    CHECK(invoke(default_session, {"tinydbms"}, "SELECT 1;", false, output, error) == 0);
    CHECK((default_session.execute_modes == std::vector<ExecutionMode>{ExecutionMode::kExecute}));

    FakeSession plan_session;
    output.clear();
    error.clear();
    CHECK(invoke(
              plan_session,
              {"tinydbms", "--plan", "--format", "json"},
              "SELECT 1;",
              false,
              output,
              error) == 0);
    CHECK((plan_session.execute_modes == std::vector<ExecutionMode>{ExecutionMode::kPlanOnly}));
    CHECK((plan_session.execute_policies ==
           std::vector<ScriptErrorPolicy>{ScriptErrorPolicy::kStopOnFirstError}));

    // REPL 每条语句都按计划模式执行，提示符仍然只出现在 stderr。
    FakeSession repl_session;
    output.clear();
    error.clear();
    CHECK(invoke(repl_session, {"tinydbms", "--plan"}, "SELECT 1;\nSELECT 2;\n", true, output, error) == 0);
    CHECK((repl_session.execute_modes ==
           std::vector<ExecutionMode>{ExecutionMode::kPlanOnly, ExecutionMode::kPlanOnly}));
    CHECK(error.find("tinydbms> ") != std::string::npos);
    return true;
}

bool test_max_rows_argument_errors() {
    const std::vector<std::vector<std::string>> invalid_arguments{
        {"tinydbms", "--max-rows"},
        {"tinydbms", "--max-rows", ""},
        {"tinydbms", "--max-rows", "--plan"},
        {"tinydbms", "--max-rows", "abc"},
        {"tinydbms", "--max-rows", "12x"},
        {"tinydbms", "--max-rows", "+12"},
        {"tinydbms", "--max-rows", "-1"},
        {"tinydbms", "--max-rows", "0"},
        {"tinydbms", "--max-rows", "0x10"},
        {"tinydbms", "--max-rows", "99999999999999999999999999"},
        {"tinydbms", "--max-rows=12"},
        {"tinydbms", "--max-rows", "12", "--max-rows", "13"},
        {"tinydbms", "--max-rows", "12", "--max-rows"},
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

    FakeSession help_session;
    std::string output;
    std::string error;
    CHECK(invoke(help_session, {"tinydbms", "--help"}, "", false, output, error) == 0);
    CHECK(output.find("--max-rows") != std::string::npos);
    return true;
}

bool test_max_rows_is_forwarded() {
    FakeSession default_session;
    std::string output;
    std::string error;
    CHECK(invoke(default_session, {"tinydbms"}, "SELECT 1;", false, output, error) == 0);
    CHECK((default_session.execute_max_query_rows ==
           std::vector<std::size_t>{tinydbms::core::kMaxQueryRows}));

    FakeSession limited_session;
    output.clear();
    error.clear();
    CHECK(invoke(
              limited_session,
              {"tinydbms", "--max-rows", "7"},
              "SELECT 1;",
              false,
              output,
              error) == 0);
    CHECK((limited_session.execute_max_query_rows == std::vector<std::size_t>{7U}));

    // 与 --plan 组合不报错：计划模式不进入执行器，参数照常透传。
    FakeSession plan_session;
    output.clear();
    error.clear();
    CHECK(invoke(
              plan_session,
              {"tinydbms", "--plan", "--max-rows", "3"},
              "SELECT 1;",
              false,
              output,
              error) == 0);
    CHECK((plan_session.execute_modes == std::vector<ExecutionMode>{ExecutionMode::kPlanOnly}));
    CHECK((plan_session.execute_max_query_rows == std::vector<std::size_t>{3U}));

    // REPL 每行都沿用同一个参数值。
    FakeSession repl_session;
    output.clear();
    error.clear();
    CHECK(invoke(
              repl_session,
              {"tinydbms", "--max-rows", "5"},
              "SELECT 1;\nSELECT 2;\n",
              true,
              output,
              error) == 0);
    CHECK((repl_session.execute_max_query_rows == std::vector<std::size_t>{5U, 5U}));
    return true;
}

bool test_max_rows_limit_error_rendering() {
    const std::string message{"query materialization exceeds the maximum row count (limit 5)"};
    const std::string suggestion{"raise max_query_rows or narrow the query"};

    FakeSession table_session;
    table_session.execute_results.push_back(make_script({
        StatementResult::execution_error(
            0,
            range(1, 1, 1, 21),
            ExecuteResult{make_error(ErrorKind::kExecute, message, suggestion)}),
    }));

    std::string output;
    std::string error;
    CHECK(invoke(
              table_session,
              {"tinydbms", "--max-rows", "5"},
              "SELECT * FROM events;",
              false,
              output,
              error) == 1);
    CHECK(output.empty());
    CHECK(error ==
          "ERROR execute 1:1-1:21 " + message + "\n"
          "SUGGESTION " + suggestion + "\n");

    FakeSession json_session;
    json_session.execute_results.push_back(make_script({
        StatementResult::execution_error(
            0,
            range(1, 1, 1, 21),
            ExecuteResult{make_error(ErrorKind::kExecute, message, suggestion)}),
    }));
    output.clear();
    error.clear();
    CHECK(invoke(
              json_session,
              {"tinydbms", "--format", "json", "--max-rows", "5"},
              "SELECT * FROM events;",
              false,
              output,
              error) == 1);
    CHECK(output.empty());
    CHECK(error ==
          "{\"type\":\"error\",\"scope\":\"statement\",\"statement_index\":0,"
          "\"kind\":\"execute\",\"range\":\"1:1-1:21\","
          "\"message\":\"query materialization exceeds the maximum row count (limit 5)\","
          "\"suggestion\":\"raise max_query_rows or narrow the query\"}\n");
    return true;
}

bool test_cancelled_statement_rendering() {
    // table 格式：CANCELLED 写 stderr，本次调用按失败处理（退出码 1）。
    FakeSession table_session;
    table_session.execute_results.push_back(make_script({
        StatementResult::cancelled(0, range(1, 1, 1, 12)),
        StatementResult::cancelled(1, range(1, 13, 1, 24)),
    }));

    std::string output;
    std::string error;
    CHECK(invoke(table_session, {"tinydbms"}, "SELECT 1;\nSELECT 2;\n", false, output, error) == 1);
    CHECK(output.empty());
    CHECK(error == "CANCELLED 1:1-1:12\nCANCELLED 1:13-1:24\n");

    // JSON 格式：与 SKIPPED/INDETERMINATE 同构的 status 对象，同样写 stderr。
    FakeSession json_session;
    json_session.execute_results.push_back(make_script({
        StatementResult::cancelled(0, range(1, 1, 1, 12)),
    }));
    output.clear();
    error.clear();
    CHECK(invoke(
              json_session,
              {"tinydbms", "--format", "json"},
              "SELECT 1;\n",
              false,
              output,
              error) == 1);
    CHECK(output.empty());
    CHECK(error ==
          "{\"type\":\"status\",\"statement_index\":0,\"status\":\"cancelled\","
          "\"range\":\"1:1-1:12\"}\n");
    return true;
}

bool test_repl_uses_fresh_cancel_token_per_line() {
    FakeSession session;
    // 第一行由"信号处理器"请求取消；第二行必须使用未请求取消的新令牌。
    session.request_cancel_on_call = 1;
    session.execute_results.push_back(make_script({
        StatementResult::executed(
            0,
            range(1, 1, 1, 10),
            ExecuteResult{CommandResult{1, std::nullopt}}),
    }));

    std::string output;
    std::string error;
    CHECK(invoke(session, {"tinydbms"}, "SELECT 1;\nSELECT 2;\n", true, output, error) == 1);
    CHECK((session.execute_cancel_requested == std::vector<bool>{false, false}));
    CHECK((session.execute_texts == std::vector<std::string>{"SELECT 1;", "SELECT 2;"}));
    // 取消后 REPL 继续读取下一行；被取消的语句本身写 stderr，不影响后续结果。
    CHECK(output == "OK 1\n");
    CHECK(error.find("CANCELLED 1:1-1:1") != std::string::npos);
    return true;
}

bool test_json_query_and_command_objects() {
    FakeSession session;
    session.execute_results.push_back(make_script({
        StatementResult::executed(
            0,
            range(1, 1, 1, 20),
            ExecuteResult{QueryResult{
                {tinydbms::core::ColumnHeader{"id", Type::kInt},
                 tinydbms::core::ColumnHeader{"name", Type::kVarchar}},
                {{Value{std::int32_t{1}}, Value{std::string{"a"}}},
                 {Value{std::int32_t{2}}, Value{std::monostate{}}}}}}),
        StatementResult::executed(
            1,
            range(1, 21, 1, 40),
            ExecuteResult{CommandResult{3, std::nullopt}}),
    }));

    std::string output;
    std::string error;
    CHECK(invoke(session, {"tinydbms", "--format", "json"}, "x", false, output, error) == 0);
    CHECK(error.empty());

    const std::string expected =
        "{\"type\":\"query\",\"statement_index\":0,\"range\":\"1:1-1:20\",\"columns\":["
        "{\"name\":\"id\",\"type\":\"INT\"},{\"name\":\"name\",\"type\":\"VARCHAR\"}],"
        "\"row_count\":2,\"rows\":[[1,\"a\"],[2,null]]}\n"
        "{\"type\":\"command\",\"statement_index\":1,\"range\":\"1:21-1:40\",\"affected_rows\":3}\n";
    CHECK(output == expected);

    std::vector<JsonNode> nodes;
    CHECK(parse_json_lines(output, nodes));
    CHECK(nodes.size() == 2);
    CHECK(member_string(nodes[0], "type") == std::optional<std::string>{"query"});
    CHECK(member_integer(nodes[0], "statement_index") == std::optional<std::int64_t>{0});
    CHECK(member_string(nodes[0], "range") == std::optional<std::string>{"1:1-1:20"});
    CHECK(member_integer(nodes[0], "row_count") == std::optional<std::int64_t>{2});
    const JsonNode* rows = json_member(nodes[0], "rows");
    CHECK(rows != nullptr && rows->kind == JsonNode::Kind::kArray);
    // rows 长度必须与 row_count 一致，且每行宽度等于列数。
    CHECK(rows->items.size() == 2);
    CHECK(rows->items[0].items.size() == 2);
    CHECK(rows->items[1].items.size() == 2);
    CHECK(rows->items[1].items[1].kind == JsonNode::Kind::kNull);
    CHECK(member_string(nodes[1], "type") == std::optional<std::string>{"command"});
    CHECK(member_integer(nodes[1], "affected_rows") == std::optional<std::int64_t>{3});
    return true;
}

bool test_json_diagnostics_and_ordering() {
    Error script_error = make_error(ErrorKind::kInternal, "storage raised an exception");
    script_error.source = std::optional<SourceRange>{range(1, 1, 1, 1)};
    FakeSession session;
    session.execute_results.push_back(make_fatal_script(
        {StatementResult::executed(
             0,
             range(1, 1, 1, 20),
             ExecuteResult{CommandResult{1, std::nullopt}}),
         StatementResult::execution_indeterminate(1, range(1, 21, 1, 40)),
         StatementResult::skipped(2, range(1, 41, 1, 60))},
        script_error));

    std::string output;
    std::string error;
    CHECK(invoke(session, {"tinydbms", "--format", "json"}, "x", false, output, error) == 1);
    CHECK(output ==
          "{\"type\":\"command\",\"statement_index\":0,\"range\":\"1:1-1:20\",\"affected_rows\":1}\n");
    // script_error 插在首条 kSkippedExecution 之前，被跳过的语句用 status 对象表示。
    CHECK(error ==
          "{\"type\":\"status\",\"statement_index\":1,\"status\":\"indeterminate\","
          "\"range\":\"1:21-1:40\"}\n"
          "{\"type\":\"error\",\"scope\":\"script\",\"kind\":\"internal\","
          "\"range\":\"1:1-1:1\",\"message\":\"storage raised an exception\"}\n"
          "{\"type\":\"status\",\"statement_index\":2,\"status\":\"skipped\","
          "\"range\":\"1:41-1:60\",\"reason\":\"aborted\"}\n");

    // 没有 script_error 时，跳过原因来自错误策略。
    FakeSession policy_session;
    policy_session.execute_results.push_back(make_script({
        StatementResult::compile_error(
            0,
            range(1, 1, 1, 10),
            make_compile_error(
                CompileStage::kSyntax,
                range(1, 8, 1, 9),
                "unexpected token",
                std::string{"did you mean 'x'"},
                FixIt{range(1, 8, 1, 9), "x"})),
        StatementResult::skipped(1, range(1, 11, 1, 20)),
    }));
    output.clear();
    error.clear();
    CHECK(invoke(policy_session, {"tinydbms", "--format", "json"}, "x", false, output, error) == 1);
    CHECK(output.empty());
    CHECK(error ==
          "{\"type\":\"error\",\"scope\":\"statement\",\"statement_index\":0,"
          "\"kind\":\"compile\",\"stage\":\"syntax\",\"range\":\"1:8-1:9\","
          "\"message\":\"unexpected token\",\"suggestion\":\"did you mean 'x'\","
          "\"fix_it\":{\"range\":\"1:8-1:9\",\"replacement\":\"x\"}}\n"
          "{\"type\":\"status\",\"statement_index\":1,\"status\":\"skipped\","
          "\"range\":\"1:11-1:20\",\"reason\":\"policy\"}\n");
    return true;
}

bool test_json_analyze_status_and_partial_command() {
    FakeSession session;
    session.execute_results.push_back(make_script({
        StatementResult::execution_error(
            0,
            range(1, 1, 1, 20),
            ExecuteResult{CommandResult{2, make_error(ErrorKind::kStorage, "page write failed")}}),
        StatementResult::analysis_only(1, range(1, 21, 1, 40)),
    }));

    std::string output;
    std::string error;
    CHECK(invoke(
              session,
              {"tinydbms", "--format", "json", "--error-policy", "analyze"},
              "x",
              false,
              output,
              error) == 1);
    // 部分成功的 CommandResult 先输出已完成行数，再在 stderr 报告错误。
    CHECK(output ==
          "{\"type\":\"command\",\"statement_index\":0,\"range\":\"1:1-1:20\",\"affected_rows\":2}\n");
    CHECK(error ==
          "{\"type\":\"error\",\"scope\":\"statement\",\"statement_index\":0,\"kind\":\"storage\","
          "\"range\":\"1:1-1:20\",\"message\":\"page write failed\"}\n"
          "{\"type\":\"status\",\"statement_index\":1,\"status\":\"analyzed\","
          "\"range\":\"1:21-1:40\"}\n");
    return true;
}

bool test_json_value_mapping_and_escaping() {
    const std::string invalid_utf8{"\xFF\xFE"};
    FakeSession session;
    session.execute_results.push_back(make_script({
        StatementResult::executed(
            0,
            range(1, 1, 1, 20),
            ExecuteResult{QueryResult{
                {tinydbms::core::ColumnHeader{"text", Type::kVarchar},
                 tinydbms::core::ColumnHeader{"big", Type::kBigInt},
                 tinydbms::core::ColumnHeader{"num", Type::kDouble},
                 tinydbms::core::ColumnHeader{"flag", Type::kBoolean},
                 tinydbms::core::ColumnHeader{"nil", Type::kInt}},
                {{Value{std::string{"a\"b\\c\nd\te\x01"}},
                  Value{std::int64_t{9007199254740993LL}},
                  Value{std::numeric_limits<double>::quiet_NaN()},
                  Value{true},
                  Value{std::monostate{}}},
                 {Value{invalid_utf8},
                  Value{std::int64_t{-1}},
                  Value{std::numeric_limits<double>::infinity()},
                  Value{false},
                  Value{std::monostate{}}},
                 {Value{std::string{"中文"}},
                  Value{std::int64_t{0}},
                  Value{1.5},
                  Value{false},
                  Value{std::monostate{}}}}}}),
    }));

    std::string output;
    std::string error;
    CHECK(invoke(session, {"tinydbms", "--format", "json"}, "x", false, output, error) == 0);
    CHECK(error.empty());

    const std::string expected =
        "{\"type\":\"query\",\"statement_index\":0,\"range\":\"1:1-1:20\",\"columns\":["
        "{\"name\":\"text\",\"type\":\"VARCHAR\"},{\"name\":\"big\",\"type\":\"BIGINT\"},"
        "{\"name\":\"num\",\"type\":\"DOUBLE\"},{\"name\":\"flag\",\"type\":\"BOOLEAN\"},"
        "{\"name\":\"nil\",\"type\":\"INT\"}],\"row_count\":3,\"rows\":["
        "[\"a\\\"b\\\\c\\nd\\te\\u0001\",9007199254740993,null,true,null],"
        "[\"" "\xEF\xBF\xBD\xEF\xBF\xBD" "\",-1,null,false,null],"
        "[\"中文\",0,1.5,false,null]]}\n";
    CHECK(output == expected);

    std::vector<JsonNode> nodes;
    CHECK(parse_json_lines(output, nodes));
    CHECK(nodes.size() == 1);
    const JsonNode* rows = json_member(nodes[0], "rows");
    CHECK(rows != nullptr && rows->items.size() == 3);
    // 反斜杠转义、控制字符、非法 UTF-8 与 BIGINT 都必须按契约还原。
    CHECK(json_string(rows->items[0].items[0]) == std::optional<std::string>{"a\"b\\c\nd\te\x01"});
    CHECK(json_integer(rows->items[0].items[1]) == std::optional<std::int64_t>{9007199254740993LL});
    CHECK(rows->items[0].items[2].kind == JsonNode::Kind::kNull);
    CHECK(rows->items[1].items[0].kind == JsonNode::Kind::kString);
    CHECK(json_string(rows->items[1].items[0]) ==
          std::optional<std::string>{"\xEF\xBF\xBD\xEF\xBF\xBD"});
    CHECK(rows->items[2].items[0].kind == JsonNode::Kind::kString);
    CHECK(json_string(rows->items[2].items[0]) == std::optional<std::string>{"中文"});
    return true;
}

bool test_json_lifecycle_errors() {
    FakeSession open_session;
    open_session.open_result.error = std::optional<Error>{make_error(ErrorKind::kStorage, "cannot open data dir")};
    std::string output;
    std::string error;
    CHECK(invoke(open_session, {"tinydbms", "--format", "json"}, "", false, output, error) == 1);
    CHECK(output.empty());
    CHECK(error ==
          "{\"type\":\"error\",\"scope\":\"script\",\"kind\":\"storage\","
          "\"message\":\"cannot open data dir\"}\n");
    CHECK((open_session.calls == std::vector<std::string>{"open"}));

    FakeSession close_session;
    close_session.close_result.error = std::optional<Error>{make_error(ErrorKind::kStorage, "cannot flush")};
    output.clear();
    error.clear();
    CHECK(invoke(close_session, {"tinydbms", "--format", "json"}, "x", false, output, error) == 1);
    CHECK(error.find("{\"type\":\"error\",\"scope\":\"script\",\"kind\":\"storage\"") == 0);
    return true;
}

bool test_json_repl_keeps_prompt_on_stderr() {
    FakeSession session;
    session.execute_results.push_back(make_script({
        StatementResult::executed(
            0, range(1, 1, 1, 10), ExecuteResult{CommandResult{1, std::nullopt}}),
    }));

    std::string output;
    std::string error;
    CHECK(invoke(session, {"tinydbms", "--format", "json"}, "CREATE TABLE t(id INT);\n", true, output, error) == 0);
    CHECK(output ==
          "{\"type\":\"command\",\"statement_index\":0,\"range\":\"1:1-1:10\",\"affected_rows\":1}\n");
    CHECK(error.find("tinydbms> ") != std::string::npos);
    CHECK(error.find('{') == std::string::npos);
    return true;
}

bool test_plan_only_statement_rendering() {
    tinydbms::core::QueryResult plan;
    plan.columns.push_back(tinydbms::core::ColumnHeader{"plan", Type::kVarchar});
    plan.rows.push_back({Value{std::string{"QueryPlan outputs=[id:INT]"}}});
    plan.rows.push_back({Value{std::string{"  Project [id#0]"}}});

    FakeSession table_session;
    table_session.execute_results.push_back(make_script(
        {StatementResult::plan_only(0, range(1, 1, 1, 10), plan)}));
    std::string output;
    std::string error;
    CHECK(invoke(table_session, {"tinydbms", "--plan"}, "x", false, output, error) == 0);
    CHECK(output ==
          "plan\nQueryPlan outputs=[id:INT]\n  Project [id#0]\n");
    CHECK(error.empty());

    FakeSession json_session;
    json_session.execute_results.push_back(make_script(
        {StatementResult::plan_only(0, range(1, 1, 1, 10), plan)}));
    output.clear();
    error.clear();
    CHECK(invoke(json_session, {"tinydbms", "--plan", "--format", "json"}, "x", false, output, error) == 0);
    CHECK(output ==
          "{\"type\":\"query\",\"statement_index\":0,\"range\":\"1:1-1:10\",\"columns\":["
          "{\"name\":\"plan\",\"type\":\"VARCHAR\"}],\"row_count\":2,\"rows\":["
          "[\"QueryPlan outputs=[id:INT]\"],[\"  Project [id#0]\"]]}\n");
    return true;
}

// pretty 用例共用的查询结果：两列文本 + 一列数值，含 NULL、CJK 与多位数。
tinydbms::core::QueryResult make_pretty_query() {
    tinydbms::core::QueryResult query;
    query.columns = {
        tinydbms::core::ColumnHeader{"id", Type::kInt},
        tinydbms::core::ColumnHeader{"name", Type::kVarchar},
        tinydbms::core::ColumnHeader{"score", Type::kDouble}};
    query.rows.push_back(
        {Value{std::int32_t{1}}, Value{std::string{"一甲"}}, Value{98.5}});
    query.rows.push_back(
        {Value{std::int32_t{2}}, Value{std::string{"乙"}}, Value{std::monostate{}}});
    query.rows.push_back(
        {Value{std::int32_t{12}}, Value{std::string{"abc"}}, Value{7.0}});
    return query;
}

// 只含一条查询语句的脚本结果。
ExecuteScriptResult make_pretty_script() {
    return make_script({StatementResult::executed(
        0, range(1, 1, 1, 20), ExecuteResult{make_pretty_query()})});
}

FakeSession make_pretty_session() {
    FakeSession session;
    session.execute_results.push_back(make_pretty_script());
    return session;
}

bool test_pretty_query_and_command_rendering() {
    FakeSession session;
    session.execute_results.push_back(make_script({
        StatementResult::executed(
            0, range(1, 1, 1, 20), ExecuteResult{make_pretty_query()}),
        StatementResult::executed(
            1, range(1, 21, 1, 40), ExecuteResult{CommandResult{1, std::nullopt}}),
    }));

    std::string output;
    std::string error;
    CHECK(invoke(session, {"tinydbms", "--format", "pretty"}, "x", false, output, error) == 0);
    CHECK(error.empty());
    CHECK(output ==
          "┌────┬──────┬───────┐\n"
          "│ id │ name │ score │\n"
          "├────┼──────┼───────┤\n"
          "│  1 │ 一甲 │  98.5 │\n"
          "│  2 │ 乙   │  NULL │\n"
          "│ 12 │ abc  │   7.0 │\n"
          "└────┴──────┴───────┘\n"
          "3 rows\n"
          "OK, 1 row affected\n");
    return true;
}

bool test_pretty_empty_result_and_truncation() {
    // 空结果：只打印表头、边框与 0 rows，不打印任何数据行。
    tinydbms::core::QueryResult empty_query;
    empty_query.columns = {
        tinydbms::core::ColumnHeader{"id", Type::kInt},
        tinydbms::core::ColumnHeader{"name", Type::kVarchar}};

    FakeSession empty_session;
    empty_session.execute_results.push_back(make_script({
        StatementResult::executed(
            0, range(1, 1, 1, 20), ExecuteResult{std::move(empty_query)}),
    }));
    std::string output;
    std::string error;
    CHECK(invoke(
              empty_session,
              {"tinydbms", "--format", "pretty"},
              "x",
              false,
              output,
              error) == 0);
    CHECK(error.empty());
    CHECK(output ==
          "┌────┬──────┐\n"
          "│ id │ name │\n"
          "├────┼──────┤\n"
          "└────┴──────┘\n"
          "0 rows\n");

    // 超宽单元格按显示宽度截断到 48 列并追加省略号；表头过长时同样截断。
    const std::string long_text(60, 'x');
    const std::string truncated = std::string(47, 'x') + "…";

    tinydbms::core::QueryResult wide_query;
    wide_query.columns = {tinydbms::core::ColumnHeader{"blob", Type::kVarchar}};
    wide_query.rows.push_back({Value{std::string{long_text}}});

    FakeSession wide_session;
    wide_session.execute_results.push_back(make_script({
        StatementResult::executed(
            0, range(1, 1, 1, 20), ExecuteResult{std::move(wide_query)}),
    }));
    output.clear();
    error.clear();
    CHECK(invoke(
              wide_session,
              {"tinydbms", "--format", "pretty"},
              "x",
              false,
              output,
              error) == 0);
    const std::string dashes = []() {
        std::string value;
        for (int index = 0; index < 50; ++index) {
            value += "─";
        }
        return value;
    }();
    CHECK(output ==
          "┌" + dashes + "┐\n" +
          "│ blob" + std::string(45, ' ') + "│\n" +
          "├" + dashes + "┤\n" +
          "│ " + truncated + " │\n" +
          "└" + dashes + "┘\n" +
          "1 row\n");
    CHECK(output.find(long_text) == std::string::npos);
    return true;
}

bool test_pretty_diagnostics_match_table_format() {
    // pretty 只替换成功结果的呈现：错误与状态行与 table 模式逐字节一致。
    FakeSession session;
    session.execute_results.push_back(make_script({
        StatementResult::compile_error(
            0,
            range(1, 1, 1, 12),
            make_compile_error(
                CompileStage::kSemantic,
                range(1, 8, 1, 12),
                "table 't' does not exist")),
        StatementResult::skipped(1, range(1, 13, 1, 20)),
    }));

    std::string pretty_output;
    std::string pretty_error;
    CHECK(invoke(
              session,
              {"tinydbms", "--format", "pretty"},
              "x",
              false,
              pretty_output,
              pretty_error) == 1);
    CHECK(pretty_output.empty());
    CHECK(pretty_error ==
          "ERROR semantic 1:8-1:12 table 't' does not exist\n"
          "SKIPPED 1:13-1:20 policy\n");

    FakeSession table_session;
    table_session.execute_results.push_back(make_script({
        StatementResult::compile_error(
            0,
            range(1, 1, 1, 12),
            make_compile_error(
                CompileStage::kSemantic,
                range(1, 8, 1, 12),
                "table 't' does not exist")),
        StatementResult::skipped(1, range(1, 13, 1, 20)),
    }));
    std::string table_output;
    std::string table_error;
    CHECK(invoke(
              table_session,
              {"tinydbms", "--format", "table"},
              "x",
              false,
              table_output,
              table_error) == 1);
    CHECK(table_output == pretty_output);
    CHECK(table_error == pretty_error);
    return true;
}

bool test_pretty_rejects_malformed_result() {
    tinydbms::core::QueryResult query;
    query.columns = {
        tinydbms::core::ColumnHeader{"id", Type::kInt},
        tinydbms::core::ColumnHeader{"name", Type::kVarchar}};
    query.rows.push_back({Value{std::int32_t{1}}});

    FakeSession session;
    session.execute_results.push_back(make_script({
        StatementResult::executed(
            0, range(1, 1, 1, 20), ExecuteResult{std::move(query)}),
    }));

    std::string output;
    std::string error;
    CHECK(invoke(session, {"tinydbms", "--format", "pretty"}, "x", false, output, error) == 1);
    CHECK(output.empty());
    CHECK(error ==
          "ERROR internal query result row width does not match column count\n");
    return true;
}

bool test_default_format_depends_on_terminal() {
    // 未显式指定 --format：stdout 是终端用 pretty，否则保持 table（TSV）。
    FakeSession terminal_session = make_pretty_session();
    std::string terminal_output;
    std::string terminal_error;
    CHECK(invoke(
              terminal_session, {"tinydbms"}, "x", false, terminal_output, terminal_error, true) == 0);
    CHECK(terminal_output.find("│ id │ name │ score │") != std::string::npos);
    CHECK(terminal_output.find("3 rows\n") != std::string::npos);

    FakeSession piped_session = make_pretty_session();
    std::string piped_output;
    std::string piped_error;
    CHECK(invoke(
              piped_session, {"tinydbms"}, "x", false, piped_output, piped_error, false) == 0);
    CHECK(piped_output == "id\tname\tscore\n1\t一甲\t98.5\n2\t乙\tNULL\n12\tabc\t7.0\n");

    // 显式 --format 覆盖终端判定：终端下也能拿到 TSV。
    FakeSession explicit_session = make_pretty_session();
    std::string explicit_output;
    std::string explicit_error;
    CHECK(invoke(
              explicit_session,
              {"tinydbms", "--format", "table"},
              "x",
              false,
              explicit_output,
              explicit_error,
              true) == 0);
    CHECK(explicit_output == piped_output);
    return true;
}

bool test_pretty_argument_errors() {
    const std::vector<std::vector<std::string>> invalid_arguments{
        {"tinydbms", "--format", "prettyx"},
        {"tinydbms", "--format", "html"},
        {"tinydbms", "--format", "pretty", "--format", "table"},
        {"tinydbms", "--time", "--time"},
        {"tinydbms", "--time"},
    };

    for (std::size_t index = 0; index < invalid_arguments.size(); ++index) {
        const auto& arguments = invalid_arguments[index];
        FakeSession session;
        std::string output;
        std::string error;
        // 最后一组是合法参数，用来确认 --time 单独出现不会被误判。
        const int expected = index + 1 == invalid_arguments.size() ? 0 : 2;
        CHECK(invoke(session, arguments, "x", false, output, error) == expected);
        if (expected == 2) {
            CHECK(session.calls.empty());
            CHECK(error.find("argument error:") != std::string::npos);
        }
    }
    return true;
}

// TIME 行格式：`TIME <scope> <毫秒> ms`，毫秒固定三位小数。REPL 的提示符没有换行，
// 所以这里在整段文本里定位 TIME 片段，而不是按行切分。
bool contains_time_line(const std::string& text, const std::string& scope) {
    const std::string prefix = "TIME " + scope + " ";
    const std::size_t begin = text.find(prefix);
    if (begin == std::string::npos) {
        return false;
    }
    const std::size_t end = text.find('\n', begin);
    if (end == std::string::npos) {
        return false;
    }
    const std::string line = text.substr(begin, end - begin);
    if (line.size() < prefix.size() + 6U ||
        line.compare(line.size() - 3, 3, " ms") != 0) {
        return false;
    }
    const std::string value = line.substr(prefix.size(), line.size() - prefix.size() - 3);
    const std::size_t dot = value.find('.');
    if (dot == std::string::npos || value.size() - dot != 4U) {
        return false;
    }
    return value.find_first_not_of("0123456789.") == std::string::npos;
}

bool test_time_flag_writes_stderr_only() {
    // 批处理：stdout 与不加 --time 时逐字节相同，stderr 只有一行 TIME script。
    FakeSession plain_session = make_pretty_session();
    std::string plain_output;
    std::string plain_error;
    CHECK(invoke(
              plain_session, {"tinydbms", "--format", "json"}, "x", false, plain_output, plain_error) == 0);
    CHECK(plain_error.empty());

    FakeSession timed_session = make_pretty_session();
    std::string timed_output;
    std::string timed_error;
    CHECK(invoke(
              timed_session,
              {"tinydbms", "--format", "json", "--time"},
              "x",
              false,
              timed_output,
              timed_error) == 0);
    CHECK(timed_output == plain_output);
    CHECK(timed_error.rfind("TIME script ", 0) == 0);
    CHECK(contains_time_line(timed_error, "script"));
    CHECK(count_occurrences(timed_error, "TIME ") == 1);

    // REPL：每读取一行输出一条 TIME line <序号>，序号从 1 开始。
    FakeSession repl_plain;
    repl_plain.execute_results.push_back(make_script({
        StatementResult::executed(
            0, range(1, 1, 1, 10), ExecuteResult{CommandResult{1, std::nullopt}}),
    }));
    repl_plain.execute_results.push_back(make_script({
        StatementResult::executed(
            0, range(1, 1, 1, 10), ExecuteResult{CommandResult{2, std::nullopt}}),
    }));
    std::string repl_output;
    std::string repl_error;
    CHECK(invoke(
              repl_plain,
              {"tinydbms", "--format", "pretty"},
              "a\nb\n",
              true,
              repl_output,
              repl_error) == 0);
    CHECK(repl_output == "OK, 1 row affected\nOK, 2 rows affected\n");
    CHECK(repl_error == "tinydbms> tinydbms> tinydbms> ");

    FakeSession repl_timed;
    repl_timed.execute_results.push_back(make_script({
        StatementResult::executed(
            0, range(1, 1, 1, 10), ExecuteResult{CommandResult{1, std::nullopt}}),
    }));
    repl_timed.execute_results.push_back(make_script({
        StatementResult::executed(
            0, range(1, 1, 1, 10), ExecuteResult{CommandResult{2, std::nullopt}}),
    }));
    std::string repl_timed_output;
    std::string repl_timed_error;
    CHECK(invoke(
              repl_timed,
              {"tinydbms", "--format", "pretty", "--time"},
              "a\nb\n",
              true,
              repl_timed_output,
              repl_timed_error) == 0);
    CHECK(repl_timed_output == repl_output);
    CHECK(contains_time_line(repl_timed_error, "line 1"));
    CHECK(contains_time_line(repl_timed_error, "line 2"));
    CHECK(count_occurrences(repl_timed_error, "TIME ") == 2);
    // 提示符数量不变：TIME 行不改变 REPL 的交互文本。
    CHECK(count_occurrences(repl_timed_error, "tinydbms> ") == 3);
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
        test_query_result_escaping() &&
        test_format_and_plan_argument_errors() &&
        test_plan_flag_is_forwarded_as_mode() &&
        test_max_rows_argument_errors() &&
        test_max_rows_is_forwarded() &&
        test_max_rows_limit_error_rendering() &&
        test_cancelled_statement_rendering() &&
        test_repl_uses_fresh_cancel_token_per_line() &&
        test_json_query_and_command_objects() &&
        test_json_diagnostics_and_ordering() &&
        test_json_analyze_status_and_partial_command() &&
        test_json_value_mapping_and_escaping() &&
        test_json_lifecycle_errors() &&
        test_json_repl_keeps_prompt_on_stderr() &&
        test_plan_only_statement_rendering() &&
        test_pretty_query_and_command_rendering() &&
        test_pretty_empty_result_and_truncation() &&
        test_pretty_diagnostics_match_table_format() &&
        test_pretty_rejects_malformed_result() &&
        test_default_format_depends_on_terminal() &&
        test_pretty_argument_errors() &&
        test_time_flag_writes_stderr_only();
    return passed ? 0 : 1;
}
