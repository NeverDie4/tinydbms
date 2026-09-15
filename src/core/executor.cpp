#include "internal.hpp"

#include "expression.hpp"
#include "tinydbms/storage.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace tinydbms::core {
namespace {

using ExpressionError = internal::expression::Error;
using SlotRow = internal::expression::SlotRow;

using SlotRowResult = std::variant<SlotRow, Error>;
using SlotLookupResult = std::variant<const Value*, Error>;

// Plan 树遍历的防御性深度上限。compiler 的表达式复杂度预算 256 已间接限制 Plan 深度，
// 该检查保证即使 Plan 来自违约实现，core 也以 kInternal 结束而不是递归溢出。
inline constexpr std::size_t kMaxPlanDepth = 256;

[[nodiscard]] Value validation_value(Type type) {
    switch (type) {
        case Type::kInt: return Value{std::int32_t{0}};
        case Type::kBigInt: return Value{std::int64_t{0}};
        case Type::kDouble: return Value{0.0};
        case Type::kBoolean: return Value{false};
        case Type::kVarchar: return Value{std::string{}};
    }
    return Value{std::monostate{}};
}

[[nodiscard]] std::optional<Type> runtime_type(const Value& value) {
    if (std::holds_alternative<std::int32_t>(value.data)) return Type::kInt;
    if (std::holds_alternative<std::int64_t>(value.data)) return Type::kBigInt;
    if (std::holds_alternative<double>(value.data)) return Type::kDouble;
    if (std::holds_alternative<bool>(value.data)) return Type::kBoolean;
    if (std::holds_alternative<std::string>(value.data)) return Type::kVarchar;
    return std::nullopt;
}

const TableMeta* find_table(
    const std::vector<TableMeta>& catalog,
    TableId table_id) {
    for (const TableMeta& table : catalog) {
        if (table.table_id == table_id) {
            return &table;
        }
    }
    return nullptr;
}

ExecuteResult make_internal_error(std::string message) {
    return internal::make_execute_error(ErrorKind::kInternal, std::move(message));
}

ExecuteResult make_expression_error(const ExpressionError& error) {
    return make_internal_error(error.message);
}

bool value_matches_column(const Value& value, const ColumnMeta& column) {
    if (std::holds_alternative<std::monostate>(value.data)) {
        return column.nullable;
    }
    switch (column.type) {
        case Type::kInt:
            return std::holds_alternative<std::int32_t>(value.data);
        case Type::kVarchar:
            return std::holds_alternative<std::string>(value.data);
        case Type::kBigInt:
            return std::holds_alternative<std::int64_t>(value.data);
        case Type::kDouble:
            return std::holds_alternative<double>(value.data);
        case Type::kBoolean:
            return std::holds_alternative<bool>(value.data);
    }
    return false;
}

std::optional<Error> validate_physical_row(
    const TableMeta& table,
    const Row& row) {
    if (row.size() != table.columns.size()) {
        return internal::make_error(
            ErrorKind::kInternal,
            "record row width does not match table schema");
    }
    for (std::size_t index = 0; index < row.size(); ++index) {
        if (!value_matches_column(row[index], table.columns[index])) {
            return internal::make_error(
                ErrorKind::kInternal,
                "record value type does not match table schema");
        }
    }
    return std::nullopt;
}

std::optional<Error> validate_scan_columns(
    const TableMeta& table,
    const std::vector<compiler::ScanColumn>& columns) {
    if (columns.size() != table.columns.size()) {
        return internal::make_error(
            ErrorKind::kInternal,
            "scan column mapping must cover the table schema");
    }

    std::vector<bool> used_columns(table.columns.size(), false);
    std::vector<SlotId> used_slots;
    used_slots.reserve(columns.size());
    for (const compiler::ScanColumn& column : columns) {
        const std::size_t column_index = static_cast<std::size_t>(column.column_id);
        if (column_index >= table.columns.size()) {
            return internal::make_error(
                ErrorKind::kInternal,
                "scan column id is out of range");
        }
        if (used_columns[column_index]) {
            return internal::make_error(
                ErrorKind::kInternal,
                "scan column mapping contains a duplicate column id");
        }
        for (SlotId slot_id : used_slots) {
            if (slot_id == column.output_slot) {
                return internal::make_error(
                    ErrorKind::kInternal,
                    "scan column mapping contains a duplicate output slot");
            }
        }
        used_columns[column_index] = true;
        used_slots.push_back(column.output_slot);
    }
    return std::nullopt;
}

SlotRowResult materialize_slot_row(
    const TableMeta& table,
    const Row& physical_row,
    const std::vector<compiler::ScanColumn>& columns) {
    if (const std::optional<Error> row_error = validate_physical_row(table, physical_row);
        row_error.has_value()) {
        return *row_error;
    }
    if (const std::optional<Error> mapping_error = validate_scan_columns(table, columns);
        mapping_error.has_value()) {
        return *mapping_error;
    }

    SlotRow result;
    result.reserve(columns.size());
    for (const compiler::ScanColumn& column : columns) {
        const std::size_t column_index = static_cast<std::size_t>(column.column_id);
        result.push_back(internal::expression::SlotValue{
            column.output_slot,
            physical_row[column_index]});
    }
    return result;
}

SlotRowResult make_validation_slot_row(
    const TableMeta& table,
    const std::vector<compiler::ScanColumn>& columns) {
    Row physical_row;
    physical_row.reserve(table.columns.size());
    for (const ColumnMeta& column : table.columns) {
        switch (column.type) {
            case Type::kInt:
                physical_row.push_back(Value{std::int32_t{0}});
                break;
            case Type::kVarchar:
                physical_row.push_back(Value{std::string{}});
                break;
            case Type::kBigInt:
                physical_row.push_back(Value{std::int64_t{0}});
                break;
            case Type::kDouble:
                physical_row.push_back(Value{0.0});
                break;
            case Type::kBoolean:
                physical_row.push_back(Value{false});
                break;
        }
    }
    return materialize_slot_row(table, physical_row, columns);
}

SlotLookupResult lookup_slot(const SlotRow& row, SlotId slot_id) {
    const Value* result = nullptr;
    for (const internal::expression::SlotValue& entry : row) {
        if (entry.slot_id != slot_id) {
            continue;
        }
        if (result != nullptr) {
            return internal::make_error(
                ErrorKind::kInternal,
                "slot is bound more than once");
        }
        result = &entry.value;
    }
    if (result == nullptr) {
        return internal::make_error(
            ErrorKind::kInternal,
            "slot is missing or unbound");
    }
    return result;
}

class CursorGuard {
public:
    explicit CursorGuard(storage::CursorId cursor) : cursor_{cursor} {}

    CursorGuard(const CursorGuard&) = delete;
    CursorGuard& operator=(const CursorGuard&) = delete;

    ~CursorGuard() noexcept {
        best_effort_close();
    }

    std::optional<Error> close() {
        if (close_attempted_) {
            return close_error_;
        }

        // Mark before calling storage. An exception means the attempt happened, and
        // retrying could turn a partially closed cursor into a second-close bug.
        close_attempted_ = true;
        try {
            const storage::CloseCursorResult closed =
                storage::close_cursor(storage::CloseCursorRequest{cursor_});
            if (closed.error.has_value()) {
                close_error_ = internal::map_storage_error(*closed.error);
            }
        } catch (const std::exception& exception) {
            close_threw_ = true;
            close_error_ = internal::make_error(ErrorKind::kInternal, exception.what());
        } catch (...) {
            close_threw_ = true;
            close_error_ = internal::make_error(
                ErrorKind::kInternal,
                "unknown exception while closing cursor");
        }
        return close_error_;
    }

