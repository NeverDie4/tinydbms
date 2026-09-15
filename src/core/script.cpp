#include "internal.hpp"

#include "analysis_catalog.hpp"
#include "diagnostics.hpp"
#include "plan_text.hpp"

#include "tinydbms/compiler.hpp"

#include <cstddef>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace tinydbms::core {
namespace {

ExecuteScriptResult make_script_error_result(Error error) {
    ExecuteScriptResult result;
    result.script_error = std::move(error);
    return result;
}

std::string exception_text(const std::exception& exception) {
    const char* what = exception.what();
    return what == nullptr ? std::string{"unknown exception"} : std::string{what};
}

}  // namespace

ExecuteScriptResult Database::execute_script(const ExecuteScriptRequest& request) {
    if (impl_ == nullptr) {
        return make_script_error_result(internal::make_error(
            ErrorKind::kExecute,
            "database object is moved-from"));
    }
    if (!impl_->open) {
        return make_script_error_result(internal::make_error(
            ErrorKind::kExecute,
            "database is not open"));
    }
    if (request.max_query_rows == 0U) {
        // 调用方违约：不静默回退到默认值，直接拒绝整段脚本。
        return make_script_error_result(internal::make_error(
            ErrorKind::kExecute,
            "max_query_rows must be at least 1"));
    }

    const bool analyze = request.error_policy == ScriptErrorPolicy::kAnalyzeRemaining;
    const bool plan_only = request.mode == ExecutionMode::kPlanOnly;
    ExecuteScriptResult result;
    std::vector<compiler::SplitStatement> statements;
    // 当前正在处理的语句下标；等于 statements.size() 表示尚未进入语句循环。
    std::size_t active_index = 0;

    auto side_effects_possible = [this]() noexcept {
        return impl_->current_plan_storage_called ||
            impl_->cleanup_retry_needed ||
            impl_->forced_close_pending;
    };
    auto fill_skipped = [&](std::size_t from) {
        for (std::size_t index = from; index < statements.size(); ++index) {
            result.statements.push_back(
                StatementResult::skipped(index, statements[index].source));
        }
    };
    // 取消优先于错误策略：剩余语句既不编译也不执行，直接记为 kCancelled。
    auto fill_cancelled = [&](std::size_t from) {
        for (std::size_t index = from; index < statements.size(); ++index) {
            result.statements.push_back(
                StatementResult::cancelled(index, statements[index].source));
        }
    };
    // 致命中止发生在已识别语句上时，script_error 必须指向该语句；只有完全
    // 无法定位（分句阶段、契约违反）才保留空插入点。
    auto attach_source = [&](Error& error, std::size_t index) {
        if (!error.source.has_value() && index < statements.size()) {
            error.source = statements[index].source;
        }
    };
    // 致命中止：当前语句可能已产生副作用时记 indeterminate，否则记 skipped。
    auto abort_script = [&](std::size_t current, Error error) {
        attach_source(error, current);
        if (current < statements.size()) {
            if (side_effects_possible()) {
                result.statements.push_back(StatementResult::execution_indeterminate(
                    current, statements[current].source));
                fill_skipped(current + 1U);
            } else {
                fill_skipped(current);
            }
        }
        result.script_error = std::move(error);
        return result;
    };
    auto abort_after_exception = [&](const std::string& message) {
        Error error = internal::make_error(ErrorKind::kInternal, message);
        const std::size_t next = result.statements.size();
        // 只有正在处理某条语句时才有可信位置；分句或影子建立阶段失败使用空插入点。
        error.source = active_index < statements.size()
            ? std::optional<SourceRange>{statements[active_index].source}
            : std::optional<SourceRange>{internal::empty_source_range()};
        if (next < statements.size()) {
            if (side_effects_possible()) {
                result.statements.push_back(StatementResult::execution_indeterminate(
                    next, statements[next].source));
                fill_skipped(next + 1U);
            } else {
                fill_skipped(next);
            }
        }
        result.script_error = std::move(error);
        return result;
    };

    try {
        compiler::SplitStatementsResult split = compiler::split_statements(request.text);
        if (const auto* split_error = std::get_if<compiler::CompileError>(&split.outcome);
            split_error != nullptr) {
            if (!internal::is_valid_source_range(split_error->source, request.text)) {
                return make_script_error_result(internal::make_error(
                    ErrorKind::kInternal,
                    internal::empty_source_range(),
                    "compiler returned an invalid split error range"));
            }
            if (split_error->fix_it.has_value() &&
                !internal::is_valid_source_range(split_error->fix_it->range, request.text)) {
                return make_script_error_result(internal::make_error(
                    ErrorKind::kInternal,
                    internal::empty_source_range(),
                    "compiler returned an invalid split fix-it range"));
            }
            result.script_error = Error{
                ErrorKind::kCompile,
                split_error->stage,
                std::optional<SourceRange>{split_error->source},
                split_error->message,
                split_error->suggestion,
                split_error->fix_it};
            return result;
        }

        statements = std::get<std::vector<compiler::SplitStatement>>(std::move(split.outcome));
        active_index = statements.size();

        for (const compiler::SplitStatement& statement : statements) {
            const bool range_valid =
                internal::is_valid_source_range(statement.source, request.text);
            const bool text_matches_range =
                statement.source.end.byte_offset >= statement.source.begin.byte_offset &&
                statement.source.end.byte_offset - statement.source.begin.byte_offset ==
                    statement.sql.size();
            if (!range_valid || !text_matches_range) {
                return make_script_error_result(internal::make_error(
                    ErrorKind::kInternal,
                    internal::empty_source_range(),
                    "compiler returned an invalid split statement range"));
            }
        }

        if (statements.size() > kMaxStatementsPerScript) {
            return make_script_error_result(internal::make_compile_error(
                CompileStage::kLex,
                internal::empty_source_range(),
                "script contains too many statements"));
        }

        result.statements.reserve(statements.size());

        bool execution_allowed = true;
        std::optional<internal::AnalysisCatalog> analysis;
        if (analyze) {
            analysis = internal::make_analysis_catalog(impl_->catalog, impl_->next_table_id);
        }

        for (std::size_t index = 0; index < statements.size(); ++index) {
            const compiler::SplitStatement& statement = statements[index];
            active_index = index;
            impl_->current_plan_storage_called = false;

            // 语句边界检查点：取消后不再编译剩余语句，也不产生 skipped/analysis 状态。
            if (request.cancel.cancel_requested()) {
                fill_cancelled(index);
                return result;
            }

            // execution_allowed 关闭只可能出现在 analyze 策略（stop 策略首错后
            // 立即返回），因此此时影子 Catalog 必然已建立。
            const std::span<const TableMeta> catalog_view = execution_allowed
                ? std::span<const TableMeta>{impl_->catalog.data(), impl_->catalog.size()}
                : std::span<const TableMeta>{analysis->tables.data(), analysis->tables.size()};

            std::optional<compiler::CompileResult> compiled;
            try {
                compiled = compiler::compile(compiler::CompileRequest{
                    statement.sql,
                    compiler::CatalogView{catalog_view}});
            } catch (const std::exception& exception) {
                return abort_script(
                    index,
                    internal::make_error(
                        ErrorKind::kInternal,
                        "compiler raised an exception: " + exception_text(exception)));
            } catch (...) {
                return abort_script(
                    index,
                    internal::make_error(
                        ErrorKind::kInternal,
                        "compiler raised an unknown exception"));
            }

            if (const auto* compile_error =
                    std::get_if<compiler::CompileError>(&compiled->outcome);
                compile_error != nullptr) {
                const std::optional<SourceRange> absolute =
                    internal::absolutize_range(statement, compile_error->source, request.text);
                if (!absolute.has_value()) {
                    return abort_script(
                        index,
                        internal::make_error(
                            ErrorKind::kInternal,
                            "compiler returned an invalid compile error range"));
                }
                // fix-it 与主诊断使用同一套换算；非法范围按致命错误处理，避免把
                // 语句相对偏移当作脚本偏移展示。
                std::optional<FixIt> absolute_fix_it;
                if (compile_error->fix_it.has_value()) {
                    const std::optional<SourceRange> fix_range = internal::absolutize_range(
                        statement, compile_error->fix_it->range, request.text);
                    if (!fix_range.has_value()) {
                        return abort_script(
                            index,
                            internal::make_error(
                                ErrorKind::kInternal,
                                "compiler returned an invalid fix-it range"));
                    }
                    absolute_fix_it = FixIt{*fix_range, compile_error->fix_it->replacement};
                }
                result.statements.push_back(StatementResult::compile_error(
                    index,
                    statement.source,
                    Error{
                        ErrorKind::kCompile,
                        compile_error->stage,
                        std::optional<SourceRange>{*absolute},
                        compile_error->message,
                        compile_error->suggestion,
                        std::move(absolute_fix_it)}));
                if (!execution_allowed) {
                    continue;
                }
                execution_allowed = false;
                if (!analyze) {
                    fill_skipped(index + 1U);
                    return result;
                }
                continue;
            }

            compiler::Plan plan = std::move(std::get<compiler::Plan>(compiled->outcome));

            if (plan_only) {
                // 计划模式：只渲染编译产物，不进入执行器，也不更新影子 Catalog。
                // 因此不可能产生任何副作用；代价是同一脚本里 CREATE TABLE 之后的语句
                // 仍然编译不到新表，会按编译错误处理（已写进契约，不做特殊处理）。
                std::variant<QueryResult, Error> rendered =
                    internal::render_plan_result(plan, catalog_view);
                if (const Error* render_error = std::get_if<Error>(&rendered);
                    render_error != nullptr) {
                    return abort_script(index, *render_error);
                }
                result.statements.push_back(StatementResult::plan_only(
                    index,
                    statement.source,
                    std::get<QueryResult>(std::move(rendered))));
                continue;
            }

            if (!execution_allowed) {
                internal::ShadowApplyOutcome shadow;
                try {
                    shadow = internal::apply_supported_ddl(*analysis, plan);
                } catch (const std::exception& exception) {
                    return abort_script(
                        index,
                        internal::make_error(
                            ErrorKind::kInternal,
                            "shadow catalog update failed: " + exception_text(exception)));
                } catch (...) {
                    return abort_script(
                        index,
                        internal::make_error(
                            ErrorKind::kInternal,
                            "shadow catalog update raised an unknown exception"));
                }

                if (shadow.result == internal::ShadowApplyResult::kRejected) {
                    result.statements.push_back(StatementResult::analysis_error(
                        index,
                        statement.source,
                        internal::make_error(
                            ErrorKind::kAnalysis,
                            statement.source,
                            shadow.rejection_message)));
                } else {
                    result.statements.push_back(
                        StatementResult::analysis_only(index, statement.source));
                }
                continue;
            }

            const bool creates_table =
                std::holds_alternative<compiler::CreateTablePlan>(plan.kind);
            PlanExecutionResult plan_execution = impl_->execute_plan_impl(
                std::move(plan), request.max_query_rows, &request.cancel);

            if (plan_execution.cancelled) {
                // 取消只发生在无副作用检查点：不产生 script_error，也不使会话失效。
                result.statements.push_back(
                    StatementResult::cancelled(index, statement.source));
                fill_cancelled(index + 1U);
                return result;
            }

            ExecuteResult execution = std::move(plan_execution.outcome);

            if (const Error* internal_error = std::get_if<Error>(&execution.outcome);
                internal_error != nullptr && internal_error->kind == ErrorKind::kInternal) {
                return abort_script(
                    index,
                    internal::make_error(ErrorKind::kInternal, internal_error->message));
            }

            if (internal::is_execution_failure(execution)) {
                result.statements.push_back(StatementResult::execution_error(
                    index, statement.source, std::move(execution)));
                execution_allowed = false;
                if (!analyze) {
                    fill_skipped(index + 1U);
                    return result;
                }
                continue;
            }

            result.statements.push_back(
                StatementResult::executed(index, statement.source, std::move(execution)));

            if (analyze && creates_table) {
                try {
                    internal::mirror_runtime_catalog(
                        *analysis, impl_->catalog, impl_->next_table_id);
                } catch (const std::exception& exception) {
                    fill_skipped(index + 1U);
                    Error error = internal::make_error(
                        ErrorKind::kInternal,
                        "shadow catalog synchronization failed: " +
                            exception_text(exception));
                    error.source = statements[index].source;
                    result.script_error = std::move(error);
                    return result;
                } catch (...) {
                    fill_skipped(index + 1U);
                    Error error = internal::make_error(
                        ErrorKind::kInternal,
                        "shadow catalog synchronization raised an unknown exception");
                    error.source = statements[index].source;
                    result.script_error = std::move(error);
                    return result;
                }
            }
        }
    } catch (const std::exception& exception) {
        return abort_after_exception(
            "script execution was aborted: " + exception_text(exception));
    } catch (...) {
        return abort_after_exception(
            "script execution was aborted by an unknown exception");
    }

    return result;
}

}  // namespace tinydbms::core
