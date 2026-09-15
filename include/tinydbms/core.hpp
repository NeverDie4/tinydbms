#ifndef TINYDBMS_CORE_HPP
#define TINYDBMS_CORE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "tinydbms/common.hpp"
#include "tinydbms/diagnostic.hpp"

namespace tinydbms::core {

using Row = std::vector<Value>;  // core 执行器内部与结果中的行

struct ColumnHeader {
    std::string name;
    Type type;
};

struct QueryResult {
    std::vector<ColumnHeader> columns;
    std::vector<Row> rows;
};

enum class ErrorKind {
    kCompile,
    kExecute,
    kStorage,
    kAnalysis,
    kInternal
};

struct Error {
    ErrorKind kind;
    std::optional<CompileStage> compile_stage;  // 仅 kCompile 携带
    std::optional<SourceRange> source;          // 脚本绝对范围；无法定位时为空
    std::string message;
    std::optional<std::string> suggestion;      // 仅 kCompile 可能携带
    std::optional<FixIt> fix_it;                // 仅 kCompile 可能携带
};

struct CommandResult {
    std::uint64_t affected_rows;  // CREATE 成功时为 0
    // 仅 INSERT/DELETE/UPDATE 部分行成功后 storage 出错时携带；
    // 此时 affected_rows 是错误前已成功执行的行数，core 停止后续语句
    std::optional<Error> error;
};

struct ExecuteResult {
    std::variant<QueryResult, CommandResult, Error> outcome;
};

// 脚本策略：默认在第一条错误后停止，analyze 仅在显式请求时继续静态分析。
enum class ScriptErrorPolicy {
    kStopOnFirstError,
    kAnalyzeRemaining
};

// 执行模式：kExecute 编译并执行；kPlanOnly 只编译并把执行计划整理成文本返回，
// 不调用 Storage、不修改 Catalog，因此不产生任何副作用。
enum class ExecutionMode {
    kExecute,
    kPlanOnly
};

enum class StatementStatus {
    kExecuted,
    kPlanOnly,
    kCompileError,
    kExecutionError,
    kExecutionIndeterminate,
    kAnalysisError,
    kAnalysisOnly,
    kSkippedExecution,
    // 运行中取消：当前语句在无副作用检查点结束，后续语句未开始。不携带 outcome，
    // 不产生 script_error，也不代表"状态未知"。
    kCancelled
};

inline constexpr std::size_t kMaxStatementsPerScript = 4096;

// 单条语句在内存中物化的最大行数。SQL v2 的 JOIN/Sort/Aggregate 由 core 全量物化，
// 该上限保证超越内存的结果规模以执行错误结束，而不是让进程无界增长或中止会话。
// 计数按物化节点（扫描、连接）的行数计算，不包含 Storage 内部页与结果编码。
inline constexpr std::size_t kMaxQueryRows = std::size_t{1} << 18;  // 262144

// 运行中取消的请求级令牌：只做"置位 + 轮询"，不注册回调、不分配内存，
// 因此 request_cancel() 可以在信号处理器里调用。拷贝共享同一标志。
class CancelToken {
public:
    CancelToken() : flag_{std::make_shared<std::atomic<bool>>(false)} {}

    void request_cancel() const noexcept {
        flag_->store(true, std::memory_order_relaxed);
    }

