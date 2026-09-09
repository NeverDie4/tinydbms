#include "internal.hpp"

#include "expression.hpp"
#include "tinydbms/storage.hpp"

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
            close_error_ = internal::make_error(ErrorKind::kInternal, exception.what());
        } catch (...) {
            close_error_ = internal::make_error(
                ErrorKind::kInternal,
                "unknown exception while closing cursor");
        }
        return close_error_;
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
    std::optional<Error> close_error_;
};

void close_ignoring_errors(CursorGuard& cursor) noexcept {
    try {
        (void)cursor.close();
    } catch (...) {
        // Preserve the already determined primary error.
    }
}

template <typename Handler>
std::optional<Error> scan_records(
    CursorGuard& cursor,
    storage::CursorId cursor_id,
    Handler&& handler) {
    while (true) {
        storage::ScanNextResult next;
        try {
            next = storage::scan_next(storage::ScanNextRequest{cursor_id});
        } catch (const std::exception& exception) {
            close_ignoring_errors(cursor);
            return internal::make_error(ErrorKind::kInternal, exception.what());
        } catch (...) {
            close_ignoring_errors(cursor);
            return internal::make_error(
                ErrorKind::kInternal,
                "unknown exception while scanning records");
        }

        if (next.error.has_value() && next.record.has_value()) {
            close_ignoring_errors(cursor);
            return internal::make_error(
                ErrorKind::kInternal,
                "scan_next returned both error and record");
        }
        if (next.error.has_value()) {
            Error error = internal::map_storage_error(*next.error);
            close_ignoring_errors(cursor);
            return std::optional<Error>{std::move(error)};
        }
        if (!next.record.has_value()) {
            return std::nullopt;
        }

        try {
            std::optional<Error> handler_error = handler(*next.record);
            if (handler_error.has_value()) {
                close_ignoring_errors(cursor);
                return handler_error;
            }
        } catch (const std::exception& exception) {
            close_ignoring_errors(cursor);
            return internal::make_error(ErrorKind::kInternal, exception.what());
        } catch (...) {
            close_ignoring_errors(cursor);
            return internal::make_error(
                ErrorKind::kInternal,
                "unknown exception while processing a scanned record");
        }
    }
}

}  // namespace

ExecuteResult Database::Impl::execute_create_table(
    const compiler::CreateTablePlan& plan) {
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
        created = storage::create_table(storage::CreateTableRequest{
            candidate_id,
            plan.table_name,
            plan.columns});
    } catch (const std::exception& exception) {
        return internal::make_execute_error(ErrorKind::kInternal, exception.what());
    } catch (...) {
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
    const compiler::InsertPlan& plan) {
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

        if (const std::optional<ExpressionError> row_error =
                internal::expression::validate_row(*table, physical_row);
            row_error.has_value()) {
            return make_expression_error(*row_error);
        }
        physical_rows.push_back(std::move(physical_row));
    }

    storage::InsertResult inserted;
    try {
        inserted = storage::insert(storage::InsertRequest{
            plan.table_id,
            std::move(physical_rows)});
    } catch (const std::exception& exception) {
        return make_internal_error(exception.what());
    } catch (...) {
        return make_internal_error("unknown exception while inserting rows");
    }

    if (inserted.rids.size() > plan.rows.size()) {
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
        return make_internal_error("storage returned an incomplete successful insert result");
    }
    return ExecuteResult{CommandResult{
        static_cast<std::uint64_t>(plan.rows.size()),
        std::nullopt}};
}

