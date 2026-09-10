#include "tinydbms/compiler.h"
#include "tinydbms/core.h"
#include "tinydbms/storage.h"

#include <algorithm>

namespace tinydbms::core {
namespace {

struct CoreState {
    bool open = false;
    std::vector<TableMeta> catalog;
};

CoreState state;

Error make_error(std::string message, SourceLocation location = {}) {
    return Error{location, std::move(message)};
}

SourceLocation absolute_location(SourceLocation statement_start, SourceLocation relative) {
    if (relative.line == 1) {
        return SourceLocation{statement_start.line, statement_start.column + relative.column - 1};
    }
    return SourceLocation{statement_start.line + relative.line - 1, relative.column};
}

}  // namespace

OpenDatabaseResult open_database(const OpenDatabaseRequest& request) {
    if (state.open) {
        return {make_error("database is already open")};
    }
    const auto opened = storage::open_storage(storage::OpenStorageRequest{request.data_dir});
    if (opened.error.has_value()) {
        return {make_error(opened.error->message)};
    }
    const auto tables = storage::list_tables();
    if (tables.error.has_value()) {
        (void)storage::close_storage();
        return {make_error(tables.error->message)};
    }
    state.catalog = tables.tables;
    state.open = true;
    return {};
}

CloseDatabaseResult close_database() {
    if (!state.open) {
        return {};
    }
    const auto closed = storage::close_storage();
    if (closed.error.has_value()) {
        return {make_error(closed.error->message)};
    }
    state = CoreState{};
    return {};
}

ExecuteScriptResult execute_script(const ExecuteScriptRequest& request) {
    ExecuteScriptResult result;
    if (!state.open) {
        result.outcomes.push_back(ExecuteResult{make_error("database is not open")});
        return result;
    }

    for (const compiler::SplitStatement& statement : compiler::split_statements(request.text)) {
        const compiler::CompileRequest compile_request{
            statement.sql,
            compiler::CatalogView{std::span<const TableMeta>(state.catalog)},
        };
        const compiler::CompileResult compiled = compiler::compile(compile_request);
        if (const auto* compile_error = std::get_if<compiler::CompileError>(&compiled.value)) {
            result.outcomes.push_back(ExecuteResult{make_error(
                compile_error->message,
                absolute_location(statement.start, compile_error->location))});
            break;
        }
        result.outcomes.push_back(ExecuteResult{make_error(
            "plan execution is not initialized", statement.start)});
        break;
    }
    return result;
}

}  // namespace tinydbms::core
