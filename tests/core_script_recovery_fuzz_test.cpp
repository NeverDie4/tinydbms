#include "fakes/compiler_fake.hpp"
#include "fakes/storage_fake.hpp"
#include "tinydbms/core.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

using tinydbms::ColumnMeta;
using tinydbms::SourceLocation;
using tinydbms::SourceRange;
using tinydbms::TableId;
using tinydbms::Type;
using tinydbms::compiler::CompileError;
using tinydbms::compiler::CompileErrorKind;
using tinydbms::compiler::CompileResult;
using tinydbms::compiler::CreateTablePlan;
using tinydbms::compiler::FixIt;
using tinydbms::core::Database;
using tinydbms::core::ExecuteScriptRequest;
using tinydbms::core::StatementStatus;
namespace fake_compiler = tinydbms::testing::fake_compiler;
namespace fake_storage = tinydbms::testing::fake_storage;

constexpr std::uint32_t kSeed = 20260928U;
constexpr std::size_t kCaseCount = 1000U;

struct CaseSpec {
    std::size_t statement_count;
    std::size_t first_error_index;
    bool execution_error;
    std::vector<bool> later_compile_errors;
};

struct CaseOutcome {
    std::vector<StatementStatus> statuses;
    std::size_t executed_count;
    std::size_t real_create_count;
};

CompileResult valid_create(std::string name) {
    return CompileResult{CreateTablePlan{
        std::move(name),
        std::vector<ColumnMeta>{{"id", Type::kInt}}}};
}

CompileResult compile_error(std::string message) {
    return CompileResult{CompileError{
        CompileErrorKind::kSyntax,
        SourceLocation{1, 1},
        std::move(message),
        "did you mean 'SELECT'?",
        FixIt{SourceRange{{1, 1}, {1, 2}}, "S"}}};
}

