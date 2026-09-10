#pragma once

#include "tinydbms/types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace tinydbms::core {

struct Error {
    SourceLocation location;
    std::string message;
};

struct OpenDatabaseRequest {
    std::string data_dir;
};

struct OpenDatabaseResult {
    std::optional<Error> error;
};

struct CloseDatabaseResult {
    std::optional<Error> error;
};

struct QueryResult {
    std::vector<ColumnMeta> columns;
    std::vector<std::vector<Value>> rows;
};

struct CommandResult {
    std::uint64_t affected_rows = 0;
    std::optional<Error> error;
};

struct ExecuteResult {
    std::variant<QueryResult, CommandResult, Error> value;
};

struct ExecuteScriptRequest {
    std::string text;
};

struct ExecuteScriptResult {
    std::vector<ExecuteResult> outcomes;
};

OpenDatabaseResult open_database(const OpenDatabaseRequest& request);
CloseDatabaseResult close_database();
ExecuteScriptResult execute_script(const ExecuteScriptRequest& request);

}  // namespace tinydbms::core