    bool close_threw() const noexcept {
        return close_threw_;
    }

private:
    void best_effort_close() noexcept {
        if (close_attempted_) {
            return;
        }
        close_attempted_ = true;
        try {
            (void)storage::close_cursor(storage::CloseCursorRequest{cursor_});
        } catch (...) {
            // Destruction cannot report a cleanup error and must never throw.
        }
    }

    storage::CursorId cursor_;
    bool close_attempted_ = false;
    bool close_threw_ = false;
    std::optional<Error> close_error_;
};

void close_ignoring_errors(CursorGuard& cursor) noexcept {
    try {
        (void)cursor.close();
    } catch (...) {
        // Preserve the already determined primary error.
    }
}

template <typename Abort, typename Handler>
std::optional<Error> scan_records(
    CursorGuard& cursor,
    storage::CursorId cursor_id,
    const CancelToken* cancel,
    Abort&& abort,
    Handler&& handler) {
    while (true) {
        // 取消检查点在 storage 调用之前：命中时数组与 cursor 都由既有 RAII 路径回收，
        // 且此时还没有产生任何副作用。
        internal::throw_if_cancelled(cancel);

        storage::ScanNextResult next;
        try {
            next = storage::scan_next(storage::ScanNextRequest{cursor_id});
        } catch (const std::exception& exception) {
            close_ignoring_errors(cursor);
            abort();
            return internal::make_error(ErrorKind::kInternal, exception.what());
        } catch (...) {
            close_ignoring_errors(cursor);
            abort();
            return internal::make_error(
                ErrorKind::kInternal,
                "unknown exception while scanning records");
        }

        if (next.error.has_value() && next.record.has_value()) {
            close_ignoring_errors(cursor);
            if (cursor.close_threw()) {
                abort();
            }
            return internal::make_error(
                ErrorKind::kInternal,
                "scan_next returned both error and record");
        }
        if (next.error.has_value()) {
            Error error = internal::map_storage_error(*next.error);
            close_ignoring_errors(cursor);
            if (cursor.close_threw()) {
                abort();
            }
            return std::optional<Error>{std::move(error)};
        }
        if (!next.record.has_value()) {
            return std::nullopt;
        }

        try {
            std::optional<Error> handler_error = handler(*next.record);
            if (handler_error.has_value()) {
                close_ignoring_errors(cursor);
                if (cursor.close_threw()) {
                    abort();
                }
                return handler_error;
            }
        } catch (const std::exception& exception) {
            close_ignoring_errors(cursor);
            if (cursor.close_threw()) {
                abort();
            }
            return internal::make_error(ErrorKind::kInternal, exception.what());
        } catch (...) {
            close_ignoring_errors(cursor);
            if (cursor.close_threw()) {
                abort();
            }
            return internal::make_error(
                ErrorKind::kInternal,
                "unknown exception while processing a scanned record");
        }
    }
}

}  // namespace

ExecuteResult Database::Impl::execute_create_table(
    const compiler::CreateTablePlan& plan,
    ExecutionContext& context) {
    if (next_table_id > std::numeric_limits<TableId>::max()) {
        return internal::make_execute_error(ErrorKind::kExecute, "table id exhausted");
    }

    const TableId candidate_id = static_cast<TableId>(next_table_id);
    TableMeta metadata{candidate_id, plan.table_name, plan.columns};
    if (auto metadata_error = internal::validate_table_metadata(metadata);
        metadata_error.has_value()) {
        return ExecuteResult{std::move(*metadata_error)};
    }

    try {
        if (catalog.size() == catalog.max_size()) {
            return internal::make_execute_error(
                ErrorKind::kInternal,
                "catalog cannot grow further");
        }
        catalog.reserve(catalog.size() + 1U);
    } catch (const std::exception& exception) {
        return internal::make_execute_error(ErrorKind::kInternal, exception.what());
    } catch (...) {
        return internal::make_execute_error(
            ErrorKind::kInternal,
            "unable to reserve catalog capacity");
    }

    if (internal::has_duplicate_table(catalog, metadata)) {
        return internal::make_execute_error(
            ErrorKind::kExecute,
            "table id or table name already exists");
    }

    storage::CreateTableResult created;
    try {
        context.storage_called = true;
        created = storage::create_table(storage::CreateTableRequest{
            candidate_id,
            plan.table_name,
            plan.columns});
    } catch (const std::exception& exception) {
        abort_after_storage_exception();
        return internal::make_execute_error(ErrorKind::kInternal, exception.what());
    } catch (...) {
        abort_after_storage_exception();
        return internal::make_execute_error(
            ErrorKind::kInternal,
            "unknown exception while creating table");
    }

    if (created.error.has_value()) {
        return ExecuteResult{internal::map_storage_error(*created.error)};
    }

    catalog.push_back(std::move(metadata));
    next_table_id = static_cast<std::uint64_t>(candidate_id) + 1U;
    return ExecuteResult{CommandResult{0, std::nullopt}};
}

ExecuteResult Database::Impl::execute_insert(
    const compiler::InsertPlan& plan,
    ExecutionContext& context) {
    const TableMeta* table = find_table(catalog, plan.table_id);
    if (table == nullptr) {
        return make_internal_error("insert plan references an unknown table");
    }
    if (const std::optional<Error> table_error = internal::validate_table_metadata(*table);
        table_error.has_value()) {
        return ExecuteResult{std::move(*table_error)};
    }
    if (plan.rows.empty()) {
        return make_internal_error("insert plan must contain at least one row");
    }

    const std::size_t column_count = table->columns.size();
    std::vector<bool> used_columns(column_count, false);
    if (!plan.columns.empty()) {
        if (plan.columns.size() != column_count) {
            return make_internal_error("insert column list must contain every table column");
        }
        for (ColumnId column_id : plan.columns) {
            if (column_id >= column_count || used_columns[column_id]) {
                return make_internal_error("insert column list is not a permutation");
            }
            used_columns[column_id] = true;
        }
    }

    std::vector<std::vector<Value>> physical_rows;
    physical_rows.reserve(plan.rows.size());
    for (const std::vector<Value>& input_row : plan.rows) {
        if (input_row.size() != (plan.columns.empty() ? column_count : plan.columns.size())) {
            return make_internal_error("insert row width does not match the plan");
        }

        Row physical_row;
        physical_row.resize(column_count);
        if (plan.columns.empty()) {
            physical_row = input_row;
        } else {
            for (std::size_t index = 0; index < plan.columns.size(); ++index) {
                physical_row[plan.columns[index]] = input_row[index];
            }
        }

        if (const std::optional<Error> row_error =
                validate_physical_row(*table, physical_row);
            row_error.has_value()) {
            return ExecuteResult{*row_error};
        }
        physical_rows.push_back(std::move(physical_row));
    }

    storage::InsertResult inserted;
    try {
        context.storage_called = true;
        inserted = storage::insert(storage::InsertRequest{
            plan.table_id,
            std::move(physical_rows)});
    } catch (const std::exception& exception) {
        abort_after_storage_exception();
        return make_internal_error(exception.what());
    } catch (...) {
        abort_after_storage_exception();
        return make_internal_error("unknown exception while inserting rows");
    }

    if (inserted.rids.size() > plan.rows.size()) {
        abort_after_storage_exception();
        return make_internal_error("storage returned too many inserted record ids");
    }
    if (inserted.error.has_value()) {
        Error error = internal::map_storage_error(*inserted.error);
        if (inserted.rids.empty()) {
            return ExecuteResult{std::move(error)};
        }
        return ExecuteResult{CommandResult{
            static_cast<std::uint64_t>(inserted.rids.size()),
            std::optional<Error>{std::move(error)}}};
    }
    if (inserted.rids.size() != plan.rows.size()) {
        abort_after_storage_exception();
        return make_internal_error("storage returned an incomplete successful insert result");
    }
    return ExecuteResult{CommandResult{
        static_cast<std::uint64_t>(plan.rows.size()),
        std::nullopt}};
}

