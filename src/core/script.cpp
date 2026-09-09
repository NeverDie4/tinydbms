#include "internal.hpp"

#include "tinydbms/compiler.hpp"

#include <cstdint>
#include <exception>
#include <limits>
#include <span>
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

    if (line > std::numeric_limits<int>::max() || column > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return SourceLocation{static_cast<int>(line), static_cast<int>(column)};
}

}  // namespace

ExecuteScriptResult Database::execute_script(const ExecuteScriptRequest& request) {
    if (impl_ == nullptr) {
        return ExecuteScriptResult{{internal::make_execute_error(
            ErrorKind::kExecute,
            "database object is moved-from")}};
    }
    if (!impl_->open) {
        return ExecuteScriptResult{{internal::make_execute_error(
            ErrorKind::kExecute,
            "database is not open")}};
    }

    ExecuteScriptResult result;
    try {
        const std::vector<compiler::SplitStatement> statements =
            compiler::split_statements(request.text);
        result.outcomes.reserve(statements.size());

        for (const compiler::SplitStatement& statement : statements) {
            compiler::CompileResult compiled = compiler::compile(compiler::CompileRequest{
                statement.sql,
                compiler::CatalogView{
                    std::span<const TableMeta>{impl_->catalog.data(), impl_->catalog.size()}}});

            if (std::holds_alternative<compiler::CompileError>(compiled.outcome)) {
                compiler::CompileError compile_error =
                    std::move(std::get<compiler::CompileError>(compiled.outcome));
                const std::optional<SourceLocation> location = make_absolute_location(
                    statement.start,
                    compile_error.location);
                if (!location.has_value()) {
                    result.outcomes.push_back(internal::make_execute_error(
                        ErrorKind::kInternal,
                        "compiler returned an invalid error location"));
                } else {
                    result.outcomes.push_back(ExecuteResult{Error{
                        ErrorKind::kCompile,
                        location,
                        std::move(compile_error.message)}});
                }
                break;
            }

            compiler::Plan plan = std::move(std::get<compiler::Plan>(compiled.outcome));
            ExecuteResult execution = impl_->execute_plan_impl(std::move(plan));
            const bool stop = internal::is_execution_failure(execution);
            result.outcomes.push_back(std::move(execution));
            if (stop) {
                break;
            }
        }
    } catch (const std::exception& exception) {
        if (impl_->open) {
            impl_->abort_after_storage_exception();
        }
        result.outcomes.push_back(internal::make_execute_error(ErrorKind::kInternal, exception.what()));
    } catch (...) {
        if (impl_->open) {
            impl_->abort_after_storage_exception();
        }
        result.outcomes.push_back(internal::make_execute_error(
            ErrorKind::kInternal,
            "unknown exception while executing script"));
    }
    return result;
}

}  // namespace tinydbms::core
