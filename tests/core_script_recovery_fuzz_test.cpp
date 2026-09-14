#include "fakes/compiler_fake.hpp"
#include "fakes/result_helpers.hpp"
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
using tinydbms::CompileStage;
using tinydbms::FixIt;
using tinydbms::SourceLocation;
using tinydbms::SourceRange;
using tinydbms::TableId;
using tinydbms::TableMeta;
using tinydbms::Type;
using tinydbms::compiler::CompileError;
using tinydbms::compiler::CompileResult;
using tinydbms::compiler::CreateTablePlan;
using tinydbms::core::Database;
using tinydbms::core::ExecuteScriptRequest;
using tinydbms::core::StatementStatus;
namespace fake_compiler = tinydbms::testing::fake_compiler;
namespace fake_storage = tinydbms::testing::fake_storage;

constexpr std::uint32_t kSeed = 20260928U;
constexpr std::size_t kCaseCount = 1000U;

// 真实 storage 的 V2 bootstrap 总会暴露 0/1 两张系统表，core 的 next_table_id
// 因此从 2 起；影子 Catalog 拒绝把保留 ID 分给用户表，测试必须复现同一运行态。
constexpr std::size_t kSeedTableCount = 2U;

std::vector<TableMeta> seed_catalog() {
    return {
        TableMeta{0U, "tdb_sys_tables", {{"id", Type::kInt}}},
        TableMeta{1U, "tdb_sys_columns", {{"id", Type::kInt}}}};
}

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

CompileResult compile_error(std::string message, bool has_leading_newline) {
    const SourceLocation begin = has_leading_newline
        ? SourceLocation{2, 1, 1}
        : SourceLocation{1, 1, 0};
    const SourceLocation end{
        begin.line,
        begin.column + 1,
        begin.byte_offset + 1};
    return CompileResult{CompileError{
        CompileStage::kSyntax,
        SourceRange{begin, end},
        std::move(message),
        "did you mean 'SELECT'?",
        FixIt{SourceRange{begin, end}, "S"}}};
}