ExecuteResult Database::Impl::execute_delete(
    const compiler::DeletePlan& plan,
    ExecutionContext& context) {
    const TableMeta* table = find_table(catalog, plan.table_id);
    if (table == nullptr) {
        return make_internal_error("delete plan references an unknown table");
    }
    if (const std::optional<Error> table_error = internal::validate_table_metadata(*table);
        table_error.has_value()) {
        return ExecuteResult{std::move(*table_error)};
    }
    SlotRowResult validation_row = make_validation_slot_row(*table, plan.input_columns);
    if (const Error* error = std::get_if<Error>(&validation_row)) {
        return ExecuteResult{*error};
    }
    if (plan.predicate.has_value()) {
        if (const std::optional<ExpressionError> predicate_error =
                internal::expression::validate_predicate(
                    *plan.predicate,
                    std::get<SlotRow>(validation_row));
            predicate_error.has_value()) {
            return make_expression_error(*predicate_error);
        }
    }

    storage::OpenTableResult opened;
    try {
        context.storage_called = true;
        opened = storage::open_table(storage::OpenTableRequest{plan.table_id});
    } catch (const std::exception& exception) {
        abort_after_storage_exception();
        return make_internal_error(exception.what());
    } catch (...) {
        abort_after_storage_exception();
        return make_internal_error("unknown exception while opening table for delete");
    }

    if (opened.error.has_value()) {
        if (opened.cursor.has_value()) {
            CursorGuard invalid_result_cursor{*opened.cursor};
            close_ignoring_errors(invalid_result_cursor);
            if (invalid_result_cursor.close_threw()) {
                abort_after_storage_exception();
            }
            return make_internal_error("open_table returned both error and cursor");
        }
        return ExecuteResult{internal::map_storage_error(*opened.error)};
    }
    if (!opened.cursor.has_value()) {
        return make_internal_error("open_table returned no cursor on success");
    }

    CursorGuard cursor{*opened.cursor};
    std::vector<storage::RecordId> record_ids;

    const std::optional<Error> scan_error = scan_records(
        cursor,
        *opened.cursor,
        context.cancel_token,
        [this]() noexcept { abort_after_storage_exception(); },
        [&](const storage::Record& record) -> std::optional<Error> {
            SlotRowResult materialized =
                materialize_slot_row(*table, record.values, plan.input_columns);
            if (const Error* error = std::get_if<Error>(&materialized)) {
                // 记录与 schema 不符属于 storage 违约：结束会话，由调用方 close 后重新 open。
                abort_after_storage_exception();
                return *error;
            }
            const SlotRow& row = std::get<SlotRow>(materialized);

            bool matches = true;
            if (plan.predicate.has_value()) {
                const std::variant<bool, ExpressionError> predicate =
                    internal::expression::evaluate_predicate(*plan.predicate, row);
                if (const ExpressionError* error = std::get_if<ExpressionError>(&predicate)) {
                    return internal::make_error(ErrorKind::kInternal, error->message);
                }
                matches = std::get<bool>(predicate);
            }
            if (matches) {
                record_ids.push_back(record.rid);
            }
            return std::nullopt;
        });
    if (scan_error.has_value()) {
        return ExecuteResult{std::move(*scan_error)};
    }

    if (const std::optional<Error> close_error = cursor.close(); close_error.has_value()) {
        if (cursor.close_threw()) {
            abort_after_storage_exception();
        }
        return ExecuteResult{std::move(*close_error)};
    }

    storage::DeleteResult deleted;
    try {
        context.storage_called = true;
        deleted = storage::delete_records(storage::DeleteRequest{
            plan.table_id,
            record_ids});
    } catch (const std::exception& exception) {
        abort_after_storage_exception();
        return make_internal_error(exception.what());
    } catch (...) {
        abort_after_storage_exception();
        return make_internal_error("unknown exception while deleting rows");
    }

    const std::uint64_t requested_count = static_cast<std::uint64_t>(record_ids.size());
    if (deleted.deleted_count > requested_count) {
        abort_after_storage_exception();
        return make_internal_error("storage returned too many deleted rows");
    }
    if (deleted.error.has_value()) {
        Error error = internal::map_storage_error(*deleted.error);
        if (deleted.deleted_count == 0) {
            return ExecuteResult{std::move(error)};
        }
        return ExecuteResult{CommandResult{
            deleted.deleted_count,
            std::optional<Error>{std::move(error)}}};
    }
    if (deleted.deleted_count != requested_count) {
        abort_after_storage_exception();
        return make_internal_error("storage returned an incomplete successful delete result");
    }
    return ExecuteResult{CommandResult{deleted.deleted_count, std::nullopt}};
}