    [[nodiscard]] bool cancel_requested() const noexcept {
        return flag_->load(std::memory_order_relaxed);
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

// 信号处理器可用性依赖无锁实现：标准没有显式担保，因此把前提固定在编译期，
// 而不是留下运行时不确定性。
static_assert(std::atomic<bool>::is_always_lock_free, "CancelToken requires lock-free atomics");

namespace detail {

[[noreturn]] inline void invalid_statement_result(const char* message) {
    throw std::invalid_argument{message};
}

inline const ExecuteResult* outcome_pointer(
    const std::optional<ExecuteResult>& outcome) noexcept {
    return outcome.has_value() ? &*outcome : nullptr;
}

inline bool is_error_kind(const ExecuteResult& outcome, ErrorKind kind) noexcept {
    const Error* error = std::get_if<Error>(&outcome.outcome);
    return error != nullptr && error->kind == kind;
}

inline bool is_execution_error_kind(ErrorKind kind) noexcept {
    return kind == ErrorKind::kExecute || kind == ErrorKind::kStorage;
}

inline bool carries_execution_error(const ExecuteResult& outcome) noexcept {
    if (const Error* error = std::get_if<Error>(&outcome.outcome); error != nullptr) {
        return is_execution_error_kind(error->kind);
    }
    const CommandResult* command = std::get_if<CommandResult>(&outcome.outcome);
    return command != nullptr && command->error.has_value() &&
        is_execution_error_kind(command->error->kind);
}

// 工厂的唯一职责是拒绝“状态与 outcome 组合”错误；这里抛出的异常属于实现缺陷，
// 不应作为正常业务错误被调用方捕获。
inline void validate_statement_result(
    StatementStatus status,
    const std::optional<ExecuteResult>& outcome) {
    const ExecuteResult* value = outcome_pointer(outcome);
    switch (status) {
    case StatementStatus::kExecuted:
        if (value == nullptr) {
            invalid_statement_result("kExecuted requires an outcome");
        }
        if (std::holds_alternative<QueryResult>(value->outcome)) {
            return;
        }
        if (const CommandResult* command = std::get_if<CommandResult>(&value->outcome);
            command != nullptr && !command->error.has_value()) {
            return;
        }
        invalid_statement_result("kExecuted must carry a result without an error");
    case StatementStatus::kCompileError:
        if (value == nullptr || !is_error_kind(*value, ErrorKind::kCompile)) {
            invalid_statement_result("kCompileError requires a kCompile error");
        }
        return;
    case StatementStatus::kPlanOnly:
        if (value == nullptr || !std::holds_alternative<QueryResult>(value->outcome)) {
            invalid_statement_result("kPlanOnly requires a query result");
        }
        return;
    case StatementStatus::kExecutionError:
        if (value == nullptr || !carries_execution_error(*value)) {
            invalid_statement_result("kExecutionError requires a kExecute or kStorage error");
        }
        return;
    case StatementStatus::kAnalysisError:
        if (value == nullptr || !is_error_kind(*value, ErrorKind::kAnalysis)) {
            invalid_statement_result("kAnalysisError requires a kAnalysis error");
        }
        return;
    case StatementStatus::kExecutionIndeterminate:
    case StatementStatus::kAnalysisOnly:
    case StatementStatus::kSkippedExecution:
    case StatementStatus::kCancelled:
        if (value != nullptr) {
            invalid_statement_result("status must not carry an outcome");
        }
        return;
    }
    invalid_statement_result("unknown statement status");
}

}  // namespace detail

// 逐语句结果只能经命名工厂构造，字段私有且只读。
class StatementResult {
public:
    static StatementResult executed(
        std::size_t statement_index,
        SourceRange source,
        ExecuteResult outcome) {
        std::optional<ExecuteResult> wrapped{std::move(outcome)};
        detail::validate_statement_result(StatementStatus::kExecuted, wrapped);
        return StatementResult{
            statement_index, source, StatementStatus::kExecuted, std::move(wrapped)};
    }

    static StatementResult compile_error(
        std::size_t statement_index,
        SourceRange source,
        Error error) {
        std::optional<ExecuteResult> wrapped{ExecuteResult{std::move(error)}};
        detail::validate_statement_result(StatementStatus::kCompileError, wrapped);
        return StatementResult{
            statement_index, source, StatementStatus::kCompileError, std::move(wrapped)};
    }

    // 计划模式：语句已编译并渲染成单列 VARCHAR 的计划文本，但没有进入执行器。
    static StatementResult plan_only(
        std::size_t statement_index,
        SourceRange source,
        QueryResult plan) {
        std::optional<ExecuteResult> wrapped{ExecuteResult{std::move(plan)}};
        detail::validate_statement_result(StatementStatus::kPlanOnly, wrapped);
        return StatementResult{
            statement_index, source, StatementStatus::kPlanOnly, std::move(wrapped)};
    }

    static StatementResult execution_error(
        std::size_t statement_index,
        SourceRange source,
        ExecuteResult outcome) {
        std::optional<ExecuteResult> wrapped{std::move(outcome)};
        detail::validate_statement_result(StatementStatus::kExecutionError, wrapped);
        return StatementResult{
            statement_index, source, StatementStatus::kExecutionError, std::move(wrapped)};
    }

    static StatementResult execution_indeterminate(
        std::size_t statement_index,
        SourceRange source) {
        return StatementResult{
            statement_index, source, StatementStatus::kExecutionIndeterminate, std::nullopt};
    }

    static StatementResult analysis_error(
        std::size_t statement_index,
        SourceRange source,
        Error error) {
        std::optional<ExecuteResult> wrapped{ExecuteResult{std::move(error)}};
        detail::validate_statement_result(StatementStatus::kAnalysisError, wrapped);
        return StatementResult{
            statement_index, source, StatementStatus::kAnalysisError, std::move(wrapped)};
    }

    static StatementResult analysis_only(
        std::size_t statement_index,
        SourceRange source) {
        return StatementResult{
            statement_index, source, StatementStatus::kAnalysisOnly, std::nullopt};
    }

    static StatementResult skipped(
        std::size_t statement_index,
        SourceRange source) {
        return StatementResult{
            statement_index, source, StatementStatus::kSkippedExecution, std::nullopt};
    }

    // 取消只发生在无副作用检查点：既不携带结果，也不表示物理状态未知。
    static StatementResult cancelled(
        std::size_t statement_index,
        SourceRange source) {
        return StatementResult{
            statement_index, source, StatementStatus::kCancelled, std::nullopt};
    }

    std::size_t statement_index() const noexcept {
        return statement_index_;
    }

    const SourceRange& source() const noexcept {
        return source_;
    }

    StatementStatus status() const noexcept {
        return status_;
    }

    const std::optional<ExecuteResult>& outcome() const noexcept {
        return outcome_;
    }

private:
    StatementResult(
        std::size_t statement_index,
        SourceRange source,
        StatementStatus status,
        std::optional<ExecuteResult> outcome)
        : statement_index_{statement_index},
          source_{source},
          status_{status},
          outcome_{std::move(outcome)} {}

    std::size_t statement_index_;
    SourceRange source_;
    StatementStatus status_;
    std::optional<ExecuteResult> outcome_;
};

struct OpenDatabaseRequest {
    std::string data_dir;  // 已由入口补全的非空数据库目录，UTF-8 编码
};

struct OpenDatabaseResult {
    std::optional<Error> error;
};

struct CloseDatabaseResult {
    std::optional<Error> error;
};

struct ExecuteScriptRequest {
    std::string text;  // REPL 一行，或 stdin 批处理的整段文本
    ScriptErrorPolicy error_policy{ScriptErrorPolicy::kStopOnFirstError};
    ExecutionMode mode{ExecutionMode::kExecute};
    // 单条语句在内存中物化的最大行数；必须 >= 1，默认值保持现行行为。
    // 只约束按行物化的节点（SeqScan、Join），不约束只收集 RecordId 的 DELETE/UPDATE。
    std::size_t max_query_rows{kMaxQueryRows};
    // 运行中取消令牌：默认从未请求取消，因此不传令牌时行为与现状完全一致。
    // 令牌不跨调用存活，调用方每次 execute_script 使用独立令牌。
    CancelToken cancel{};
};

struct ExecuteScriptResult {
    // 分句失败、语句数超限或致命中止时非空；普通语句错误进入 statements。
    std::optional<Error> script_error;
    // 分句成功后覆盖全部已识别语句，按 statement_index 升序且连续。
    std::vector<StatementResult> statements;
};

// core 对入口与测试暴露的有状态对象；每个实例持有自己的 Catalog 与生命周期状态。
//
// 线程语义：同一个实例的 open/close/execute_script 可以被多个执行流并发调用，
// core 内部按"调用级互斥"串行执行——一次调用从进入到返回全程持锁，后到者阻塞等待。
// 这不承诺并行度：同一时刻只有一个执行流在执行，不同实例之间仍受"同一进程同一时间
// 只有一个数据库打开"的进程级限制。对象移动与析构属于调用方责任，必须与并发调用互斥；
// 存储层（storage 公共 API）依然要求串行调用，这条前提由本类的串行化保证。
class Database {
public:
    Database();
    ~Database() noexcept;

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) noexcept;
    Database& operator=(Database&&) noexcept;

    OpenDatabaseResult open(const OpenDatabaseRequest& request);
    CloseDatabaseResult close();
    ExecuteScriptResult execute_script(const ExecuteScriptRequest& request);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tinydbms::core

#endif  // TINYDBMS_CORE_HPP
