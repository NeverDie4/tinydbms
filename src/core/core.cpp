#include "tinydbms/core.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

#include "tinydbms/compiler.hpp"
#include "tinydbms/storage.hpp"

namespace tinydbms::core {
namespace {
enum class Lifecycle { kClosed, kOpen, kCleanupPending };
void* storage_owner = nullptr;

Error execute_error(std::string message) { return {ErrorKind::kExecute, std::nullopt, std::move(message)}; }
Error storage_error(const storage::StorageError& source) { return {ErrorKind::kStorage, std::nullopt, source.message}; }
Error compile_error(const compiler::CompileError& source, SourceLocation start) {
    return {ErrorKind::kCompile,
            SourceLocation{start.line + source.location.line - 1,
                           source.location.line == 1 ? start.column + source.location.column - 1 : source.location.column},
            source.message};
}

struct Evaluated { std::variant<Value, bool> value; };
std::variant<Evaluated, Error> evaluate(const compiler::Expr& expression, const Row& row) {
    if (const auto* column = std::get_if<compiler::ColumnRef>(&expression.kind)) {
        if (column->column_id >= row.size()) return execute_error("plan column id is outside the record layout");
        return Evaluated{row[column->column_id]};
    }
    if (const auto* literal = std::get_if<compiler::Literal>(&expression.kind)) return Evaluated{literal->value};
    if (const auto* unary = std::get_if<compiler::Unary>(&expression.kind)) {
        const auto operand = evaluate(*unary->operand, row);
        if (const auto* error = std::get_if<Error>(&operand)) return *error;
        const auto* boolean = std::get_if<bool>(&std::get<Evaluated>(operand).value);
        return boolean == nullptr ? std::variant<Evaluated, Error>{execute_error("NOT operand is not BOOL")} : Evaluated{!*boolean};
    }
    const auto& binary = std::get<compiler::Binary>(expression.kind);
    const auto lhs = evaluate(*binary.lhs, row);
    if (const auto* error = std::get_if<Error>(&lhs)) return *error;
    const auto rhs = evaluate(*binary.rhs, row);
    if (const auto* error = std::get_if<Error>(&rhs)) return *error;
    const auto& left = std::get<Evaluated>(lhs).value;
    const auto& right = std::get<Evaluated>(rhs).value;
    if (const auto* logic = std::get_if<compiler::LogicOp>(&binary.op)) {
        const auto* left_bool = std::get_if<bool>(&left);
        const auto* right_bool = std::get_if<bool>(&right);
        if (left_bool == nullptr || right_bool == nullptr) return execute_error("logical operator operands are not BOOL");
        return Evaluated{*logic == compiler::LogicOp::kAnd ? (*left_bool && *right_bool) : (*left_bool || *right_bool)};
    }
    const auto* left_value = std::get_if<Value>(&left);
    const auto* right_value = std::get_if<Value>(&right);
    if (left_value == nullptr || right_value == nullptr || left_value->data.index() != right_value->data.index()) {
        return execute_error("comparison operands have incompatible types");
    }
    const int compare = std::visit([](const auto& a, const auto& b) -> int {
        if constexpr (!std::is_same_v<std::decay_t<decltype(a)>, std::decay_t<decltype(b)>>) return 0;
        else return a < b ? -1 : a > b ? 1 : 0;
    }, left_value->data, right_value->data);
    switch (std::get<compiler::CmpOp>(binary.op)) {
        case compiler::CmpOp::kEq: return Evaluated{compare == 0}; case compiler::CmpOp::kNe: return Evaluated{compare != 0};
        case compiler::CmpOp::kLt: return Evaluated{compare < 0}; case compiler::CmpOp::kLe: return Evaluated{compare <= 0};
        case compiler::CmpOp::kGt: return Evaluated{compare > 0}; case compiler::CmpOp::kGe: return Evaluated{compare >= 0};
    }
    return execute_error("unknown comparison operator");
}
std::variant<bool, Error> evaluate_predicate(const compiler::Expr& expression, const Row& row) {
    const auto result = evaluate(expression, row);
    if (const auto* error = std::get_if<Error>(&result)) return *error;
    const auto* boolean = std::get_if<bool>(&std::get<Evaluated>(result).value);
    return boolean == nullptr ? std::variant<bool, Error>{execute_error("predicate does not evaluate to BOOL")} : *boolean;
}
}  // namespace

struct Database::Impl {
    Lifecycle lifecycle{Lifecycle::kClosed}; std::string data_dir; std::vector<TableMeta> catalog;
    std::optional<TableId> next_table_id{TableId{0}};
    bool owns_storage() const noexcept { return storage_owner == this; }
    void clear_local() { data_dir.clear(); catalog.clear(); next_table_id = TableId{0}; }
    std::optional<Error> refresh_catalog() {
        const auto listed = storage::list_tables({});
        if (listed.error) return storage_error(*listed.error);
        catalog = listed.tables;
        if (catalog.empty()) { next_table_id = TableId{0}; return std::nullopt; }
        std::uint64_t maximum = 0;
        for (const auto& table : catalog) maximum = std::max(maximum, static_cast<std::uint64_t>(table.table_id));
        if (maximum == std::numeric_limits<TableId>::max()) next_table_id.reset();
        else next_table_id = static_cast<TableId>(maximum + 1U);
        return std::nullopt;
    }
    std::variant<CommandResult, Error> execute_create(const compiler::CreateTablePlan& plan) {
        if (!next_table_id) return execute_error("table id space is exhausted");
        const auto created = storage::create_table({*next_table_id, plan.table_name, plan.columns});
        if (created.error) return CommandResult{0, storage_error(*created.error)};
        if (const auto error = refresh_catalog()) { lifecycle = Lifecycle::kCleanupPending; return CommandResult{0, *error}; }
        return CommandResult{0, std::nullopt};
    }
    std::variant<CommandResult, Error> execute_insert(const compiler::InsertPlan& plan) {
        const auto table = std::find_if(catalog.begin(), catalog.end(), [&plan](const auto& meta) { return meta.table_id == plan.table_id; });
        if (table == catalog.end()) return execute_error("plan references a table outside the catalog");
        std::vector<std::vector<Value>> rows; rows.reserve(plan.rows.size());
        for (const auto& source : plan.rows) {
            if (source.size() != table->columns.size()) return execute_error("plan row has an invalid column count");
            if (plan.columns.empty()) { rows.push_back(source); continue; }
            if (plan.columns.size() != table->columns.size()) return execute_error("plan column list is incomplete");
            std::vector<Value> reordered(table->columns.size()); std::vector<bool> assigned(table->columns.size());
            for (std::size_t index = 0; index < plan.columns.size(); ++index) {
                const auto column = plan.columns[index];
                if (column >= reordered.size() || assigned[column]) return execute_error("plan column list is invalid");
                reordered[column] = source[index]; assigned[column] = true;
            }
            if (std::find(assigned.begin(), assigned.end(), false) != assigned.end()) return execute_error("plan column list is incomplete");
            rows.push_back(std::move(reordered));
        }
        const auto inserted = storage::insert({plan.table_id, std::move(rows)});
        return inserted.error ? std::variant<CommandResult, Error>{CommandResult{inserted.rids.size(), storage_error(*inserted.error)}}
                              : CommandResult{inserted.rids.size(), std::nullopt};
    }
    struct QueryShape { TableId table_id; const std::vector<ColumnId>* outputs; const compiler::Expr* predicate; };
    std::variant<QueryShape, Error> query_shape(const compiler::QueryPlan& query) const {
        if (!query.root) return execute_error("query plan has no root");
        const auto* project = std::get_if<compiler::ProjectNode>(&query.root->kind);
        if (project == nullptr || !project->child) return execute_error("query plan lacks a Project node");
        const compiler::PlanNode* node = project->child.get(); const compiler::Expr* predicate = nullptr;
        if (const auto* filter = std::get_if<compiler::FilterNode>(&node->kind)) { if (!filter->child) return execute_error("filter plan has no child"); predicate = &filter->predicate; node = filter->child.get(); }
        const auto* scan = std::get_if<compiler::SeqScanNode>(&node->kind);
        return scan == nullptr ? std::variant<QueryShape, Error>{execute_error("query plan lacks a SeqScan node")} : QueryShape{scan->table_id, &project->outputs, predicate};
    }
    std::variant<QueryResult, Error> execute_query(const compiler::QueryPlan& query) {
        const auto shape_result = query_shape(query); if (const auto* error = std::get_if<Error>(&shape_result)) return *error;
        const auto& shape = std::get<QueryShape>(shape_result);
        const auto table = std::find_if(catalog.begin(), catalog.end(), [&shape](const auto& meta) { return meta.table_id == shape.table_id; });
        if (table == catalog.end()) return execute_error("query plan references a table outside the catalog");
        QueryResult output; output.columns.reserve(shape.outputs->size());
        for (const auto column : *shape.outputs) { if (column >= table->columns.size()) return execute_error("projection column id is outside the schema"); output.columns.push_back({table->columns[column].name, table->columns[column].type}); }
        const auto opened = storage::open_table({shape.table_id});
        if (opened.error) return storage_error(*opened.error);
        if (!opened.cursor) return execute_error("storage opened a table without a cursor");
        const auto cursor = *opened.cursor; std::optional<Error> failure;
        for (;;) { const auto next = storage::scan_next({cursor});
            if (next.error) { failure = storage_error(*next.error); break; } if (!next.record) break;
            if (next.record->values.size() != table->columns.size()) { failure = execute_error("storage record does not match table schema"); break; }
            if (shape.predicate) { const auto accepted = evaluate_predicate(*shape.predicate, next.record->values); if (const auto* error = std::get_if<Error>(&accepted)) { failure = *error; break; } if (!std::get<bool>(accepted)) continue; }
            Row row; row.reserve(shape.outputs->size()); for (const auto column : *shape.outputs) row.push_back(next.record->values[column]); output.rows.push_back(std::move(row)); }
        const auto closed = storage::close_cursor({cursor}); if (failure) return *failure; if (closed.error) return storage_error(*closed.error); return output;
    }
    std::variant<CommandResult, Error> execute_delete(const compiler::DeletePlan& plan) {
        const auto table = std::find_if(catalog.begin(), catalog.end(), [&plan](const auto& meta) { return meta.table_id == plan.table_id; });
        if (table == catalog.end()) return execute_error("delete plan references a table outside the catalog");
        const auto opened = storage::open_table({plan.table_id}); if (opened.error) return storage_error(*opened.error); if (!opened.cursor) return execute_error("storage opened a table without a cursor");
        const auto cursor = *opened.cursor; std::vector<storage::RecordId> matches; std::optional<Error> failure;
        for (;;) { const auto next = storage::scan_next({cursor});
            if (next.error) { failure = storage_error(*next.error); break; } if (!next.record) break;
            if (next.record->values.size() != table->columns.size()) { failure = execute_error("storage record does not match table schema"); break; }
            if (plan.predicate) { const auto accepted = evaluate_predicate(*plan.predicate, next.record->values); if (const auto* error = std::get_if<Error>(&accepted)) { failure = *error; break; } if (!std::get<bool>(accepted)) continue; }
            matches.push_back(next.record->rid); }
        const auto closed = storage::close_cursor({cursor}); if (failure) return *failure; if (closed.error) return storage_error(*closed.error);
        if (matches.empty()) return CommandResult{0, std::nullopt};
        const auto deleted = storage::delete_records({plan.table_id, std::move(matches)});
        return deleted.error ? std::variant<CommandResult, Error>{CommandResult{deleted.deleted_count, storage_error(*deleted.error)}} : CommandResult{deleted.deleted_count, std::nullopt};
    }
    ExecuteResult execute_plan(const compiler::Plan& plan) {
        if (const auto* create = std::get_if<compiler::CreateTablePlan>(&plan.kind)) { const auto result = execute_create(*create); return std::holds_alternative<Error>(result) ? ExecuteResult{std::get<Error>(result)} : ExecuteResult{std::get<CommandResult>(result)}; }
        if (const auto* insert = std::get_if<compiler::InsertPlan>(&plan.kind)) { const auto result = execute_insert(*insert); return std::holds_alternative<Error>(result) ? ExecuteResult{std::get<Error>(result)} : ExecuteResult{std::get<CommandResult>(result)}; }
        if (const auto* deletion = std::get_if<compiler::DeletePlan>(&plan.kind)) { const auto result = execute_delete(*deletion); return std::holds_alternative<Error>(result) ? ExecuteResult{std::get<Error>(result)} : ExecuteResult{std::get<CommandResult>(result)}; }
        const auto result = execute_query(std::get<compiler::QueryPlan>(plan.kind)); return std::holds_alternative<Error>(result) ? ExecuteResult{std::get<Error>(result)} : ExecuteResult{std::get<QueryResult>(result)};
    }
};

Database::Database() : impl_(std::make_unique<Impl>()) {}
Database::~Database() noexcept { if (impl_ && impl_->owns_storage()) (void)close(); }
Database::Database(Database&& other) noexcept : impl_(std::move(other.impl_)) {}
Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        if (impl_ && impl_->owns_storage()) (void)close();
        const bool other_owns_storage = storage_owner == other.impl_.get();
        impl_ = std::move(other.impl_);
        if (other_owns_storage) storage_owner = impl_.get();
    }
    return *this;
}

