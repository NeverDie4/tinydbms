#ifndef TINYDBMS_TESTING_RESULT_HELPERS_HPP
#define TINYDBMS_TESTING_RESULT_HELPERS_HPP

#include <cstddef>
#include <optional>

#include "tinydbms/core.hpp"

namespace tinydbms::testing {

// 合并前的 ExecuteScriptResult 曾提供 executed_count/first_error_index 两个派生字段；
// 合并后统一由 statements 派生，避免结果对象内出现两份可能不一致的状态。
// executed_count 保留原字段“进入过执行”的口径：语句只要到达执行阶段就计数，
// 因此执行失败（kExecutionError）与物理状态未知（kExecutionIndeterminate）也计入。
inline std::size_t executed_count(const core::ExecuteScriptResult& result) {
    std::size_t count = 0;
    for (const core::StatementResult& statement : result.statements) {
        switch (statement.status()) {
        case core::StatementStatus::kExecuted:
        case core::StatementStatus::kExecutionError:
        case core::StatementStatus::kExecutionIndeterminate:
            ++count;
            break;
        // 计划模式没有进入执行器，不计入“进入过执行”的口径。
        case core::StatementStatus::kPlanOnly:
        case core::StatementStatus::kCompileError:
        case core::StatementStatus::kAnalysisError:
        case core::StatementStatus::kAnalysisOnly:
        case core::StatementStatus::kSkippedExecution:
            break;
        }
    }
    return count;
}

inline std::optional<std::size_t> first_error_index(
    const core::ExecuteScriptResult& result) {
    for (const core::StatementResult& statement : result.statements) {
        switch (statement.status()) {
        case core::StatementStatus::kExecuted:
        case core::StatementStatus::kPlanOnly:
        case core::StatementStatus::kAnalysisOnly:
        case core::StatementStatus::kSkippedExecution:
            break;
        case core::StatementStatus::kCompileError:
        case core::StatementStatus::kExecutionError:
        case core::StatementStatus::kExecutionIndeterminate:
        case core::StatementStatus::kAnalysisError:
            return statement.statement_index();
        }
    }
    return std::nullopt;
}

}  // namespace tinydbms::testing

#endif  // TINYDBMS_TESTING_RESULT_HELPERS_HPP
