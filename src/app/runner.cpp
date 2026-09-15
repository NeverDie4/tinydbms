#include "runner.hpp"

#include "arguments.hpp"
#include "input.hpp"
#include "output.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <csignal>
#include <exception>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace tinydbms::app {
namespace {

// 进程级"当前活动令牌"指针：信号处理器只读它，因此要求无锁原子。
std::atomic<const tinydbms::core::CancelToken*> g_active_token{nullptr};
static_assert(
    std::atomic<const tinydbms::core::CancelToken*>::is_always_lock_free,
    "SIGINT handler requires lock-free atomic pointers");

}  // namespace

// 信号处理器需要 C 语言链接，定义放在文件作用域：只做一次原子置位，不展示、不写流、
// 不分配内存。空闲期（没有活动令牌）或第二次中断恢复默认处置并重新触发，进程立即终止。
extern "C" void tinydbms_sigint_handler(int);
extern "C" void tinydbms_sigint_handler(int) {
    const tinydbms::core::CancelToken* token =
        g_active_token.load(std::memory_order_relaxed);
    if (token != nullptr && !token->cancel_requested()) {
        token->request_cancel();
        return;
    }
    std::signal(SIGINT, SIG_DFL);
    std::raise(SIGINT);
}

void install_sigint_handler() noexcept {
    std::signal(SIGINT, tinydbms_sigint_handler);
}

namespace {

// 活动令牌的 RAII 维护：设置在 execute_script 之前，清除在令牌析构之前，
// 避免处理器读到悬垂指针；恢复上一层值以便将来嵌套调用。
class ActiveCancelToken {
public:
    explicit ActiveCancelToken(const tinydbms::core::CancelToken& token) noexcept
        : previous_{g_active_token.exchange(&token, std::memory_order_relaxed)} {}

    ActiveCancelToken(const ActiveCancelToken&) = delete;
    ActiveCancelToken& operator=(const ActiveCancelToken&) = delete;

    ~ActiveCancelToken() {
        g_active_token.store(previous_, std::memory_order_relaxed);
    }

private:
    const tinydbms::core::CancelToken* previous_;
};

// CLI 运行期选项：由参数解析结果构造一次，批处理与 REPL 共用同一组语义。
struct ExecutionOptions {
    OutputFormat format = OutputFormat::kTable;
    tinydbms::core::ExecutionMode mode = tinydbms::core::ExecutionMode::kExecute;
    tinydbms::core::ScriptErrorPolicy policy =
        tinydbms::core::ScriptErrorPolicy::kStopOnFirstError;
    std::size_t max_query_rows = tinydbms::core::kMaxQueryRows;
    // --time：把每次 execute_script 的墙钟耗时写到 stderr；不影响 stdout 与退出码。
    bool show_time = false;
};

// 计时范围只包含 execute_script 调用本身：open/close、输入读取与渲染都不计入，
// 这样该数值与 core 的执行耗时口径一致，也方便跨入口比较。
void report_time_noexcept(
    const CliEnvironment& environment,
    std::string scope,
    std::chrono::steady_clock::duration elapsed) noexcept {
    try {
        (void)write_time_line(
            scope,
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed),
            environment.error);
    } catch (...) {
        // 计时输出失败不能改变本次调用的结果判定。
    }
}

void report_error_noexcept(
    std::ostream& output,
    OutputFormat format,
    tinydbms::core::Error error) noexcept {
    try {
        (void)write_error(error, format, output);
    } catch (...) {
        // A failed error stream cannot be repaired by the runner.
    }
}

void report_exception_noexcept(
    std::ostream& output,
    OutputFormat format,
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
        format,
        tinydbms::core::Error{
            tinydbms::core::ErrorKind::kInternal,
            std::nullopt,
            std::nullopt,
            std::move(message),
            std::nullopt,
            std::nullopt});
}

void report_unknown_exception_noexcept(
    std::ostream& output,
    OutputFormat format) noexcept {
    report_error_noexcept(
        output,
        format,
        tinydbms::core::Error{
            tinydbms::core::ErrorKind::kInternal,
            std::nullopt,
            std::nullopt,
            "application raised an unknown exception",
            std::nullopt,
            std::nullopt});
}

