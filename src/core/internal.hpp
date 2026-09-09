#ifndef TINYDBMS_CORE_INTERNAL_HPP
#define TINYDBMS_CORE_INTERNAL_HPP

#include "tinydbms/compiler.hpp"
#include "tinydbms/core.hpp"
#include "tinydbms/storage.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tinydbms::core {

struct Database::Impl {
    std::vector<TableMeta> catalog;
    std::uint64_t next_table_id = 0;
    bool open = false;
    bool forced_close_pending = false;
    bool cleanup_retry_needed = false;

    void clear() noexcept;
    void abort_after_storage_exception() noexcept;
    ExecuteResult execute_create_table(const compiler::CreateTablePlan& plan);
    ExecuteResult execute_insert(const compiler::InsertPlan& plan);
    ExecuteResult execute_delete(const compiler::DeletePlan& plan);
    ExecuteResult execute_query(const compiler::QueryPlan& plan);
    ExecuteResult execute_plan_impl(compiler::Plan plan);
};

namespace internal {

inline Error make_error(ErrorKind kind, std::string message) {
    return Error{kind, std::nullopt, std::move(message)};
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
    return type == Type::kInt || type == Type::kVarchar;
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
