#ifndef TINYDBMS_CORE_INTERNAL_HPP
#define TINYDBMS_CORE_INTERNAL_HPP

#include "tinydbms/compiler.hpp"
#include "tinydbms/core.hpp"
#include "tinydbms/storage.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydbms::core {

// 语句执行的内部结果。取消与"执行完成（可能带错误）"是两种不同结局：取消只发生在
// 无副作用检查点，不携带 outcome，也不产生 script_error，因此不能用 Error 表达。
struct PlanExecutionResult {
    ExecuteResult outcome;
    bool cancelled = false;
    // 本次 Plan 是否已经调用过 Storage。执行上下文随调用栈回收，标记必须回传给
    // 调用方（script.cpp 用它把执行器的 kInternal 区分成
    // kExecutionIndeterminate 或 kSkippedExecution）。
    bool storage_called = false;
};

// 一次 Plan 执行的临时状态：只属于当前执行流，不放进共享的 Impl。
// U6 之前它们是 Impl 的成员，并发调用会互相覆盖。
struct ExecutionContext {
    // 本次语句的取消令牌；nullptr 表示本次调用不可取消。
    const CancelToken* cancel_token = nullptr;
    // 本次语句是否已经调用过 Storage（见 PlanExecutionResult::storage_called）。
    bool storage_called = false;
};

struct Database::Impl {
    // 会话状态的唯一互斥量，只由 Database 的公开方法持有；Impl 内部的 helper 不再加锁，
    // 也不得在持锁期间回调公开方法（不存在这样的路径）。
    std::mutex mutex;

    std::vector<TableMeta> catalog;
    std::uint64_t next_table_id = 0;
    bool open = false;
    bool forced_close_pending = false;
    bool cleanup_retry_needed = false;

    void clear() noexcept;
    void abort_after_storage_exception() noexcept;
    ExecuteResult execute_create_table(
        const compiler::CreateTablePlan& plan,
        ExecutionContext& context);
    ExecuteResult execute_insert(const compiler::InsertPlan& plan, ExecutionContext& context);
    ExecuteResult execute_delete(const compiler::DeletePlan& plan, ExecutionContext& context);
    ExecuteResult execute_update(const compiler::UpdatePlan& plan, ExecutionContext& context);
    ExecuteResult execute_query(
        const compiler::QueryPlan& plan,
        std::size_t max_query_rows,
        ExecutionContext& context);
    PlanExecutionResult execute_plan_impl(
        compiler::Plan plan,
        std::size_t max_query_rows,
        const CancelToken* cancel);
};

namespace internal {

// 取消信号的内部载体：只在执行检查点抛出，由 execute_plan_impl 统一捕获并转换为
// PlanExecutionResult::cancelled。它不派生自 std::exception，避免被"存储/编译器抛异常"
// 的兜底分支误当成致命中止；中间层也不得捕获它。
struct CancellationSignal {};

// 检查点：只在无副作用位置调用（扫描行循环、内存中的 Join/Sort/Aggregate 行循环）。
// 命中时抛出 CancellationSignal，由 CursorGuard 的析构负责关闭 cursor。
inline void throw_if_cancelled(const CancelToken* token) {
    if (token != nullptr && token->cancel_requested()) {
        throw CancellationSignal{};
    }
}

inline Error make_error(ErrorKind kind, std::string message) {
    return Error{
        kind,
        std::nullopt,
        std::nullopt,
        std::move(message),
        std::nullopt,
        std::nullopt};
}

inline Error make_error(ErrorKind kind, SourceRange source, std::string message) {
    return Error{
        kind,
        std::nullopt,
        std::optional<SourceRange>{source},
        std::move(message),
        std::nullopt,
        std::nullopt};
}

inline Error make_error(
    ErrorKind kind,
    std::string message,
    std::optional<std::string> suggestion) {
    return Error{
        kind,
        std::nullopt,
        std::nullopt,
        std::move(message),
        std::move(suggestion),
        std::nullopt};
}

inline Error make_compile_error(
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

inline ExecuteResult make_execute_error(ErrorKind kind, std::string message) {
    return ExecuteResult{make_error(kind, std::move(message))};
}

inline Error map_storage_error(storage::StorageError error) {
    return make_error(ErrorKind::kStorage, std::move(error.message));
}

inline bool has_duplicate_table(
    const std::vector<TableMeta>& tables,
    const TableMeta& candidate) {
    for (const TableMeta& table : tables) {
        if (table.table_id == candidate.table_id || table.table_name == candidate.table_name) {
            return true;
        }
    }
    return false;
}

inline bool is_supported_type(Type type) noexcept {
    return type == Type::kInt || type == Type::kBigInt || type == Type::kDouble ||
        type == Type::kBoolean || type == Type::kVarchar;
}

inline bool is_valid_identifier(std::string_view value) noexcept {
    if (value.empty() || value.size() > 64U) {
        return false;
    }
    if (value.front() < 'a' || value.front() > 'z') {
        return false;
    }
    for (std::size_t index = 1; index < value.size(); ++index) {
        const char character = value[index];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') ||
              character == '_')) {
            return false;
        }
    }
    return true;
}

inline std::optional<Error> validate_table_metadata(const TableMeta& table) {
    if (!is_valid_identifier(table.table_name)) {
        return make_error(
            ErrorKind::kInternal,
            "table metadata contains an invalid table name");
    }
    if (table.columns.empty()) {
        return make_error(ErrorKind::kInternal, "table schema must contain at least one column");
    }
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
        const ColumnMeta& column = table.columns[index];
        if (!is_valid_identifier(column.name)) {
            return make_error(
                ErrorKind::kInternal,
                "table schema contains an invalid column name");
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (table.columns[previous].name == column.name) {
                return make_error(
                    ErrorKind::kInternal,
                    "table schema contains duplicate column names");
            }
        }
        if (!is_supported_type(column.type)) {
            return make_error(ErrorKind::kInternal, "table schema contains an unsupported column type");
        }
    }
    return std::nullopt;
}

inline bool is_execution_failure(const ExecuteResult& result) {
    if (std::holds_alternative<Error>(result.outcome)) {
        return true;
    }
    const CommandResult* command = std::get_if<CommandResult>(&result.outcome);
    return command != nullptr && command->error.has_value();
}

}  // namespace internal

}  // namespace tinydbms::core

#endif  // TINYDBMS_CORE_INTERNAL_HPP