bool close_once(
    Session& session,
    bool& opened,
    OutputFormat format,
    std::ostream& error_output) noexcept {
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
            (void)write_error(*result.error, format, error_output);
            return false;
        } catch (...) {
            return false;
        }
    } catch (const std::exception& exception) {
        report_exception_noexcept(error_output, format, "close exception: ", exception);
        return false;
    } catch (...) {
        report_unknown_exception_noexcept(error_output, format);
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

void report_input_error(
    CliEnvironment& environment,
    OutputFormat format,
    std::string_view message) {
    report_error_noexcept(
        environment.error,
        format,
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
    OutputFormat format,
    CliEnvironment& environment) {
    RenderResult aggregate;
    bool script_error_written = false;

    for (const auto& statement : result.statements) {
        if (!script_error_written && result.script_error.has_value() &&
            statement.status() == tinydbms::core::StatementStatus::kSkippedExecution) {
            if (!write_error(*result.script_error, format, environment.error)) {
                aggregate.output_ok = false;
                return aggregate;
            }
            script_error_written = true;
        }

        const RenderResult current = render_statement_result(
            statement,
            result.script_error.has_value(),
            format,
            environment.output,
            environment.error);
        aggregate.output_ok = aggregate.output_ok && current.output_ok;
        aggregate.had_error = aggregate.had_error || current.had_error;
        if (!current.output_ok) {
            break;
        }
    }

    if (aggregate.output_ok && !script_error_written && result.script_error.has_value()) {
        if (!write_error(*result.script_error, format, environment.error)) {
            aggregate.output_ok = false;
        }
    }
    aggregate.had_error = aggregate.had_error || result.script_error.has_value();
    return aggregate;
}

bool run_batch(
    Session& session,
    CliEnvironment& environment,
    const ExecutionOptions& options,
    bool& failed) {
    std::string text;
    std::string input_error;
    if (!read_batch(environment.input, text, input_error)) {
        failed = true;
        report_input_error(environment, options.format, input_error);
        return false;
    }

    tinydbms::core::ExecuteScriptRequest request;
    request.text = std::move(text);
    request.error_policy = options.policy;
    request.mode = options.mode;
    request.max_query_rows = options.max_query_rows;
    // 每次调用使用独立令牌：批处理整段文本共享一个令牌，空闲期按下的 Ctrl+C 不会残留。
    request.cancel = tinydbms::core::CancelToken{};

    tinydbms::core::ExecuteScriptResult result;
    const auto started = std::chrono::steady_clock::now();
    {
        const ActiveCancelToken active{request.cancel};
        result = session.execute_script(request);
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;

    const RenderResult rendered = render_script_result(result, options.format, environment);
    failed = failed || rendered.had_error || !rendered.output_ok;
    if (options.show_time) {
        report_time_noexcept(environment, "script", elapsed);
    }
    return rendered.output_ok;
}

bool run_repl(
    Session& session,
    CliEnvironment& environment,
    const ExecutionOptions& options,
    bool& failed) {
    std::size_t line_number = 0;
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
            report_input_error(environment, options.format, input_error);
            return false;
        }
        if (reached_eof) {
            return true;
        }
        ++line_number;

        tinydbms::core::ExecuteScriptRequest request;
        request.text = std::move(line);
        request.error_policy = options.policy;
        request.mode = options.mode;
        request.max_query_rows = options.max_query_rows;
        // REPL 每一行使用新令牌：上一行被取消不会毒化后续输入。
        request.cancel = tinydbms::core::CancelToken{};

        tinydbms::core::ExecuteScriptResult result;
        const auto started = std::chrono::steady_clock::now();
        {
            const ActiveCancelToken active{request.cancel};
            result = session.execute_script(request);
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;

        const RenderResult rendered = render_script_result(result, options.format, environment);
        failed = failed || rendered.had_error || !rendered.output_ok;
        if (options.show_time) {
            report_time_noexcept(environment, "line " + std::to_string(line_number), elapsed);
        }
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
    // 参数错误保持纯文本（--format 是否生效尚未确定）；解析成功后按请求的格式输出诊断。
    OutputFormat format = OutputFormat::kTable;
    try {
        const ParseArgumentsResult parsed_result = parse_arguments(argc, argv);
        if (const auto* argument_error = std::get_if<ArgumentError>(&parsed_result);
            argument_error != nullptr) {
            return write_argument_error(*argument_error, environment) ? 2 : 1;
        }

        const ParsedArguments& arguments = std::get<ParsedArguments>(parsed_result);
        // 未显式指定 --format 时：stdout 是终端就给人看的 pretty，被重定向/接管道就保持
        // 脚本契约的 table（TSV）。显式值永远优先，终端判定不参与。
        format = arguments.format_explicit
            ? arguments.format
            : (environment.output_is_terminal
                   ? OutputFormat::kPretty
                   : OutputFormat::kTable);
        if (arguments.action == CliAction::kHelp) {
            return write_help(environment.output) ? 0 : 1;
        }
        if (arguments.action == CliAction::kVersion) {
            return write_version(environment.output, version) ? 0 : 1;
        }

        const tinydbms::core::OpenDatabaseResult opened_result =
            session.open(tinydbms::core::OpenDatabaseRequest{arguments.data_dir});
        if (opened_result.error.has_value()) {
            (void)write_error(*opened_result.error, format, environment.error);
            return 1;
        }
        opened = true;

        ExecutionOptions options;
        options.format = format;
        options.mode =
            arguments.plan_only
            ? tinydbms::core::ExecutionMode::kPlanOnly
            : tinydbms::core::ExecutionMode::kExecute;
        options.policy =
            arguments.error_policy == ErrorPolicy::kAnalyze
            ? tinydbms::core::ScriptErrorPolicy::kAnalyzeRemaining
            : tinydbms::core::ScriptErrorPolicy::kStopOnFirstError;
        options.max_query_rows =
            arguments.max_query_rows.value_or(tinydbms::core::kMaxQueryRows);
        options.show_time = arguments.show_time;

        bool failed = false;
        if (environment.interactive) {
            (void)run_repl(session, environment, options, failed);
        } else {
            (void)run_batch(session, environment, options, failed);
        }

        const bool close_ok = close_once(session, opened, format, environment.error);
        return (failed || !close_ok) ? 1 : 0;
    } catch (const std::exception& exception) {
        report_exception_noexcept(
            environment.error, format, "application exception: ", exception);
        (void)close_once(session, opened, format, environment.error);
        return 1;
    } catch (...) {
        report_unknown_exception_noexcept(environment.error, format);
        (void)close_once(session, opened, format, environment.error);
        return 1;
    }
}

}  // namespace tinydbms::app
