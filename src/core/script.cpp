#include "internal.hpp"

#include "tinydbms/compiler.hpp"

#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydbms::core {
namespace {

std::optional<SourceLocation> make_absolute_location(
    SourceLocation statement_start,
    SourceLocation relative_location) {
    if (statement_start.line < 1 || statement_start.column < 1 ||
        relative_location.line < 1 || relative_location.column < 1) {
        return std::nullopt;
    }

    const std::int64_t line = static_cast<std::int64_t>(statement_start.line) +
        static_cast<std::int64_t>(relative_location.line) - 1;
    const std::int64_t column = relative_location.line == 1
        ? static_cast<std::int64_t>(statement_start.column) + relative_location.column - 1
        : relative_location.column;

    if (line > std::numeric_limits<int>::max() || column > std::numeric_limits<int>::max() ||
        relative_location.byte_offset >
            std::numeric_limits<std::size_t>::max() - statement_start.byte_offset) {
        return std::nullopt;
    }
    return SourceLocation{
        static_cast<int>(line),
        static_cast<int>(column),
        statement_start.byte_offset + relative_location.byte_offset};
}

std::optional<SourceRange> make_absolute_range(
    SourceLocation statement_start,
    SourceRange relative_range) {
    const std::optional<SourceLocation> begin = make_absolute_location(
        statement_start, relative_range.begin);
    const std::optional<SourceLocation> end = make_absolute_location(
        statement_start, relative_range.end);
    if (!begin.has_value() || !end.has_value() ||
        begin->byte_offset > end->byte_offset) {
        return std::nullopt;
    }
    return SourceRange{*begin, *end};
}

std::optional<FixIt> make_absolute_fix_it(
    SourceLocation statement_start,
    const std::optional<FixIt>& relative_fix_it) {
    if (!relative_fix_it.has_value()) {
        return std::nullopt;
    }
    const std::optional<SourceRange> range = make_absolute_range(
        statement_start, relative_fix_it->range);
    if (!range.has_value()) {
        return std::nullopt;
    }
    return FixIt{
        *range,
        relative_fix_it->replacement};
}

Error make_compile_error(
    SourceLocation statement_start,
    compiler::CompileError compile_error) {
    const std::optional<SourceRange> source = make_absolute_range(
        statement_start, compile_error.source);
    const std::optional<FixIt> fix_it = make_absolute_fix_it(
        statement_start, compile_error.fix_it);
    if (!source.has_value() ||
        (compile_error.fix_it.has_value() && !fix_it.has_value())) {
        return internal::make_error(
            ErrorKind::kInternal,
            "compiler returned an invalid error location");
    }
    return Error{
        ErrorKind::kCompile,
        source->begin,
        std::move(compile_error.message),
        std::move(compile_error.suggestion),
        fix_it};
}

struct ShadowCatalog {
    std::vector<TableMeta> tables;
    std::uint64_t next_table_id;
};

std::optional<Error> apply_analysis_create(
    ShadowCatalog& shadow,
    const compiler::Plan& plan) {
    const auto* create = std::get_if<compiler::CreateTablePlan>(&plan.kind);
    if (create == nullptr) {
        return std::nullopt;
    }
    if (shadow.next_table_id > std::numeric_limits<TableId>::max()) {
        return internal::make_error(ErrorKind::kCompile, "analysis catalog table id exhausted");
    }

    const TableId table_id = static_cast<TableId>(shadow.next_table_id);
    TableMeta metadata{table_id, create->table_name, create->columns};
    if (const std::optional<Error> error = internal::validate_table_metadata(metadata);
        error.has_value()) {
        return error;
    }
    if (internal::has_duplicate_table(shadow.tables, metadata)) {
        return internal::make_error(
            ErrorKind::kCompile,
            "analysis catalog contains a duplicate table");
    }
    if (shadow.tables.size() == shadow.tables.max_size()) {
        return internal::make_error(ErrorKind::kInternal, "analysis catalog cannot grow further");
    }

    shadow.tables.push_back(std::move(metadata));
    shadow.next_table_id = static_cast<std::uint64_t>(table_id) + 1U;
    return std::nullopt;
}

StatementResult make_statement_result(
    std::size_t index,
    const compiler::SplitStatement& statement,
    StatementStatus status,
    std::optional<ExecuteResult> outcome) {
    return StatementResult{
        index,
        statement.source,
        status,
        std::move(outcome)};
}

}  // namespace

