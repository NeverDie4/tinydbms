#ifndef TINYDBMS_CORE_INTERNAL_HPP
#define TINYDBMS_CORE_INTERNAL_HPP

#include "tinydbms/compiler.hpp"
#include "tinydbms/core.hpp"
#include "tinydbms/storage.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tinydbms::core {

struct Database::Impl {
    std::vector<TableMeta> catalog;
    std::uint64_t next_table_id = 0;
    bool open = false;

    void clear() noexcept;
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