ExecuteResult Database::Impl::execute_update(
    const compiler::UpdatePlan& plan,
    ExecutionContext& context) {
    const TableMeta* table = find_table(catalog, plan.table_id);
    if (table == nullptr) {
        return make_internal_error("update plan references an unknown table");
    }
    if (const std::optional<Error> table_error = internal::validate_table_metadata(*table);
        table_error.has_value()) {
        return ExecuteResult{*table_error};
    }
    if (plan.assignments.empty()) {
        return make_internal_error("update plan must contain assignments");
    }

    SlotRowResult validation_row = make_validation_slot_row(*table, plan.input_columns);
    if (const Error* error = std::get_if<Error>(&validation_row)) {
        return ExecuteResult{*error};
    }

    std::vector<bool> assigned(table->columns.size(), false);
    for (const compiler::UpdateAssignment& assignment : plan.assignments) {
        const std::size_t column_index = static_cast<std::size_t>(assignment.column_id);
        if (column_index >= table->columns.size()) {
            return make_internal_error("update assignment column id is out of range");
        }
        if (assigned[column_index]) {
            return make_internal_error("update plan contains a duplicate assignment column");
        }
        if (!value_matches_column(assignment.value, table->columns[column_index])) {
            return make_internal_error("update assignment value type does not match table schema");
        }
        assigned[column_index] = true;
    }
    if (plan.predicate.has_value()) {
        if (const std::optional<ExpressionError> error = internal::expression::validate_predicate(
                *plan.predicate, std::get<SlotRow>(validation_row));
            error.has_value()) {
            return make_expression_error(*error);
        }
    }

    storage::OpenTableResult opened;
    try {
        context.storage_called = true;
        opened = storage::open_table(storage::OpenTableRequest{plan.table_id});
    } catch (const std::exception& exception) {
        abort_after_storage_exception();
        return make_internal_error(exception.what());
    } catch (...) {
        abort_after_storage_exception();
        return make_internal_error("unknown exception while opening table for update");
    }
    if (opened.error.has_value()) {
        if (opened.cursor.has_value()) {
            CursorGuard invalid_result_cursor{*opened.cursor};
            close_ignoring_errors(invalid_result_cursor);
            if (invalid_result_cursor.close_threw()) {
                abort_after_storage_exception();
            }
            return make_internal_error("open_table returned both error and cursor");
        }
        return ExecuteResult{internal::map_storage_error(*opened.error)};
    }
    if (!opened.cursor.has_value()) {
        return make_internal_error("open_table returned no cursor on success");
    }

    CursorGuard cursor{*opened.cursor};
    std::vector<storage::UpdateRow> updates;
    const std::optional<Error> scan_error = scan_records(
        cursor,
        *opened.cursor,
        context.cancel_token,
        [this]() noexcept { abort_after_storage_exception(); },
        [&](const storage::Record& record) -> std::optional<Error> {
            SlotRowResult materialized =
                materialize_slot_row(*table, record.values, plan.input_columns);
            if (const Error* error = std::get_if<Error>(&materialized)) {
                // 记录与 schema 不符属于 storage 违约：结束会话，由调用方 close 后重新 open。
                abort_after_storage_exception();
                return *error;
            }
            const SlotRow& row = std::get<SlotRow>(materialized);

            if (plan.predicate.has_value()) {
                const std::variant<bool, ExpressionError> predicate =
                    internal::expression::evaluate_predicate(*plan.predicate, row);
                if (const ExpressionError* error = std::get_if<ExpressionError>(&predicate)) {
                    return internal::make_error(ErrorKind::kInternal, error->message);
                }
                if (!std::get<bool>(predicate)) {
                    return std::nullopt;
                }
            }

            Row replacement = record.values;
            for (const compiler::UpdateAssignment& assignment : plan.assignments) {
                replacement[static_cast<std::size_t>(assignment.column_id)] = assignment.value;
            }
            if (const std::optional<Error> row_error = validate_physical_row(*table, replacement);
                row_error.has_value()) {
                return *row_error;
            }
            updates.push_back(storage::UpdateRow{record.rid, std::move(replacement)});
            return std::nullopt;
        });
    if (scan_error.has_value()) {
        return ExecuteResult{*scan_error};
    }
    if (const std::optional<Error> close_error = cursor.close(); close_error.has_value()) {
        if (cursor.close_threw()) {
            abort_after_storage_exception();
        }
        return ExecuteResult{*close_error};
    }

    const std::uint64_t requested = static_cast<std::uint64_t>(updates.size());
    storage::UpdateResult updated;
    try {
        context.storage_called = true;
        updated = storage::update_rows(storage::UpdateRequest{plan.table_id, std::move(updates)});
    } catch (const std::exception& exception) {
        abort_after_storage_exception();
        return make_internal_error(exception.what());
    } catch (...) {
        abort_after_storage_exception();
        return make_internal_error("unknown exception while updating rows");
    }

    if (updated.updated_count > requested) {
        abort_after_storage_exception();
        return make_internal_error("storage returned too many updated rows");
    }
    if (updated.error.has_value()) {
        Error error = internal::map_storage_error(*updated.error);
        if (updated.updated_count == 0) {
            return ExecuteResult{std::move(error)};
        }
        return ExecuteResult{CommandResult{updated.updated_count, std::move(error)}};
    }
    if (updated.updated_count != requested) {
        abort_after_storage_exception();
        return make_internal_error("storage returned an incomplete successful update result");
    }
    return ExecuteResult{CommandResult{updated.updated_count, std::nullopt}};
}