ExecuteScriptResult Database::execute_script(const ExecuteScriptRequest& request) {
    if (impl_ == nullptr) {
        ExecuteScriptResult result;
        result.script_error = internal::make_error(
            ErrorKind::kExecute, "database object is moved-from");
        return result;
    }
    if (!impl_->open) {
        ExecuteScriptResult result;
        result.script_error = internal::make_error(
            ErrorKind::kExecute, "database is not open");
        return result;
    }

    ExecuteScriptResult result;
    bool execution_enabled = true;
    bool storage_execution_started = false;
    std::optional<ShadowCatalog> shadow;
    try {
        compiler::SplitStatementsResult split =
            compiler::split_statements(request.text);
        if (auto* split_error = std::get_if<compiler::CompileError>(&split.outcome)) {
            result.script_error = Error{
                ErrorKind::kCompile,
                split_error->source.begin,
                std::move(split_error->message),
                std::move(split_error->suggestion),
                std::move(split_error->fix_it)};
            return result;
        }

        const auto& statements =
            std::get<std::vector<compiler::SplitStatement>>(split.outcome);
        result.statements.reserve(statements.size());

        for (std::size_t index = 0; index < statements.size(); ++index) {
            const compiler::SplitStatement& statement = statements[index];
            const std::vector<TableMeta>& compile_catalog = shadow.has_value()
                ? shadow->tables
                : impl_->catalog;
            compiler::CompileResult compiled = compiler::compile(compiler::CompileRequest{
                statement.sql,
                compiler::CatalogView{
                    std::span<const TableMeta>{compile_catalog.data(), compile_catalog.size()}}});

            if (std::holds_alternative<compiler::CompileError>(compiled.outcome)) {
                compiler::CompileError compile_error =
                    std::move(std::get<compiler::CompileError>(compiled.outcome));
                result.statements.push_back(make_statement_result(
                    index,
                    statement,
                    StatementStatus::kCompileError,
                    ExecuteResult{make_compile_error(
                        statement.source.begin,
                        std::move(compile_error))}));
                if (execution_enabled) {
                    execution_enabled = false;
                    result.first_error_index = index;
                    shadow.emplace(ShadowCatalog{impl_->catalog, impl_->next_table_id});
                }
                continue;
            }

            compiler::Plan plan = std::move(std::get<compiler::Plan>(compiled.outcome));
            if (!execution_enabled) {
                const std::optional<Error> analysis_error = apply_analysis_create(*shadow, plan);
                if (analysis_error.has_value()) {
                    result.statements.push_back(make_statement_result(
                        index,
                        statement,
                        StatementStatus::kCompileError,
                        ExecuteResult{*analysis_error}));
                } else {
                    result.statements.push_back(make_statement_result(
                        index,
                        statement,
                        StatementStatus::kAnalysisOnly,
                        std::nullopt));
                }
                continue;
            }

            // 从这里开始 Plan 可能已打开 cursor 或改动 Storage，异常需要清理。
            storage_execution_started = true;
            ++result.executed_count;
            ExecuteResult execution = impl_->execute_plan_impl(std::move(plan));
            storage_execution_started = false;
            const bool failed = internal::is_execution_failure(execution);
            result.statements.push_back(make_statement_result(
                index,
                statement,
                failed ? StatementStatus::kExecutionError : StatementStatus::kExecuted,
                std::move(execution)));
            if (failed) {
                execution_enabled = false;
                result.first_error_index = index;
                shadow.emplace(ShadowCatalog{impl_->catalog, impl_->next_table_id});
            }
        }
    } catch (const std::exception& exception) {
        if (storage_execution_started && impl_->open) {
            impl_->abort_after_storage_exception();
        }
        result.script_error = internal::make_error(ErrorKind::kInternal, exception.what());
    } catch (...) {
        if (storage_execution_started && impl_->open) {
            impl_->abort_after_storage_exception();
        }
        result.script_error = internal::make_error(
            ErrorKind::kInternal, "unknown exception while executing script");
    }
    return result;
}

}  // namespace tinydbms::core
