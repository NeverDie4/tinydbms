#include "analysis_catalog.hpp"

#include "internal.hpp"

#include <limits>
#include <utility>

namespace tinydbms::core::internal {
namespace {

// System Catalog V2 把 0/1 保留给系统表，用户表从 2 开始。
constexpr TableId kFirstUserTableId = 2;

ShadowApplyOutcome rejected(std::string message) {
    ShadowApplyOutcome outcome;
    outcome.result = ShadowApplyResult::kRejected;
    outcome.rejection_message = std::move(message);
    return outcome;
}

}  // namespace

AnalysisCatalog make_analysis_catalog(
    const std::vector<TableMeta>& runtime_tables,
    std::uint64_t runtime_next_table_id) {
    AnalysisCatalog analysis;
    std::vector<TableMeta> copy{runtime_tables};
    analysis.tables = std::move(copy);
    analysis.next_table_id = runtime_next_table_id;
    return analysis;
}

void mirror_runtime_catalog(
    AnalysisCatalog& analysis,
    const std::vector<TableMeta>& runtime_tables,
    std::uint64_t runtime_next_table_id) {
    std::vector<TableMeta> copy{runtime_tables};
    analysis.tables = std::move(copy);
    analysis.next_table_id = runtime_next_table_id;
}

ShadowApplyOutcome apply_supported_ddl(
    AnalysisCatalog& analysis,
    const compiler::Plan& plan) {
    const compiler::CreateTablePlan* create =
        std::get_if<compiler::CreateTablePlan>(&plan.kind);
    if (create == nullptr) {
        return ShadowApplyOutcome{};
    }

    if (analysis.next_table_id > std::numeric_limits<TableId>::max()) {
        return rejected("table id exhausted");
    }
    const TableId candidate = static_cast<TableId>(analysis.next_table_id);
    if (candidate < kFirstUserTableId) {
        return rejected("shadow catalog cannot allocate a reserved table id");
    }

    TableMeta metadata{candidate, create->table_name, create->columns};
    if (const std::optional<Error> metadata_error = validate_table_metadata(metadata);
        metadata_error.has_value()) {
        return rejected(metadata_error->message);
    }
    if (has_duplicate_table(analysis.tables, metadata)) {
        return rejected("table id or table name already exists");
    }

    // 强异常保证：先完成可能抛出的分配，再提交 Catalog 与计数器。
    if (analysis.tables.size() == analysis.tables.max_size()) {
        return rejected("catalog cannot grow further");
    }
    analysis.tables.reserve(analysis.tables.size() + 1U);
    analysis.tables.push_back(std::move(metadata));
    analysis.next_table_id = static_cast<std::uint64_t>(candidate) + 1U;

    ShadowApplyOutcome outcome;
    outcome.result = ShadowApplyResult::kApplied;
    return outcome;
}

}  // namespace tinydbms::core::internal