namespace {

struct ValidatedQuery {
    const compiler::PlanNode* input = nullptr;
    std::vector<SlotId> outputs;
    std::vector<ColumnHeader> result_columns;
};

using QueryValidationResult = std::variant<ValidatedQuery, Error>;
using DataflowValidationResult = std::variant<SlotRow, Error>;

[[nodiscard]] SlotRowResult merge_slot_rows(const SlotRow& left, const SlotRow& right) {
    SlotRow merged = left;
    merged.reserve(left.size() + right.size());
    for (const internal::expression::SlotValue& value : right) {
        SlotLookupResult existing = lookup_slot(merged, value.slot_id);
        if (std::holds_alternative<const Value*>(existing)) {
            return internal::make_error(
                ErrorKind::kInternal, "join inputs contain duplicate slot bindings");
        }
        merged.push_back(value);
    }
    return merged;
}

DataflowValidationResult validate_dataflow_node(
    const std::vector<TableMeta>& catalog,
    const compiler::PlanNode& node,
    std::size_t depth) {
    if (depth >= kMaxPlanDepth) {
        return internal::make_error(
            ErrorKind::kInternal, "query plan depth exceeds core limit");
    }

    if (const auto* scan = std::get_if<compiler::SeqScanNode>(&node.kind)) {
        const TableMeta* table = find_table(catalog, scan->table_id);
        if (table == nullptr) {
            return internal::make_error(
                ErrorKind::kInternal, "query plan references an unknown table");
        }
        if (const std::optional<Error> table_error = internal::validate_table_metadata(*table);
            table_error.has_value()) {
            return std::move(*table_error);
        }
        return make_validation_slot_row(*table, scan->columns);
    }

    if (const auto* filter = std::get_if<compiler::FilterNode>(&node.kind)) {
        if (!filter->child) {
            return internal::make_error(ErrorKind::kInternal, "filter node has a null child");
        }
        DataflowValidationResult child =
            validate_dataflow_node(catalog, *filter->child, depth + 1U);
        if (const Error* error = std::get_if<Error>(&child)) {
            return *error;
        }
        SlotRow row = std::get<SlotRow>(std::move(child));
        if (const std::optional<ExpressionError> error =
                internal::expression::validate_predicate(filter->predicate, row);
            error.has_value()) {
            return internal::make_error(ErrorKind::kInternal, error->message);
        }
        return row;
    }

    if (const auto* sort = std::get_if<compiler::SortNode>(&node.kind)) {
        if (sort->keys.empty()) {
            return internal::make_error(ErrorKind::kInternal, "sort keys must not be empty");
        }
        if (!sort->child) {
            return internal::make_error(ErrorKind::kInternal, "sort node has a null child");
        }
        DataflowValidationResult child =
            validate_dataflow_node(catalog, *sort->child, depth + 1U);
        if (const Error* error = std::get_if<Error>(&child)) {
            return *error;
        }
        SlotRow row = std::get<SlotRow>(std::move(child));
        for (const compiler::SortKey& key : sort->keys) {
            SlotLookupResult slot = lookup_slot(row, key.slot_id);
            if (const Error* error = std::get_if<Error>(&slot)) {
                return *error;
            }
        }
        return row;
    }

    if (const auto* join = std::get_if<compiler::JoinNode>(&node.kind)) {
        if (!join->left || !join->right) {
            return internal::make_error(ErrorKind::kInternal, "join node has a null child");
        }
        DataflowValidationResult left =
            validate_dataflow_node(catalog, *join->left, depth + 1U);
        if (const Error* error = std::get_if<Error>(&left)) {
            return *error;
        }
        DataflowValidationResult right =
            validate_dataflow_node(catalog, *join->right, depth + 1U);
        if (const Error* error = std::get_if<Error>(&right)) {
            return *error;
        }
        SlotRowResult merged = merge_slot_rows(
            std::get<SlotRow>(left), std::get<SlotRow>(right));
        if (const Error* error = std::get_if<Error>(&merged)) {
            return *error;
        }
        SlotRow row = std::get<SlotRow>(std::move(merged));
        if (const std::optional<ExpressionError> error =
                internal::expression::validate_predicate(join->condition, row);
            error.has_value()) {
            return internal::make_error(ErrorKind::kInternal, error->message);
        }
        return row;
    }

    if (const auto* aggregate = std::get_if<compiler::AggregateNode>(&node.kind)) {
        if (!aggregate->child) {
            return internal::make_error(
                ErrorKind::kInternal, "aggregate node has a null child");
        }
        DataflowValidationResult child =
            validate_dataflow_node(catalog, *aggregate->child, depth + 1U);
        if (const Error* error = std::get_if<Error>(&child)) {
            return *error;
        }
        const SlotRow& child_row = std::get<SlotRow>(child);
        SlotRow output;
        output.reserve(aggregate->group_keys.size() + aggregate->aggregates.size());
        for (SlotId slot_id : aggregate->group_keys) {
            SlotLookupResult value = lookup_slot(child_row, slot_id);
            if (const Error* error = std::get_if<Error>(&value)) {
                return *error;
            }
            if (std::any_of(
                    output.begin(), output.end(), [slot_id](const auto& entry) {
                        return entry.slot_id == slot_id;
                    })) {
                return internal::make_error(
                    ErrorKind::kInternal, "aggregate contains a duplicate group slot");
            }
            output.push_back(internal::expression::SlotValue{
                slot_id, **std::get_if<const Value*>(&value)});
        }
        for (const compiler::AggregateCall& call : aggregate->aggregates) {
            SlotLookupResult collision = lookup_slot(child_row, call.output_slot);
            if (std::holds_alternative<const Value*>(collision) ||
                std::any_of(
                    output.begin(), output.end(), [&call](const auto& entry) {
                        return entry.slot_id == call.output_slot;
                    })) {
                return internal::make_error(
                    ErrorKind::kInternal,
                    "aggregate output slot collides with an existing slot");
            }

            std::optional<Type> input_type;
            if (call.input_slot.has_value()) {
                SlotLookupResult input = lookup_slot(child_row, *call.input_slot);
                if (const Error* error = std::get_if<Error>(&input)) {
                    return *error;
                }
                input_type = runtime_type(**std::get_if<const Value*>(&input));
            }
            const bool numeric = input_type == Type::kInt ||
                input_type == Type::kBigInt || input_type == Type::kDouble;
            bool valid = true;
            switch (call.kind) {
                case compiler::AggregateKind::kCount:
                    valid = call.output_type == Type::kBigInt && !call.nullable;
                    break;
                case compiler::AggregateKind::kSum:
                    valid = call.input_slot.has_value() && numeric && call.nullable &&
                        call.output_type ==
                            (input_type == Type::kDouble ? Type::kDouble : Type::kBigInt);
                    break;
                case compiler::AggregateKind::kAvg:
                    valid = call.input_slot.has_value() && numeric && call.nullable &&
                        call.output_type == Type::kDouble;
                    break;
                case compiler::AggregateKind::kMin:
                case compiler::AggregateKind::kMax:
                    valid = call.input_slot.has_value() && input_type.has_value() &&
                        *input_type != Type::kBoolean && call.nullable &&
                        call.output_type == *input_type;
                    break;
                default:
                    valid = false;
                    break;
            }
            if (call.kind != compiler::AggregateKind::kCount &&
                !call.input_slot.has_value()) {
                valid = false;
            }
            if (!valid) {
                return internal::make_error(
                    ErrorKind::kInternal, "aggregate call has an invalid type contract");
            }
            output.push_back(internal::expression::SlotValue{
                call.output_slot, validation_value(call.output_type)});
        }
        return output;
    }

    return internal::make_error(
        ErrorKind::kInternal, "query plan has an invalid node topology");
}

QueryValidationResult validate_query_plan(
    const std::vector<TableMeta>& catalog,
    const compiler::QueryPlan& plan) {
    if (!plan.root) {
        return internal::make_error(ErrorKind::kInternal, "query plan has a null root");
    }

    const auto* project = std::get_if<compiler::ProjectNode>(&plan.root->kind);
    if (project != nullptr && !project->child) {
        return internal::make_error(ErrorKind::kInternal, "project node has a null child");
    }
    if (project != nullptr && project->outputs.empty()) {
        return internal::make_error(ErrorKind::kInternal, "project outputs must not be empty");
    }
    const compiler::PlanNode* input = project == nullptr
        ? plan.root.get()
        : project->child.get();
    DataflowValidationResult validation_row =
        validate_dataflow_node(catalog, *input, 1U);
    if (const Error* error = std::get_if<Error>(&validation_row)) {
        return *error;
    }

    ValidatedQuery result;
    result.input = input;
    if (project == nullptr) {
        for (const internal::expression::SlotValue& value : std::get<SlotRow>(validation_row)) {
            result.outputs.push_back(value.slot_id);
        }
    } else {
        result.outputs = project->outputs;
    }

    if (result.outputs.size() != plan.outputs.size()) {
        return internal::make_error(
            ErrorKind::kInternal,
            "query output count does not match projected row width");
    }
    for (SlotId slot_id : result.outputs) {
        SlotLookupResult slot = lookup_slot(std::get<SlotRow>(validation_row), slot_id);
        if (const Error* error = std::get_if<Error>(&slot)) {
            return *error;
        }
    }

    result.result_columns.reserve(plan.outputs.size());
    for (std::size_t index = 0; index < plan.outputs.size(); ++index) {
        const compiler::QueryOutput& output = plan.outputs[index];
        if (result.outputs[index] != output.slot_id) {
            return internal::make_error(
                ErrorKind::kInternal,
                "query output slot does not match project position");
        }
        result.result_columns.push_back(ColumnHeader{output.name, output.type});
    }
    return result;
}

enum class SortValueFamily {
    kNumeric,
    kBoolean,
    kVarchar
};

[[nodiscard]] bool is_null(const Value& value) {
    return std::holds_alternative<std::monostate>(value.data);
}

[[nodiscard]] SortValueFamily sort_value_family(const Value& value) {
    if (std::holds_alternative<std::int32_t>(value.data) ||
        std::holds_alternative<std::int64_t>(value.data) ||
        std::holds_alternative<double>(value.data)) {
        return SortValueFamily::kNumeric;
    }
    if (std::holds_alternative<bool>(value.data)) {
        return SortValueFamily::kBoolean;
    }
    return SortValueFamily::kVarchar;
}

[[nodiscard]] bool is_finite_sort_value(const Value& value) {
    const auto* number = std::get_if<double>(&value.data);
    return number == nullptr || std::isfinite(*number);
}

[[nodiscard]] double sort_numeric_double(const Value& value) {
    if (const auto* number = std::get_if<double>(&value.data)) {
        return *number;
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value.data)) {
        return static_cast<double>(*integer);
    }
    return static_cast<double>(std::get<std::int32_t>(value.data));
}

