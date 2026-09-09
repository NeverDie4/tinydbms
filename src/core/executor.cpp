#include "internal.hpp"

#include "tinydbms/storage.hpp"

#include <exception>
#include <limits>
#include <type_traits>
#include <utility>

namespace tinydbms::core {

ExecuteResult Database::Impl::execute_plan_impl(compiler::Plan plan) {
    return std::visit(
        [this](auto&& typed_plan) -> ExecuteResult {
            using PlanType = std::decay_t<decltype(typed_plan)>;

            if constexpr (std::is_same_v<PlanType, compiler::CreateTablePlan>) {
                if (!open) {
                    return internal::make_execute_error(ErrorKind::kExecute, "database is not open");
                }
                if (next_table_id > std::numeric_limits<TableId>::max()) {
                    return internal::make_execute_error(ErrorKind::kExecute, "table id exhausted");
                }

                const TableId candidate_id = static_cast<TableId>(next_table_id);
                TableMeta metadata{candidate_id, typed_plan.table_name, typed_plan.columns};

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
                        typed_plan.table_name,
                        typed_plan.columns});
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
            } else {
                return internal::make_execute_error(
                    ErrorKind::kInternal,
                    "this plan type is not implemented in the current implementation slice");
            }
        },
        std::move(plan.kind));
}

}  // namespace tinydbms::core