ExecuteResult Database::Impl::execute_delete(
    const compiler::DeletePlan& plan) {
    const TableMeta* table = find_table(catalog, plan.table_id);
    if (table == nullptr) {
        return make_internal_error("delete plan references an unknown table");
    }
    if (const std::optional<Error> table_error = internal::validate_table_metadata(*table);
        table_error.has_value()) {
        return ExecuteResult{std::move(*table_error)};
    }
    if (plan.predicate.has_value()) {
        if (const std::optional<ExpressionError> predicate_error =
                internal::expression::validate_predicate(*plan.predicate, *table);
            predicate_error.has_value()) {
            return make_expression_error(*predicate_error);
        }
    }

    storage::OpenTableResult opened;
    try {
        opened = storage::open_table(storage::OpenTableRequest{plan.table_id});
    } catch (const std::exception& exception) {
        return make_internal_error(exception.what());
    } catch (...) {
        return make_internal_error("unknown exception while opening table for delete");
    }

    if (opened.error.has_value()) {
        if (opened.cursor.has_value()) {
            CursorGuard invalid_result_cursor{*opened.cursor};
            close_ignoring_errors(invalid_result_cursor);
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
        [&](const storage::Record& record) -> std::optional<Error> {
            const Row& row = record.values;
            if (const std::optional<ExpressionError> row_error =
                    internal::expression::validate_row(*table, row);
                row_error.has_value()) {
                return internal::make_error(ErrorKind::kInternal, row_error->message);
            }

            bool matches = true;
            if (plan.predicate.has_value()) {
                const std::variant<bool, ExpressionError> predicate =
                    internal::expression::evaluate_predicate(*plan.predicate, *table, row);
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
        return ExecuteResult{std::move(*close_error)};
    }

    storage::DeleteResult deleted;
    try {
        deleted = storage::delete_records(storage::DeleteRequest{
            plan.table_id,
            record_ids});
    } catch (const std::exception& exception) {
        return make_internal_error(exception.what());
    } catch (...) {
        return make_internal_error("unknown exception while deleting rows");
    }

    const std::uint64_t requested_count = static_cast<std::uint64_t>(record_ids.size());
    if (deleted.deleted_count > requested_count) {
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
        return make_internal_error("storage returned an incomplete successful delete result");
    }
    return ExecuteResult{CommandResult{deleted.deleted_count, std::nullopt}};
}

namespace {

struct ValidatedQuery {
    const TableMeta* table = nullptr;
    const compiler::Expr* predicate = nullptr;
    std::vector<ColumnId> outputs;
};

using QueryValidationResult = std::variant<ValidatedQuery, Error>;

QueryValidationResult validate_query_plan(
    const std::vector<TableMeta>& catalog,
    const compiler::QueryPlan& plan) {
    if (!plan.root) {
        return internal::make_error(ErrorKind::kInternal, "query plan has a null root");
    }

    const compiler::FilterNode* filter = nullptr;
    const compiler::ProjectNode* project = nullptr;
    const compiler::SeqScanNode* scan = nullptr;
    const compiler::PlanNode* current = plan.root.get();

    if (const auto* scan_node = std::get_if<compiler::SeqScanNode>(&current->kind)) {
        scan = scan_node;
    } else if (const auto* filter_node = std::get_if<compiler::FilterNode>(&current->kind)) {
        filter = filter_node;
        if (!filter->child) {
            return internal::make_error(ErrorKind::kInternal, "filter node has a null child");
        }
        current = filter->child.get();
        scan = std::get_if<compiler::SeqScanNode>(&current->kind);
        if (scan == nullptr) {
            return internal::make_error(
                ErrorKind::kInternal,
                "filter node must be directly above seq scan");
        }
    } else if (const auto* project_node = std::get_if<compiler::ProjectNode>(&current->kind)) {
        project = project_node;
        if (!project->child) {
            return internal::make_error(ErrorKind::kInternal, "project node has a null child");
        }
        current = project->child.get();
        if (const auto* filter_node = std::get_if<compiler::FilterNode>(&current->kind)) {
            filter = filter_node;
            if (!filter->child) {
                return internal::make_error(ErrorKind::kInternal, "filter node has a null child");
            }
            current = filter->child.get();
        }
        scan = std::get_if<compiler::SeqScanNode>(&current->kind);
        if (scan == nullptr) {
            return internal::make_error(
                ErrorKind::kInternal,
                "project plan has an invalid child topology");
        }
    } else {
        return internal::make_error(ErrorKind::kInternal, "query plan has an unknown root topology");
    }

    const TableMeta* table = find_table(catalog, scan->table_id);
    if (table == nullptr) {
        return internal::make_error(ErrorKind::kInternal, "query plan references an unknown table");
    }
    if (const std::optional<Error> table_error = internal::validate_table_metadata(*table);
        table_error.has_value()) {
        return std::move(*table_error);
    }

    if (filter != nullptr) {
        if (const std::optional<ExpressionError> predicate_error =
                internal::expression::validate_predicate(filter->predicate, *table);
            predicate_error.has_value()) {
            return internal::make_error(ErrorKind::kInternal, predicate_error->message);
        }
    }

    ValidatedQuery result;
    result.table = table;
    result.predicate = filter == nullptr ? nullptr : &filter->predicate;
    if (project == nullptr) {
        if (table->columns.size() > std::numeric_limits<ColumnId>::max()) {
            return internal::make_error(ErrorKind::kInternal, "table has too many columns");
        }
        result.outputs.reserve(table->columns.size());
        for (std::size_t index = 0; index < table->columns.size(); ++index) {
            result.outputs.push_back(static_cast<ColumnId>(index));
        }
    } else {
        if (project->outputs.empty()) {
            return internal::make_error(ErrorKind::kInternal, "project outputs must not be empty");
        }
        result.outputs = project->outputs;
        for (ColumnId column_id : result.outputs) {
            if (column_id >= table->columns.size()) {
                return internal::make_error(ErrorKind::kInternal, "project column id is out of range");
            }
        }
    }
    return result;
}

}  // namespace

ExecuteResult Database::Impl::execute_query(
    const compiler::QueryPlan& plan) {
    QueryValidationResult validation = validate_query_plan(catalog, plan);
    if (const Error* error = std::get_if<Error>(&validation)) {
        return ExecuteResult{*error};
    }
    ValidatedQuery query = std::move(std::get<ValidatedQuery>(validation));

    QueryResult result;
    result.columns.reserve(query.outputs.size());
    for (ColumnId column_id : query.outputs) {
        const ColumnMeta& column = query.table->columns[column_id];
        result.columns.push_back(ColumnHeader{column.name, column.type});
    }

    storage::OpenTableResult opened;
    try {
        opened = storage::open_table(storage::OpenTableRequest{query.table->table_id});
    } catch (const std::exception& exception) {
        return make_internal_error(exception.what());
    } catch (...) {
        return make_internal_error("unknown exception while opening table for query");
    }

    if (opened.error.has_value()) {
        if (opened.cursor.has_value()) {
            CursorGuard invalid_result_cursor{*opened.cursor};
            close_ignoring_errors(invalid_result_cursor);
            return make_internal_error("open_table returned both error and cursor");
        }
        return ExecuteResult{internal::map_storage_error(*opened.error)};
    }
    if (!opened.cursor.has_value()) {
        return make_internal_error("open_table returned no cursor on success");
    }

    CursorGuard cursor{*opened.cursor};
    const std::optional<Error> scan_error = scan_records(
        cursor,
        *opened.cursor,
        [&](const storage::Record& record) -> std::optional<Error> {
            const Row& row = record.values;
            if (const std::optional<ExpressionError> row_error =
                    internal::expression::validate_row(*query.table, row);
                row_error.has_value()) {
                return internal::make_error(ErrorKind::kInternal, row_error->message);
            }

            if (query.predicate != nullptr) {
                const std::variant<bool, ExpressionError> predicate =
                    internal::expression::evaluate_predicate(*query.predicate, *query.table, row);
                if (const ExpressionError* error = std::get_if<ExpressionError>(&predicate)) {
                    return internal::make_error(ErrorKind::kInternal, error->message);
                }
                if (!std::get<bool>(predicate)) {
                    return std::nullopt;
                }
            }

            Row projected;
            projected.reserve(query.outputs.size());
            for (ColumnId column_id : query.outputs) {
                projected.push_back(row[column_id]);
            }
            result.rows.push_back(std::move(projected));
            return std::nullopt;
        });
    if (scan_error.has_value()) {
        return ExecuteResult{std::move(*scan_error)};
    }

    if (const std::optional<Error> close_error = cursor.close(); close_error.has_value()) {
        return ExecuteResult{std::move(*close_error)};
    }
    return ExecuteResult{std::move(result)};
}

ExecuteResult Database::Impl::execute_plan_impl(compiler::Plan plan) {
    if (!open) {
        return internal::make_execute_error(ErrorKind::kExecute, "database is not open");
    }

    try {
        return std::visit(
            [this](auto&& typed_plan) -> ExecuteResult {
                using PlanType = std::decay_t<decltype(typed_plan)>;

                if constexpr (std::is_same_v<PlanType, compiler::CreateTablePlan>) {
                    return execute_create_table(typed_plan);
                } else if constexpr (std::is_same_v<PlanType, compiler::InsertPlan>) {
                    return execute_insert(typed_plan);
                } else if constexpr (std::is_same_v<PlanType, compiler::DeletePlan>) {
                    return execute_delete(typed_plan);
                } else {
                    return execute_query(typed_plan);
                }
            },
            std::move(plan.kind));
    } catch (const std::exception& exception) {
        return make_internal_error(exception.what());
    } catch (...) {
        return make_internal_error("unknown exception while executing plan");
    }
}

}  // namespace tinydbms::core