OpenDatabaseResult Database::open(const OpenDatabaseRequest& request) {
    if (!impl_) return {execute_error("database object was moved from")};
    if (impl_->lifecycle != Lifecycle::kClosed) return {execute_error("database is already open or pending cleanup")};
    if (storage_owner != nullptr) return {execute_error("another Database owns Storage")};
    const std::string data_dir = request.data_dir.empty() ? "./tinydbms-data" : request.data_dir; const auto opened = storage::open_storage({data_dir}); if (opened.error) return {storage_error(*opened.error)};
    storage_owner = impl_.get(); impl_->lifecycle = Lifecycle::kOpen; impl_->data_dir = data_dir;
    if (const auto error = impl_->refresh_catalog()) { const auto closed = storage::close_storage({}); if (!closed.error) { storage_owner = nullptr; impl_->lifecycle = Lifecycle::kClosed; impl_->clear_local(); } else impl_->lifecycle = Lifecycle::kCleanupPending; return {*error}; }
    return {};
}
CloseDatabaseResult Database::close() {
    if (!impl_) return {execute_error("database object was moved from")};
    if (impl_->lifecycle == Lifecycle::kClosed) return {};
    if (!impl_->owns_storage()) return {execute_error("database does not own Storage")};
    const auto closed = storage::close_storage({}); if (closed.error) { impl_->lifecycle = Lifecycle::kCleanupPending; return {storage_error(*closed.error)}; }
    storage_owner = nullptr; impl_->lifecycle = Lifecycle::kClosed; impl_->clear_local(); return {};
}
ExecuteScriptResult Database::execute_script(const ExecuteScriptRequest& request) {
    ExecuteScriptResult output; if (!impl_ || impl_->lifecycle != Lifecycle::kOpen || !impl_->owns_storage()) { output.outcomes.push_back(ExecuteResult{execute_error("database is not open")}); return output; }
    std::vector<compiler::SplitStatement> statements; try { statements = compiler::split_statements(request.text); } catch (const std::length_error& error) { output.outcomes.push_back(ExecuteResult{Error{ErrorKind::kCompile, SourceLocation{1,1}, error.what()}}); return output; }
    for (const auto& statement : statements) { auto compiled = compiler::compile({statement.sql, {impl_->catalog}}); if (const auto* error = std::get_if<compiler::CompileError>(&compiled.outcome)) { output.outcomes.push_back(ExecuteResult{compile_error(*error, statement.start)}); break; }
        compiler::Plan plan = std::get<compiler::Plan>(std::move(compiled.outcome)); ExecuteResult result = impl_->execute_plan(plan);
        const bool failed = std::holds_alternative<Error>(result.outcome) || (std::holds_alternative<CommandResult>(result.outcome) && std::get<CommandResult>(result.outcome).error.has_value()); output.outcomes.push_back(std::move(result)); if (failed || impl_->lifecycle != Lifecycle::kOpen) break; }
    return output;
}
}  // namespace tinydbms::core