[[nodiscard]] int compare_sort_values(const Value& lhs, const Value& rhs) {
    if (std::holds_alternative<double>(lhs.data) ||
        std::holds_alternative<double>(rhs.data)) {
        const double lhs_number = sort_numeric_double(lhs);
        const double rhs_number = sort_numeric_double(rhs);
        return lhs_number < rhs_number ? -1 : (rhs_number < lhs_number ? 1 : 0);
    }
    if (std::holds_alternative<std::int32_t>(lhs.data) ||
        std::holds_alternative<std::int64_t>(lhs.data)) {
        const std::int64_t lhs_integer = std::holds_alternative<std::int64_t>(lhs.data)
            ? std::get<std::int64_t>(lhs.data)
            : static_cast<std::int64_t>(std::get<std::int32_t>(lhs.data));
        const std::int64_t rhs_integer = std::holds_alternative<std::int64_t>(rhs.data)
            ? std::get<std::int64_t>(rhs.data)
            : static_cast<std::int64_t>(std::get<std::int32_t>(rhs.data));
        return lhs_integer < rhs_integer ? -1 : (rhs_integer < lhs_integer ? 1 : 0);
    }
    if (const auto* lhs_boolean = std::get_if<bool>(&lhs.data)) {
        const bool rhs_boolean = std::get<bool>(rhs.data);
        return *lhs_boolean == rhs_boolean ? 0 : (*lhs_boolean ? 1 : -1);
    }
    const std::string& lhs_text = std::get<std::string>(lhs.data);
    const std::string& rhs_text = std::get<std::string>(rhs.data);
    return lhs_text < rhs_text ? -1 : (rhs_text < lhs_text ? 1 : 0);
}

struct SortableRow {
    SlotRow row;
    std::vector<Value> keys;
};

using SortRowsResult = std::variant<std::vector<SortableRow>, Error>;
using DataflowRowsResult = std::variant<std::vector<SlotRow>, Error>;

struct AggregateState {
    std::int64_t count{0};
    std::optional<Value> value;
    long double average_sum{0.0L};
};

struct AggregateGroup {
    std::vector<Value> keys;
    std::vector<AggregateState> states;
};

[[nodiscard]] bool grouping_value_equal(const Value& lhs, const Value& rhs) {
    if (std::holds_alternative<std::monostate>(lhs.data) ||
        std::holds_alternative<std::monostate>(rhs.data)) {
        return std::holds_alternative<std::monostate>(lhs.data) &&
            std::holds_alternative<std::monostate>(rhs.data);
    }
    return lhs.data == rhs.data;
}

[[nodiscard]] bool grouping_keys_equal(
    const std::vector<Value>& lhs,
    const std::vector<Value>& rhs) {
    return lhs.size() == rhs.size() && std::equal(
        lhs.begin(), lhs.end(), rhs.begin(), grouping_value_equal);
}

[[nodiscard]] std::variant<std::int64_t, Error> checked_increment(
    std::int64_t value) {
    if (value == std::numeric_limits<std::int64_t>::max()) {
        return internal::make_error(ErrorKind::kExecute, "aggregate count overflow");
    }
    return value + 1;
}

[[nodiscard]] std::variant<std::int64_t, Error> checked_add(
    std::int64_t lhs,
    std::int64_t rhs) {
    if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs)) {
        return internal::make_error(ErrorKind::kExecute, "aggregate SUM overflow");
    }
    return lhs + rhs;
}

[[nodiscard]] std::variant<bool, Error> accumulate_aggregate(
    const compiler::AggregateCall& call,
    const SlotRow& row,
    AggregateState& state) {
    const Value* input = nullptr;
    if (call.input_slot.has_value()) {
        SlotLookupResult found = lookup_slot(row, *call.input_slot);
        if (const Error* error = std::get_if<Error>(&found)) return *error;
        input = *std::get_if<const Value*>(&found);
    }
    if (call.kind == compiler::AggregateKind::kCount) {
        if (input != nullptr && std::holds_alternative<std::monostate>(input->data)) {
            return true;
        }
        auto incremented = checked_increment(state.count);
        if (const Error* error = std::get_if<Error>(&incremented)) return *error;
        state.count = std::get<std::int64_t>(incremented);
        return true;
    }
    if (input == nullptr || std::holds_alternative<std::monostate>(input->data)) {
        return true;
    }

    if (call.kind == compiler::AggregateKind::kSum) {
        if (call.output_type == Type::kDouble) {
            const auto* number = std::get_if<double>(&input->data);
            if (number == nullptr) {
                return internal::make_error(
                    ErrorKind::kInternal, "aggregate SUM input type mismatch");
            }
            const double current = state.value.has_value()
                ? std::get<double>(state.value->data)
                : 0.0;
            const double sum = current + *number;
            if (!std::isfinite(sum)) {
                return internal::make_error(
                    ErrorKind::kExecute, "aggregate SUM produced a non-finite DOUBLE");
            }
            state.value = Value{sum};
            return true;
        }
        std::int64_t operand = 0;
        if (const auto* value = std::get_if<std::int32_t>(&input->data)) {
            operand = *value;
        } else if (const auto* value = std::get_if<std::int64_t>(&input->data)) {
            operand = *value;
        } else {
            return internal::make_error(
                ErrorKind::kInternal, "aggregate SUM input type mismatch");
        }
        const std::int64_t current = state.value.has_value()
            ? std::get<std::int64_t>(state.value->data)
            : 0;
        auto sum = checked_add(current, operand);
        if (const Error* error = std::get_if<Error>(&sum)) return *error;
        state.value = Value{std::get<std::int64_t>(sum)};
        return true;
    }

    if (call.kind == compiler::AggregateKind::kAvg) {
        long double operand = 0.0L;
        if (const auto* value = std::get_if<std::int32_t>(&input->data)) {
            operand = static_cast<long double>(*value);
        } else if (const auto* value = std::get_if<std::int64_t>(&input->data)) {
            operand = static_cast<long double>(*value);
        } else if (const auto* value = std::get_if<double>(&input->data)) {
            operand = static_cast<long double>(*value);
        } else {
            return internal::make_error(
                ErrorKind::kInternal, "aggregate AVG input type mismatch");
        }
        auto incremented = checked_increment(state.count);
        if (const Error* error = std::get_if<Error>(&incremented)) return *error;
        state.count = std::get<std::int64_t>(incremented);
        state.average_sum += operand;
        if (!std::isfinite(state.average_sum)) {
            return internal::make_error(
                ErrorKind::kExecute, "aggregate AVG produced a non-finite value");
        }
        return true;
    }

    if (call.kind != compiler::AggregateKind::kMin &&
        call.kind != compiler::AggregateKind::kMax) {
        return internal::make_error(
            ErrorKind::kInternal, "unknown aggregate function");
    }

    if (!state.value.has_value()) {
        state.value = *input;
        return true;
    }
    const int comparison = compare_sort_values(*input, *state.value);
    if ((call.kind == compiler::AggregateKind::kMin && comparison < 0) ||
        (call.kind == compiler::AggregateKind::kMax && comparison > 0)) {
        state.value = *input;
    }
    return true;
}

[[nodiscard]] std::variant<Value, Error> finish_aggregate(
    const compiler::AggregateCall& call,
    const AggregateState& state) {
    if (call.kind == compiler::AggregateKind::kCount) return Value{state.count};
    if (call.kind == compiler::AggregateKind::kAvg) {
        if (state.count == 0) return Value{std::monostate{}};
        const double average = static_cast<double>(
            state.average_sum / static_cast<long double>(state.count));
        if (!std::isfinite(average)) {
            return internal::make_error(
                ErrorKind::kExecute, "aggregate AVG produced a non-finite DOUBLE");
        }
        return Value{average};
    }
    return state.value.has_value() ? *state.value : Value{std::monostate{}};
}