bool run_case(const CaseSpec& spec, std::size_t case_index, CaseOutcome& observed) {
    fake_storage::reset();
    fake_compiler::reset();

    Database database;
    if (database.open({"recovery-fuzz"}).error.has_value()) {
        return false;
    }

    std::deque<CompileResult> compile_results;
    std::vector<StatementStatus> expected_statuses;
    std::vector<std::size_t> expected_catalog_sizes;
    std::string script;
    std::size_t real_create_count = 0;
    std::size_t shadow_create_count = 0;

    for (std::size_t index = 0; index < spec.statement_count; ++index) {
        if (!script.empty()) {
            script.push_back('\n');
        }
        script += "statement_" + std::to_string(index) + ';';
        expected_catalog_sizes.push_back(real_create_count + shadow_create_count);

        if (index < spec.first_error_index) {
            compile_results.push_back(valid_create(
                "real_" + std::to_string(case_index) + '_' + std::to_string(index)));
            expected_statuses.push_back(StatementStatus::kExecuted);
            ++real_create_count;
        } else if (index == spec.first_error_index) {
            if (spec.execution_error) {
                compile_results.push_back(valid_create("invalid-name"));
                expected_statuses.push_back(StatementStatus::kExecutionError);
            } else {
                compile_results.push_back(compile_error("injected first compile error"));
                expected_statuses.push_back(StatementStatus::kCompileError);
            }
        } else if (spec.later_compile_errors[index]) {
            compile_results.push_back(compile_error("injected later compile error"));
            expected_statuses.push_back(StatementStatus::kCompileError);
        } else {
            compile_results.push_back(valid_create(
                "shadow_" + std::to_string(case_index) + '_' + std::to_string(index)));
            expected_statuses.push_back(StatementStatus::kAnalysisOnly);
            ++shadow_create_count;
        }
    }

    fake_compiler::set_compile_results(std::move(compile_results));
    const auto result = database.execute_script(ExecuteScriptRequest{script});
    if (result.statements.size() != spec.statement_count ||
        result.first_error_index != spec.first_error_index ||
        result.executed_count != spec.first_error_index + (spec.execution_error ? 1U : 0U) ||
        result.script_error.has_value() ||
        fake_compiler::state().compile_calls != spec.statement_count ||
        fake_compiler::state().catalog_sizes != expected_catalog_sizes ||
        fake_storage::state().create_table_calls != real_create_count) {
        return false;
    }

    observed.statuses.clear();
    for (std::size_t index = 0; index < result.statements.size(); ++index) {
        const auto& statement = result.statements[index];
        if (statement.statement_index != index || statement.status != expected_statuses[index] ||
            statement.source_range.begin.line != static_cast<int>(index + 1U)) {
            return false;
        }
        if (statement.status == StatementStatus::kAnalysisOnly) {
            if (statement.outcome.has_value()) {
                return false;
            }
        } else if (!statement.outcome.has_value()) {
            return false;
        }
        if (statement.status == StatementStatus::kCompileError) {
            const auto* error = std::get_if<tinydbms::core::Error>(&statement.outcome->outcome);
            if (error == nullptr || !error->location.has_value() ||
                error->location->line != static_cast<int>(index + 1U) ||
                !error->fix_it.has_value() ||
                error->fix_it->range.begin.line != static_cast<int>(index + 1U)) {
                return false;
            }
        }
        observed.statuses.push_back(statement.status);
    }

    fake_compiler::reset();
    std::deque<CompileResult> probe_results;
    probe_results.push_back(compile_error("shadow leak probe"));
    fake_compiler::set_compile_results(std::move(probe_results));
    (void)database.execute_script({"probe;"});
    if (fake_compiler::state().catalog_sizes != std::vector<std::size_t>{real_create_count}) {
        return false;
    }

    fake_compiler::reset();
    std::deque<CompileResult> next_id_results;
    next_id_results.push_back(valid_create("after_recovery"));
    fake_compiler::set_compile_results(std::move(next_id_results));
    const auto next_id_result = database.execute_script({"create after recovery;"});
    if (next_id_result.statements.size() != 1U ||
        next_id_result.statements[0].status != StatementStatus::kExecuted ||
        !fake_storage::state().last_create_request.has_value() ||
        fake_storage::state().last_create_request->table_id !=
            static_cast<TableId>(real_create_count)) {
        return false;
    }

    observed.executed_count = result.executed_count;
    observed.real_create_count = real_create_count;
    return !database.close().error.has_value();
}

}  // namespace

int main() {
    std::mt19937 random{kSeed};
    std::size_t compile_error_cases = 0;
    std::size_t execution_error_cases = 0;
    std::size_t shadow_create_statements = 0;

    for (std::size_t case_index = 0; case_index < kCaseCount; ++case_index) {
        CaseSpec spec;
        spec.statement_count = 3U + static_cast<std::size_t>(random() % 8U);
        spec.first_error_index = static_cast<std::size_t>(
            random() % static_cast<std::uint32_t>(spec.statement_count - 1U));
        spec.execution_error = (random() % 2U) == 0U;
        spec.later_compile_errors.assign(spec.statement_count, false);
        for (std::size_t index = spec.first_error_index + 1U;
             index < spec.statement_count;
             ++index) {
            spec.later_compile_errors[index] = (random() % 3U) == 0U;
            if (!spec.later_compile_errors[index]) {
                ++shadow_create_statements;
            }
        }

        CaseOutcome first;
        CaseOutcome second;
        if (!run_case(spec, case_index, first) || !run_case(spec, case_index, second) ||
            first.statuses != second.statuses ||
            first.executed_count != second.executed_count ||
            first.real_create_count != second.real_create_count) {
            std::cerr << "recovery fuzz failure: seed=" << kSeed
                      << " case=" << case_index << '\n';
            return 1;
        }
        if (spec.execution_error) {
            ++execution_error_cases;
        } else {
            ++compile_error_cases;
        }
    }

    std::cout << "core script recovery fuzz passed: seed=" << kSeed
              << " cases=" << kCaseCount
              << " compile-error=" << compile_error_cases
              << " execution-error=" << execution_error_cases
              << " shadow-create=" << shadow_create_statements << '\n';
    return 0;
}