bool run_case(
    const CaseSpec& spec,
    std::size_t case_index,
    CaseOutcome& observed,
    std::string& failure_reason) {
    failure_reason = "unknown";
    fake_storage::reset();
    fake_compiler::reset();
    fake_storage::set_tables(seed_catalog());

    Database database;
    if (database.open({"recovery-fuzz"}).error.has_value()) {
        failure_reason = "open failed";
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
        expected_catalog_sizes.push_back(
            kSeedTableCount + real_create_count + shadow_create_count);

        if (index < spec.first_error_index) {
            compile_results.push_back(valid_create(
                "real_" + std::to_string(case_index) + '_' + std::to_string(index)));
            expected_statuses.push_back(StatementStatus::kExecuted);
            ++real_create_count;
        } else if (index == spec.first_error_index) {
            if (spec.execution_error) {
                // 用与初始 Catalog 同名的 CREATE 触发语句级执行错误：
                // 非法标识符属于 core 契约违反，会升级为 script_error{kInternal}，
                // 不适合作为“普通执行错误”样本。
                compile_results.push_back(valid_create("tdb_sys_tables"));
                expected_statuses.push_back(StatementStatus::kExecutionError);
            } else {
                compile_results.push_back(compile_error(
                    "injected first compile error", index != 0));
                expected_statuses.push_back(StatementStatus::kCompileError);
            }
        } else if (spec.later_compile_errors[index]) {
            compile_results.push_back(compile_error(
                "injected later compile error", index != 0));
            expected_statuses.push_back(StatementStatus::kCompileError);
        } else {
            compile_results.push_back(valid_create(
                "shadow_" + std::to_string(case_index) + '_' + std::to_string(index)));
            expected_statuses.push_back(StatementStatus::kAnalysisOnly);
            ++shadow_create_count;
        }
    }

    fake_compiler::set_compile_results(std::move(compile_results));
    const auto result = database.execute_script(ExecuteScriptRequest{
        script, tinydbms::core::ScriptErrorPolicy::kAnalyzeRemaining});
    if (result.statements.size() != spec.statement_count) {
        failure_reason = "statement count";
        return false;
    }
    if (tinydbms::testing::first_error_index(result) !=
        std::optional<std::size_t>{spec.first_error_index}) {
        const std::optional<std::size_t> actual =
            tinydbms::testing::first_error_index(result);
        failure_reason = "first error index: actual=" +
            (actual.has_value() ? std::to_string(*actual) : std::string{"none"}) +
            " expected=" + std::to_string(spec.first_error_index);
        return false;
    }
    if (tinydbms::testing::executed_count(result) !=
        spec.first_error_index + (spec.execution_error ? 1U : 0U)) {
        failure_reason = "executed count";
        return false;
    }
    if (result.script_error.has_value()) {
        failure_reason = "unexpected script error";
        return false;
    }
    if (fake_compiler::state().compile_calls != spec.statement_count) {
        failure_reason = "compile calls";
        return false;
    }
    if (fake_compiler::state().catalog_sizes != expected_catalog_sizes) {
        std::string actual;
        for (std::size_t size : fake_compiler::state().catalog_sizes) {
            actual += std::to_string(size) + ',';
        }
        std::string expected;
        for (std::size_t size : expected_catalog_sizes) {
            expected += std::to_string(size) + ',';
        }
        failure_reason = "catalog sizes: actual=[" + actual +
            "] expected=[" + expected + ']';
        return false;
    }
    if (fake_storage::state().create_table_calls != real_create_count) {
        failure_reason = "storage create table calls";
        return false;
    }

    observed.statuses.clear();
    std::size_t expected_source_begin = 0;
    for (std::size_t index = 0; index < result.statements.size(); ++index) {
        const auto& statement = result.statements[index];
        const std::size_t expected_source_end = script.find(';', expected_source_begin) + 1U;
        if (statement.statement_index() != index) {
            failure_reason = "statement index";
            return false;
        }
        if (statement.status() != expected_statuses[index]) {
            failure_reason = "statement status";
            return false;
        }
        if (statement.source().begin.byte_offset != expected_source_begin ||
            statement.source().end.byte_offset != expected_source_end ||
            statement.source().end.line != static_cast<int>(index + 1U)) {
            failure_reason = "statement source range";
            return false;
        }
        if (script.substr(
                expected_source_begin, expected_source_end - expected_source_begin) !=
            (index == 0
                ? "statement_" + std::to_string(index) + ';'
                : "\nstatement_" + std::to_string(index) + ';')) {
            failure_reason = "statement text";
            return false;
        }
        expected_source_begin = expected_source_end;
        if (statement.status() == StatementStatus::kAnalysisOnly) {
            if (statement.outcome().has_value()) {
                failure_reason = "analysis-only carries outcome";
                return false;
            }
        } else if (!statement.outcome().has_value()) {
            failure_reason = "missing outcome";
            return false;
        }
        if (statement.status() == StatementStatus::kCompileError) {
            const auto* error =
                std::get_if<tinydbms::core::Error>(&statement.outcome()->outcome);
            if (error == nullptr || !error->source.has_value() ||
                error->source->begin.line != static_cast<int>(index + 1U) ||
                !error->fix_it.has_value() ||
                error->fix_it->range.begin.line != static_cast<int>(index + 1U)) {
                failure_reason = "compile error diagnostic";
                return false;
            }
        }
        observed.statuses.push_back(statement.status());
    }

    fake_compiler::reset();
    std::deque<CompileResult> probe_results;
    probe_results.push_back(compile_error("shadow leak probe", false));
    fake_compiler::set_compile_results(std::move(probe_results));
    (void)database.execute_script({"probe;"});
    if (fake_compiler::state().catalog_sizes !=
        std::vector<std::size_t>{kSeedTableCount + real_create_count}) {
        failure_reason = "shadow catalog leak";
        return false;
    }

    fake_compiler::reset();
    std::deque<CompileResult> next_id_results;
    next_id_results.push_back(valid_create("after_recovery"));
    fake_compiler::set_compile_results(std::move(next_id_results));
    const auto next_id_result = database.execute_script({"create after recovery;"});
    if (next_id_result.statements.size() != 1U ||
        next_id_result.statements[0].status() != StatementStatus::kExecuted ||
        !fake_storage::state().last_create_request.has_value() ||
        fake_storage::state().last_create_request->table_id !=
            static_cast<TableId>(kSeedTableCount + real_create_count)) {
        failure_reason = "next table id";
        return false;
    }

    observed.executed_count = tinydbms::testing::executed_count(result);
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
        std::string failure_reason{"unknown"};
        if (!run_case(spec, case_index, first, failure_reason)) {
            std::cerr << "recovery fuzz failure: seed=" << kSeed
                      << " case=" << case_index << " reason=" << failure_reason << '\n';
            return 1;
        }
        if (!run_case(spec, case_index, second, failure_reason)) {
            std::cerr << "recovery fuzz failure: seed=" << kSeed
                      << " case=" << case_index << " repeat reason=" << failure_reason << '\n';
            return 1;
        }
        if (first.statuses != second.statuses ||
            first.executed_count != second.executed_count ||
            first.real_create_count != second.real_create_count) {
            std::cerr << "recovery fuzz failure: seed=" << kSeed
                      << " case=" << case_index << " reason=not reproducible" << '\n';
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