[[nodiscard]] SortRowsResult prepare_sort_rows(
    std::vector<SlotRow> rows,
    const std::vector<compiler::SortKey>& keys) {
    std::vector<std::optional<SortValueFamily>> families(keys.size());
    std::vector<SortableRow> sortable_rows;
    sortable_rows.reserve(rows.size());
    for (SlotRow& row : rows) {
        SortableRow sortable{std::move(row), {}};
        sortable.keys.reserve(keys.size());
        for (std::size_t index = 0; index < keys.size(); ++index) {
            SlotLookupResult lookup = lookup_slot(sortable.row, keys[index].slot_id);
            if (const Error* error = std::get_if<Error>(&lookup)) {
                return *error;
            }
            const Value& value = **std::get_if<const Value*>(&lookup);
            if (!is_finite_sort_value(value)) {
                return internal::make_error(
                    ErrorKind::kInternal,
                    "sort key contains a non-finite DOUBLE value");
            }
            if (!is_null(value)) {
                const SortValueFamily family = sort_value_family(value);
                if (families[index].has_value() && *families[index] != family) {
                    return internal::make_error(
                        ErrorKind::kInternal,
                        "sort key contains values from incompatible type families");
                }
                families[index] = family;
            }
            sortable.keys.push_back(value);
        }
        sortable_rows.push_back(std::move(sortable));
    }
    return sortable_rows;
}

void sort_rows(
    std::vector<SortableRow>& rows,
    const std::vector<compiler::SortKey>& keys) {
    std::stable_sort(
        rows.begin(),
        rows.end(),
        [&keys](const SortableRow& lhs, const SortableRow& rhs) {
            for (std::size_t index = 0; index < keys.size(); ++index) {
                const Value& lhs_value = lhs.keys[index];
                const Value& rhs_value = rhs.keys[index];
                const bool lhs_null = is_null(lhs_value);
                const bool rhs_null = is_null(rhs_value);
                if (lhs_null || rhs_null) {
                    if (lhs_null != rhs_null) {
                        return !lhs_null;
                    }
                    continue;
                }
                const int comparison = compare_sort_values(lhs_value, rhs_value);
                if (comparison == 0) {
                    continue;
                }
                return keys[index].direction == compiler::SortDirection::kAsc
                    ? comparison < 0
                    : comparison > 0;
            }
            return false;
        });
}

}  // namespace

