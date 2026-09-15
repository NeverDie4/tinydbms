#include "runner.hpp"

#include "arguments.hpp"
#include "input.hpp"
#include "output.hpp"

#include <exception>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace tinydbms::app {
namespace {

void report_error_noexcept(
    std::ostream& output,
    tinydbms::core::Error error) noexcept {
    try {
        (void)write_error(error, output);
    } catch (...) {
        // A failed error stream cannot be repaired by the runner.
    }
}

void report_exception_noexcept(
    std::ostream& output,
    std::string_view prefix,
    const std::exception& exception) noexcept {
    std::string message;
    try {
        message = std::string{prefix};
        message += exception.what() == nullptr ? "unknown exception" : exception.what();
    } catch (...) {
        message = "application exception";
    }
    report_error_noexcept(
        output,
        tinydbms::core::Error{
            tinydbms::core::ErrorKind::kInternal,
            std::nullopt,
            std::nullopt,
            std::move(message),
            std::nullopt,
            std::nullopt});
}

void report_unknown_exception_noexcept(std::ostream& output) noexcept {
    report_error_noexcept(
        output,
        tinydbms::core::Error{
            tinydbms::core::ErrorKind::kInternal,
            std::nullopt,
            std::nullopt,
            "application raised an unknown exception",
            std::nullopt,
            std::nullopt});
}

bool close_once(Session& session, bool& opened, std::ostream& error_output) noexcept {
    if (!opened) {
        return true;
    }
    opened = false;

    try {
        const tinydbms::core::CloseDatabaseResult result = session.close();
        if (!result.error.has_value()) {
            return true;
        }
        try {
            (void)write_error(*result.error, error_output);
            return false;
        } catch (...) {
            return false;
        }
    } catch (const std::exception& exception) {
        report_exception_noexcept(error_output, "close exception: ", exception);
        return false;
    } catch (...) {
        report_unknown_exception_noexcept(error_output);
        return false;
    }
}

bool write_argument_error(
    const ArgumentError& error,
    CliEnvironment& environment) {
    environment.error << "argument error: " << escape_text(error.message) << '\n';
    if (!environment.error) {
        return false;
    }
    return write_help(environment.error);
}

void report_input_error(CliEnvironment& environment, std::string_view message) {
    report_error_noexcept(
        environment.error,
        tinydbms::core::Error{
            tinydbms::core::ErrorKind::kInternal,
            std::nullopt,
            std::nullopt,
            std::string{"input error: "} + std::string{message},
            std::nullopt,
            std::nullopt});
}

RenderResult render_script_result(
    const tinydbms::core::ExecuteScriptResult& result,
    CliEnvironment& environment) {
    RenderResult aggregate;
    bool script_error_written = false;

    for (const auto& statement : result.statements) {
        if (!script_error_written && result.script_error.has_value() &&
            statement.status() == tinydbms::core::StatementStatus::kSkippedExecution) {
            if (!write_error(*result.script_error, environment.error)) {
                aggregate.output_ok = false;
                return aggregate;
            }
            script_error_written = true;
        }

        const RenderResult current = render_statement_result(
            statement,
            result.script_error.has_value(),
            environment.output,
            environment.error);
        aggregate.output_ok = aggregate.output_ok && current.output_ok;
        aggregate.had_error = aggregate.had_error || current.had_error;
        if (!current.output_ok) {
            break;
        }
    }

    if (aggregate.output_ok && !script_error_written && result.script_error.has_value()) {
        if (!write_error(*result.script_error, environment.error)) {
            aggregate.output_ok = false;
        }
    }
    aggregate.had_error = aggregate.had_error || result.script_error.has_value();
    return aggregate;
}

bool run_batch(
    Session& session,
    CliEnvironment& environment,
    tinydbms::core::ScriptErrorPolicy policy,
    bool& failed) {
    std::string text;
    std::string input_error;
    if (!read_batch(environment.input, text, input_error)) {
        failed = true;
        report_input_error(environment, input_error);
        return false;
    }

    tinydbms::core::ExecuteScriptRequest request;
    request.text = std::move(text);
    request.error_policy = policy;
    const tinydbms::core::ExecuteScriptResult result = session.execute_script(request);
    const RenderResult rendered = render_script_result(result, environment);
    failed = failed || rendered.had_error || !rendered.output_ok;
    return rendered.output_ok;
}

bool run_repl(
    Session& session,
    CliEnvironment& environment,
    tinydbms::core::ScriptErrorPolicy policy,
    bool& failed) {
    for (;;) {
        environment.error << "tinydbms> " << std::flush;
        if (!environment.error) {
            failed = true;
            return false;
        }

        std::string line;
        bool reached_eof = false;
        std::string input_error;
        if (!read_line(environment.input, line, reached_eof, input_error)) {
            failed = true;
            report_input_error(environment, input_error);
            return false;
        }
        if (reached_eof) {
            return true;
        }

        tinydbms::core::ExecuteScriptRequest request;
        request.text = std::move(line);
        request.error_policy = policy;
        const tinydbms::core::ExecuteScriptResult result =
            session.execute_script(request);

        const RenderResult rendered = render_script_result(result, environment);
        failed = failed || rendered.had_error || !rendered.output_ok;
        if (!rendered.output_ok) {
            return false;
        }
        if (result.script_error.has_value()) {
            // 任何 script_error 都按 REPL 致命处理：停止读取后续输入，
            // 由 run_cli 统一 best-effort close 并返回退出码 1。
            return true;
        }
    }
}

}  // namespace

int run_cli(
    int argc,
    char* const argv[],
    std::string_view version,
    Session& session,
    CliEnvironment& environment) {
    bool opened = false;
    try {
        const ParseArgumentsResult parsed_result = parse_arguments(argc, argv);
        if (const auto* argument_error = std::get_if<ArgumentError>(&parsed_result);
            argument_error != nullptr) {
            return write_argument_error(*argument_error, environment) ? 2 : 1;
        }

        const ParsedArguments& arguments = std::get<ParsedArguments>(parsed_result);
        if (arguments.action == CliAction::kHelp) {
            return write_help(environment.output) ? 0 : 1;
        }
        if (arguments.action == CliAction::kVersion) {
            return write_version(environment.output, version) ? 0 : 1;
        }

        const tinydbms::core::OpenDatabaseResult opened_result =
            session.open(tinydbms::core::OpenDatabaseRequest{arguments.data_dir});
        if (opened_result.error.has_value()) {
            (void)write_error(*opened_result.error, environment.error);
            return 1;
        }
        opened = true;

        const tinydbms::core::ScriptErrorPolicy policy =
            arguments.error_policy == ErrorPolicy::kAnalyze
            ? tinydbms::core::ScriptErrorPolicy::kAnalyzeRemaining
            : tinydbms::core::ScriptErrorPolicy::kStopOnFirstError;

        bool failed = false;
        if (environment.interactive) {
            (void)run_repl(session, environment, policy, failed);
        } else {
            (void)run_batch(session, environment, policy, failed);
        }

        const bool close_ok = close_once(session, opened, environment.error);
        return (failed || !close_ok) ? 1 : 0;
    } catch (const std::exception& exception) {
        report_exception_noexcept(environment.error, "application exception: ", exception);
        (void)close_once(session, opened, environment.error);
        return 1;
    } catch (...) {
        report_unknown_exception_noexcept(environment.error);
        (void)close_once(session, opened, environment.error);
        return 1;
    }
}

}  // namespace tinydbms::app