ExecuteResult Database::Impl::execute_query(
    const compiler::QueryPlan& plan,
    const std::size_t max_query_rows,
    ExecutionContext& context) {
    QueryValidationResult validation = validate_query_plan(catalog, plan);
    if (const Error* error = std::get_if<Error>(&validation)) {
        return ExecuteResult{*error};
    }
    ValidatedQuery query = std::move(std::get<ValidatedQuery>(validation));

    QueryResult result;
    result.columns = std::move(query.result_columns);
    const auto execute_node = [this, max_query_rows, &context](
                                  auto&& self,
                                  const compiler::PlanNode& node,
                                  std::size_t depth) -> DataflowRowsResult {
        if (depth >= kMaxPlanDepth) {
            return internal::make_error(
                ErrorKind::kInternal, "query plan depth exceeds core limit");
        }

        if (const auto* scan = std::get_if<compiler::SeqScanNode>(&node.kind)) {
            const TableMeta* table = find_table(catalog, scan->table_id);
            if (table == nullptr) {
                return internal::make_error(
                    ErrorKind::kInternal, "query plan references an unknown table");
            }
            storage::OpenTableResult opened;
            try {
                context.storage_called = true;
                opened = storage::open_table(storage::OpenTableRequest{table->table_id});
            } catch (const std::exception& exception) {
                abort_after_storage_exception();
                return internal::make_error(ErrorKind::kInternal, exception.what());
            } catch (...) {
                abort_after_storage_exception();
                return internal::make_error(
                    ErrorKind::kInternal, "unknown exception while opening table for query");
            }
            if (opened.error.has_value()) {
                if (opened.cursor.has_value()) {
                    CursorGuard invalid_result_cursor{*opened.cursor};
                    close_ignoring_errors(invalid_result_cursor);
                    if (invalid_result_cursor.close_threw()) {
                        abort_after_storage_exception();
                    }
                    return internal::make_error(
                        ErrorKind::kInternal, "open_table returned both error and cursor");
                }
                return internal::map_storage_error(*opened.error);
            }
            if (!opened.cursor.has_value()) {
                return internal::make_error(
                    ErrorKind::kInternal, "open_table returned no cursor on success");
            }

            std::vector<SlotRow> rows;
            CursorGuard cursor{*opened.cursor};
            const std::optional<Error> scan_error = scan_records(
                cursor,
                *opened.cursor,
                context.cancel_token,
                [this]() noexcept { abort_after_storage_exception(); },
                [&](const storage::Record& record) -> std::optional<Error> {
                    SlotRowResult materialized =
                        materialize_slot_row(*table, record.values, scan->columns);
                    if (const Error* error = std::get_if<Error>(&materialized)) {
                        // 记录与 schema 不符属于 storage 违约：当前会话的数据流不再可信，
                        // 与数量不变量违约一样结束会话，由调用方 close 后重新 open。
                        abort_after_storage_exception();
                        return *error;
                    }
                    if (rows.size() >= max_query_rows) {
                        return internal::make_error(
                            ErrorKind::kExecute,
                            "query materialization exceeds the maximum row count (limit " +
                                std::to_string(max_query_rows) + ")",
                            std::string{"raise max_query_rows or narrow the query"});
                    }
                    rows.push_back(std::get<SlotRow>(std::move(materialized)));
                    return std::nullopt;
                });
            if (scan_error.has_value()) {
                return std::move(*scan_error);
            }
            if (const std::optional<Error> close_error = cursor.close(); close_error.has_value()) {
                if (cursor.close_threw()) {
                    abort_after_storage_exception();
                }
                return std::move(*close_error);
            }
            return rows;
        }

        if (const auto* filter = std::get_if<compiler::FilterNode>(&node.kind)) {
            DataflowRowsResult child = self(self, *filter->child, depth + 1U);
            if (const Error* error = std::get_if<Error>(&child)) {
                return *error;
            }
            std::vector<SlotRow> filtered;
            for (SlotRow& row : std::get<std::vector<SlotRow>>(child)) {
                const std::variant<bool, ExpressionError> predicate =
                    internal::expression::evaluate_predicate(filter->predicate, row);
                if (const ExpressionError* error = std::get_if<ExpressionError>(&predicate)) {
                    return internal::make_error(ErrorKind::kInternal, error->message);
                }
                if (std::get<bool>(predicate)) {
                    filtered.push_back(std::move(row));
                }
            }
            return filtered;
        }

        if (const auto* sort = std::get_if<compiler::SortNode>(&node.kind)) {
            DataflowRowsResult child = self(self, *sort->child, depth + 1U);
            if (const Error* error = std::get_if<Error>(&child)) {
                return *error;
            }
            SortRowsResult prepared = prepare_sort_rows(
                std::get<std::vector<SlotRow>>(std::move(child)), sort->keys);
            if (const Error* error = std::get_if<Error>(&prepared)) {
                return *error;
            }
            std::vector<SortableRow> sortable =
                std::get<std::vector<SortableRow>>(std::move(prepared));
            sort_rows(sortable, sort->keys);
            std::vector<SlotRow> rows;
            rows.reserve(sortable.size());
            for (SortableRow& row : sortable) {
                internal::throw_if_cancelled(context.cancel_token);
                rows.push_back(std::move(row.row));
            }
            return rows;
        }

        if (const auto* join = std::get_if<compiler::JoinNode>(&node.kind)) {
            DataflowRowsResult left_result = self(self, *join->left, depth + 1U);
            if (const Error* error = std::get_if<Error>(&left_result)) {
                return *error;
            }
            DataflowRowsResult right_result = self(self, *join->right, depth + 1U);
            if (const Error* error = std::get_if<Error>(&right_result)) {
                return *error;
            }
            const std::vector<SlotRow>& left_rows =
                std::get<std::vector<SlotRow>>(left_result);
            const std::vector<SlotRow>& right_rows =
                std::get<std::vector<SlotRow>>(right_result);
            std::vector<SlotRow> joined;
            for (const SlotRow& left : left_rows) {
                for (const SlotRow& right : right_rows) {
                    internal::throw_if_cancelled(context.cancel_token);
                    SlotRowResult merged = merge_slot_rows(left, right);
                    if (const Error* error = std::get_if<Error>(&merged)) {
                        return *error;
                    }
                    SlotRow row = std::get<SlotRow>(std::move(merged));
                    const std::variant<bool, ExpressionError> matches =
                        internal::expression::evaluate_predicate(join->condition, row);
                    if (const ExpressionError* error = std::get_if<ExpressionError>(&matches)) {
                        return internal::make_error(ErrorKind::kInternal, error->message);
                    }
                    if (std::get<bool>(matches)) {
                        if (joined.size() >= max_query_rows) {
                            return internal::make_error(
                                ErrorKind::kExecute,
                                "join materialization exceeds the maximum row count (limit " +
                                    std::to_string(max_query_rows) + ")",
                                std::string{"raise max_query_rows or narrow the query"});
                        }
                        joined.push_back(std::move(row));
                    }
                }
            }
            return joined;
        }

        if (const auto* aggregate = std::get_if<compiler::AggregateNode>(&node.kind)) {
            DataflowRowsResult child_result = self(self, *aggregate->child, depth + 1U);
            if (const Error* error = std::get_if<Error>(&child_result)) {
                return *error;
            }
            std::vector<AggregateGroup> groups;
            if (aggregate->group_keys.empty()) {
                groups.push_back(AggregateGroup{
                    {}, std::vector<AggregateState>(aggregate->aggregates.size())});
            }
            for (const SlotRow& row : std::get<std::vector<SlotRow>>(child_result)) {
                internal::throw_if_cancelled(context.cancel_token);
                std::vector<Value> keys;
                keys.reserve(aggregate->group_keys.size());
                for (SlotId slot_id : aggregate->group_keys) {
                    SlotLookupResult value = lookup_slot(row, slot_id);
                    if (const Error* error = std::get_if<Error>(&value)) return *error;
                    keys.push_back(**std::get_if<const Value*>(&value));
                }
                auto group = std::find_if(
                    groups.begin(), groups.end(), [&keys](const AggregateGroup& candidate) {
                        return grouping_keys_equal(candidate.keys, keys);
                    });
                if (group == groups.end()) {
                    groups.push_back(AggregateGroup{
                        std::move(keys),
                        std::vector<AggregateState>(aggregate->aggregates.size())});
                    group = groups.end() - 1;
                }
                for (std::size_t index = 0; index < aggregate->aggregates.size(); ++index) {
                    auto accumulated = accumulate_aggregate(
                        aggregate->aggregates[index], row, group->states[index]);
                    if (const Error* error = std::get_if<Error>(&accumulated)) return *error;
                }
            }

            std::vector<SlotRow> rows;
            rows.reserve(groups.size());
            for (const AggregateGroup& group : groups) {
                SlotRow row;
                row.reserve(aggregate->group_keys.size() + aggregate->aggregates.size());
                for (std::size_t index = 0; index < aggregate->group_keys.size(); ++index) {
                    row.push_back(internal::expression::SlotValue{
                        aggregate->group_keys[index], group.keys[index]});
                }
                for (std::size_t index = 0; index < aggregate->aggregates.size(); ++index) {
                    auto value = finish_aggregate(
                        aggregate->aggregates[index], group.states[index]);
                    if (const Error* error = std::get_if<Error>(&value)) return *error;
                    row.push_back(internal::expression::SlotValue{
                        aggregate->aggregates[index].output_slot,
                        std::get<Value>(std::move(value))});
                }
                rows.push_back(std::move(row));
            }
            return rows;
        }

        return internal::make_error(
            ErrorKind::kInternal, "query execution encountered an invalid node topology");
    };

    DataflowRowsResult executed = execute_node(execute_node, *query.input, 1U);
    if (const Error* error = std::get_if<Error>(&executed)) {
        return ExecuteResult{*error};
    }
    std::vector<SlotRow> rows =
        std::get<std::vector<SlotRow>>(std::move(executed));
    result.rows.reserve(rows.size());
    for (const SlotRow& row : rows) {
        Row projected;
        projected.reserve(query.outputs.size());
        for (SlotId slot_id : query.outputs) {
            SlotLookupResult value = lookup_slot(row, slot_id);
            if (const Error* error = std::get_if<Error>(&value)) {
                return ExecuteResult{*error};
            }
            projected.push_back(**std::get_if<const Value*>(&value));
        }
        if (projected.size() != result.columns.size()) {
            return make_internal_error(
                "projected row width does not match query output count");
        }
        result.rows.push_back(std::move(projected));
    }
    return ExecuteResult{std::move(result)};
}

PlanExecutionResult Database::Impl::execute_plan_impl(
    compiler::Plan plan,
    const std::size_t max_query_rows,
    const CancelToken* cancel) {
    if (!open) {
        return PlanExecutionResult{
            internal::make_execute_error(ErrorKind::kExecute, "database is not open"),
            false};
    }

    // 临时状态只属于本次调用：取消令牌与"是否调用过 Storage"都放在栈上上下文里，
    // 不再写进共享的 Impl，避免并发调用互相覆盖。
    ExecutionContext context;
    context.cancel_token = cancel;

    ExecuteResult result = internal::make_execute_error(
        ErrorKind::kInternal, "statement execution did not complete");
    bool cancelled = false;
    try {
        result = std::visit(
            [this, max_query_rows, &context](auto&& typed_plan) -> ExecuteResult {
                using PlanType = std::decay_t<decltype(typed_plan)>;

                if constexpr (std::is_same_v<PlanType, compiler::CreateTablePlan>) {
                    return execute_create_table(typed_plan, context);
                } else if constexpr (std::is_same_v<PlanType, compiler::InsertPlan>) {
                    return execute_insert(typed_plan, context);
                } else if constexpr (std::is_same_v<PlanType, compiler::DeletePlan>) {
                    return execute_delete(typed_plan, context);
                } else if constexpr (std::is_same_v<PlanType, compiler::UpdatePlan>) {
                    return execute_update(typed_plan, context);
                } else {
                    return execute_query(typed_plan, max_query_rows, context);
                }
            },
            std::move(plan.kind));
    } catch (const internal::CancellationSignal&) {
        // 取消只发生在无副作用检查点：不产生 outcome，也不使会话失效。
        cancelled = true;
    } catch (const std::exception& exception) {
        abort_after_storage_exception();
        result = make_internal_error(exception.what());
    } catch (...) {
        abort_after_storage_exception();
        result = make_internal_error("unknown exception while executing plan");
    }

    return PlanExecutionResult{std::move(result), cancelled, context.storage_called};
}

}  // namespace tinydbms::core
