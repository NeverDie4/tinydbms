#include "fakes/compiler_fake.hpp"
#include "fakes/storage_fake.hpp"
#include "tinydbms/core.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
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
using tinydbms::Value;
using tinydbms::compiler::CompileError;
using tinydbms::compiler::CompileResult;
using tinydbms::compiler::CreateTablePlan;
using tinydbms::compiler::InsertPlan;
using tinydbms::compiler::Plan;
using tinydbms::compiler::PlanNode;
using tinydbms::compiler::QueryPlan;
using tinydbms::compiler::SeqScanNode;
using tinydbms::compiler::SplitStatement;
using tinydbms::core::Database;
using tinydbms::core::Error;
using tinydbms::core::ErrorKind;
using tinydbms::core::ExecuteScriptRequest;
using tinydbms::core::ExecuteScriptResult;
using tinydbms::core::OpenDatabaseRequest;
using tinydbms::core::ScriptErrorPolicy;
using tinydbms::core::StatementStatus;
namespace fake = tinydbms::testing::fake_storage;
namespace fake_compiler = tinydbms::testing::fake_compiler;

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " #condition \
                      << '\n';                                                              \
            return false;                                                                    \
        }                                                                                    \
    } while (false)

Plan create_table_plan(
    std::string name,
    std::vector<ColumnMeta> columns = std::vector<ColumnMeta>{{"id", Type::kInt}}) {
    return Plan{CreateTablePlan{std::move(name), std::move(columns)}};
}

Plan scan_plan(TableId table_id = 1) {
    return Plan{QueryPlan{std::make_unique<PlanNode>(SeqScanNode{table_id})}};
}

Plan insert_plan(TableId table_id, std::vector<std::vector<Value>> rows) {
    return Plan{InsertPlan{table_id, {}, std::move(rows)}};
}

bool open_database(Database& database) {
    return !database.open(OpenDatabaseRequest{"policy-test-data"}).error.has_value();
}

bool is_empty_insertion_point(const SourceRange& range) {
    return range.begin.line == 1 && range.begin.column == 1 &&
        range.end.line == 1 && range.end.column == 1 &&
        range.begin_offset == 0 && range.end_offset == 0;
}

bool has_script_error(const ExecuteScriptResult& script, ErrorKind kind) {
    return script.script_error.has_value() && script.script_error->kind == kind;
}

StatementStatus status_at(const ExecuteScriptResult& script, std::size_t index) {
    return script.statements.at(index).status();
}

const Error& error_at(const ExecuteScriptResult& script, std::size_t index) {
    return std::get<Error>(script.statements.at(index).outcome()->outcome);
}

bool test_stop_policy_skips_remaining_statements() {
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({TableMeta{1, "users", {{"id", Type::kInt}}}});

    Database database;
    CHECK(open_database(database));

    std::deque<CompileResult> results;
    results.emplace_back(create_table_plan("events"));
    results.emplace_back(fake_compiler::make_compile_error(
        CompileStage::kSyntax,
        "select from;",
        0,
        6,
        "injected syntax error"));
    results.emplace_back(create_table_plan("never"));
    fake_compiler::set_compile_results(std::move(results));

    const auto script = database.execute_script(ExecuteScriptRequest{
        "create table events (id int);\nselect from;\ncreate table never (id int);",
        ScriptErrorPolicy::kStopOnFirstError});

    CHECK(!script.script_error.has_value());
    CHECK(script.statements.size() == 3);
    CHECK(status_at(script, 0) == StatementStatus::kExecuted);
    CHECK(status_at(script, 1) == StatementStatus::kCompileError);
    CHECK(status_at(script, 2) == StatementStatus::kSkippedExecution);
    CHECK(script.statements[0].statement_index() == 0);
    CHECK(script.statements[1].statement_index() == 1);
    CHECK(script.statements[2].statement_index() == 2);
    CHECK(script.statements[1].source().begin.line == 2);
    CHECK(script.statements[2].source().begin.line == 3);

    const Error& compile_error =
        error_at(script, 1);
    CHECK(compile_error.kind == ErrorKind::kCompile);
    CHECK(compile_error.source.has_value());
    CHECK(compile_error.source->begin.line == 2);
    CHECK(compile_error.source->begin.column == 1);
    CHECK(compile_error.source->end.line == 2);
    CHECK(compile_error.source->end.column == 7);

    // stop 策略：不编译首错之后的语句，也不发生任何 Storage 调用。
    CHECK(fake_compiler::state().compile_calls == 2);
    CHECK(fake::state().create_table_calls == 1);
    CHECK(fake::state().tables.size() == 2);
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_analyze_policy_continues_with_shadow_catalog() {
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({TableMeta{1, "users", {{"id", Type::kInt}}}});

    Database database;
    CHECK(open_database(database));

    std::deque<CompileResult> results;
    results.emplace_back(create_table_plan("orders"));
    results.emplace_back(fake_compiler::make_compile_error(
        CompileStage::kSemantic,
        "select missing from users;",
        7,
        14,
        "unknown column \"missing\""));
    results.emplace_back(create_table_plan("events"));
    results.emplace_back(scan_plan(3));
    fake_compiler::set_compile_results(std::move(results));

    const auto script = database.execute_script(ExecuteScriptRequest{
        "create table orders (id int);\n"
        "select missing from users;\n"
        "create table events (id int);\n"
        "select * from events;",
        ScriptErrorPolicy::kAnalyzeRemaining});

    CHECK(!script.script_error.has_value());
    CHECK(script.statements.size() == 4);
    CHECK(status_at(script, 0) == StatementStatus::kExecuted);
    CHECK(status_at(script, 1) == StatementStatus::kCompileError);
    CHECK(status_at(script, 2) == StatementStatus::kAnalysisOnly);
    CHECK(status_at(script, 3) == StatementStatus::kAnalysisOnly);

    // 编译期看到的 Catalog 大小：运行态 1 张，执行后 2 张，影子新增后 3 张。
    CHECK((fake_compiler::state().catalog_sizes ==
           std::vector<std::size_t>{1, 2, 2, 3}));
    CHECK(fake::state().create_table_calls == 1);
    CHECK(fake::state().tables.size() == 2);
    CHECK(fake::state().last_create_request.has_value());
    CHECK(fake::state().last_create_request->table_id == 2);

    // 影子分配不消耗运行时 next_table_id：下一条真实 CREATE 复用候选 3。
    fake_compiler::reset();
    std::deque<CompileResult> later_results;
    later_results.emplace_back(create_table_plan("later"));
    fake_compiler::set_compile_results(std::move(later_results));
    const auto later_script = database.execute_script(
        ExecuteScriptRequest{"create table later (id int);"});
    CHECK(status_at(later_script, 0) == StatementStatus::kExecuted);
    CHECK(fake::state().last_create_request->table_id == 3);
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_analyze_reports_shadow_rejection_and_analysis_only() {
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({TableMeta{1, "users", {{"id", Type::kInt}}}});

    Database database;
    CHECK(open_database(database));

    std::deque<CompileResult> results;
    results.emplace_back(fake_compiler::make_compile_error(
        CompileStage::kSemantic,
        "select missing from users;",
        7,
        14,
        "unknown column \"missing\""));
    results.emplace_back(create_table_plan("users"));
    results.emplace_back(create_table_plan("bad", {}));
    results.emplace_back(scan_plan(1));
    fake_compiler::set_compile_results(std::move(results));

    const auto script = database.execute_script(ExecuteScriptRequest{
        "select missing from users;\n"
        "create table users (id int);\n"
        "create table bad ();\n"
        "select * from users;",
        ScriptErrorPolicy::kAnalyzeRemaining});

    CHECK(!script.script_error.has_value());
    CHECK(script.statements.size() == 4);
    CHECK(status_at(script, 0) == StatementStatus::kCompileError);
    CHECK(status_at(script, 1) == StatementStatus::kAnalysisError);
    CHECK(status_at(script, 2) == StatementStatus::kAnalysisError);
    // 非 DDL 不改变影子 Catalog，必须记 kAnalysisOnly 而不是 kAnalysisError。
    CHECK(status_at(script, 3) == StatementStatus::kAnalysisOnly);
    CHECK(error_at(script, 1).kind == ErrorKind::kAnalysis);
    CHECK(error_at(script, 2).kind == ErrorKind::kAnalysis);
    CHECK(fake::state().create_table_calls == 0);
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_analyze_shadow_rejection_does_not_consume_candidate_id() {
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({TableMeta{1, "users", {{"id", Type::kInt}}}});

    Database database;
    CHECK(open_database(database));

    // 第一条编译错关闭执行门；第二条影子 CREATE 因重名被拒绝，不得消耗候选 ID；
    // 第三条影子 CREATE 必须仍拿到运行时 next_table_id（2），第四条编译时可见该表。
    std::deque<CompileResult> results;
    results.emplace_back(fake_compiler::make_compile_error(
        CompileStage::kSemantic,
        "select missing from users;",
        7,
        14,
        "unknown column \"missing\""));
    results.emplace_back(create_table_plan("users"));
    results.emplace_back(create_table_plan("events"));
    results.emplace_back(scan_plan(2));
    fake_compiler::set_compile_results(std::move(results));

    const auto script = database.execute_script(ExecuteScriptRequest{
        "select missing from users;\n"
        "create table users (id int);\n"
        "create table events (id int);\n"
        "select * from events;",
        ScriptErrorPolicy::kAnalyzeRemaining});

    CHECK(!script.script_error.has_value());
    CHECK(script.statements.size() == 4);
    CHECK(status_at(script, 1) == StatementStatus::kAnalysisError);
    CHECK(status_at(script, 2) == StatementStatus::kAnalysisOnly);
    CHECK(status_at(script, 3) == StatementStatus::kAnalysisOnly);
    CHECK(fake_compiler::state().catalog_table_ids.size() == 4);
    // 影子 events 拿到 ID 2：被拒绝的 users 没有消耗候选 ID。
    CHECK((fake_compiler::state().catalog_table_ids[3] ==
           std::vector<TableId>{1, 2}));
    // 影子分配不产生任何 Storage 副作用。
    CHECK(fake::state().create_table_calls == 0);
    CHECK(fake::state().tables.size() == 1);
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_analyze_shadow_rejects_reserved_table_id_without_storage() {
    fake::reset();
    fake_compiler::reset();
    // 空运行时 Catalog：next_table_id 为 0，影子候选落在系统保留 ID 区间。
    fake::set_tables({});

    Database database;
    CHECK(open_database(database));

    std::deque<CompileResult> results;
    results.emplace_back(fake_compiler::make_compile_error(
        CompileStage::kSemantic,
        "select missing;",
        7,
        14,
        "unknown column \"missing\""));
    results.emplace_back(create_table_plan("events"));
    results.emplace_back(create_table_plan("later"));
    fake_compiler::set_compile_results(std::move(results));

    const auto script = database.execute_script(ExecuteScriptRequest{
        "select missing;\n"
        "create table events (id int);\n"
        "create table later (id int);",
        ScriptErrorPolicy::kAnalyzeRemaining});

    CHECK(!script.script_error.has_value());
    CHECK(script.statements.size() == 3);
    CHECK(status_at(script, 1) == StatementStatus::kAnalysisError);
    CHECK(status_at(script, 2) == StatementStatus::kAnalysisError);
    CHECK(
        error_at(script, 1).message.find("reserved") != std::string::npos);
    CHECK(
        error_at(script, 2).message.find("reserved") != std::string::npos);
    CHECK(fake::state().create_table_calls == 0);
    CHECK(fake::state().tables.empty());
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_analyze_rejects_table_id_exhaustion_without_wraparound() {
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({
        TableMeta{std::numeric_limits<TableId>::max(), "last_table", {{"id", Type::kInt}}},
    });

    Database database;
    CHECK(open_database(database));

    std::deque<CompileResult> results;
    results.emplace_back(fake_compiler::make_compile_error(
        CompileStage::kSemantic,
        "select missing from last_table;",
        7,
        14,
        "unknown column \"missing\""));
    results.emplace_back(create_table_plan("events"));
    results.emplace_back(create_table_plan("later"));
    fake_compiler::set_compile_results(std::move(results));

    const auto script = database.execute_script(ExecuteScriptRequest{
        "select missing from last_table;\n"
        "create table events (id int);\n"
        "create table later (id int);",
        ScriptErrorPolicy::kAnalyzeRemaining});

    CHECK(!script.script_error.has_value());
    CHECK(script.statements.size() == 3);
    CHECK(status_at(script, 0) == StatementStatus::kCompileError);
    CHECK(status_at(script, 1) == StatementStatus::kAnalysisError);
    CHECK(status_at(script, 2) == StatementStatus::kAnalysisError);
    CHECK(
        error_at(script, 1).message.find("table id exhausted") !=
        std::string::npos);
    CHECK(
        error_at(script, 2).message.find("table id exhausted") !=
        std::string::npos);
    CHECK(fake::state().create_table_calls == 0);

    // 运行时计数器与影子计数器都不回绕：真实 CREATE 仍然只报 kExecute。
    fake_compiler::reset();
    std::deque<CompileResult> later_results;
    later_results.emplace_back(create_table_plan("later"));
    fake_compiler::set_compile_results(std::move(later_results));
    const auto later_script = database.execute_script(
        ExecuteScriptRequest{"create table later (id int);"});
    CHECK(status_at(later_script, 0) == StatementStatus::kExecutionError);
    CHECK(error_at(later_script, 0).kind == ErrorKind::kExecute);
    CHECK(fake::state().create_table_calls == 0);
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_compiler_exception_stops_script() {
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({TableMeta{1, "users", {{"id", Type::kInt}}}});

    Database database;
    CHECK(open_database(database));

    std::deque<CompileResult> results;
    results.emplace_back(create_table_plan("orders"));
    results.emplace_back(create_table_plan("events"));
    results.emplace_back(create_table_plan("later"));
    fake_compiler::set_compile_results(std::move(results));
    fake_compiler::state().throw_on_compile_call = 2;

    const auto script = database.execute_script(ExecuteScriptRequest{
        "create table orders (id int);\n"
        "create table events (id int);\n"
        "create table later (id int);",
        ScriptErrorPolicy::kAnalyzeRemaining});

    // compile 抛异常时执行阶段从未开始：当前语句起全部 skipped。
    CHECK(status_at(script, 0) == StatementStatus::kExecuted);
    CHECK(status_at(script, 1) == StatementStatus::kSkippedExecution);
    CHECK(status_at(script, 2) == StatementStatus::kSkippedExecution);
    CHECK(has_script_error(script, ErrorKind::kInternal));
    CHECK(script.script_error->source.has_value());
    CHECK(script.script_error->source->begin_offset == script.statements[1].source().begin_offset);
    CHECK(fake_compiler::state().compile_calls == 2);
    CHECK(fake::state().create_table_calls == 1);
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_storage_exception_is_indeterminate_and_stops_script() {
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({TableMeta{1, "users", {{"id", Type::kInt}}}});

    Database database;
    CHECK(open_database(database));

    std::deque<CompileResult> results;
    results.emplace_back(insert_plan(1, {{Value{std::int32_t{1}}}}));
    results.emplace_back(create_table_plan("events"));
    results.emplace_back(create_table_plan("later"));
    fake_compiler::set_compile_results(std::move(results));
    fake::set_throw_on_insert(true);

    const auto script = database.execute_script(ExecuteScriptRequest{
        "insert into users values (1);\n"
        "create table events (id int);\n"
        "create table later (id int);",
        ScriptErrorPolicy::kAnalyzeRemaining});

    CHECK(script.statements.size() == 3);
    CHECK(status_at(script, 0) == StatementStatus::kExecutionIndeterminate);
    CHECK(status_at(script, 1) == StatementStatus::kSkippedExecution);
    CHECK(status_at(script, 2) == StatementStatus::kSkippedExecution);
    CHECK(has_script_error(script, ErrorKind::kInternal));
    CHECK(script.script_error->source.has_value());
    CHECK(script.script_error->source->begin_offset == script.statements[0].source().begin_offset);
    CHECK(fake::state().insert_calls == 1);
    CHECK(fake::state().create_table_calls == 0);
    CHECK(fake_compiler::state().compile_calls == 1);

    fake::set_throw_on_insert(false);
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_analyze_after_execution_error_keeps_gate_closed() {
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({TableMeta{1, "users", {{"id", Type::kInt}}}});

    Database database;
    CHECK(open_database(database));

    std::deque<CompileResult> results;
    results.emplace_back(create_table_plan("orders"));
    results.emplace_back(insert_plan(1, {{Value{std::int32_t{1}}}}));
    results.emplace_back(create_table_plan("events"));
    results.emplace_back(scan_plan(1));
    fake_compiler::set_compile_results(std::move(results));
    fake::set_insert_error({
        tinydbms::storage::StorageErrorKind::kValueTooLarge,
        "value too large"});

    const auto script = database.execute_script(ExecuteScriptRequest{
        "create table orders (id int);\n"
        "insert into users values (1);\n"
        "create table events (id int);\n"
        "select * from users;",
        ScriptErrorPolicy::kAnalyzeRemaining});

    CHECK(!script.script_error.has_value());
    CHECK(script.statements.size() == 4);
    CHECK(status_at(script, 0) == StatementStatus::kExecuted);
    CHECK(status_at(script, 1) == StatementStatus::kExecutionError);
    CHECK(error_at(script, 1).kind == ErrorKind::kStorage);
    CHECK(status_at(script, 2) == StatementStatus::kAnalysisOnly);
    CHECK(status_at(script, 3) == StatementStatus::kAnalysisOnly);
    CHECK((fake_compiler::state().catalog_sizes ==
           std::vector<std::size_t>{1, 2, 2, 3}));
    // 执行门关闭后 Storage 调用计数冻结。
    CHECK(fake::state().create_table_calls == 1);
    CHECK(fake::state().insert_calls == 1);
    CHECK(fake::state().open_table_calls == 0);
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_split_failures_are_fatal_and_do_not_leak_state() {
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({});

    Database database;
    CHECK(open_database(database));

    // split 抛异常：statements 为空，空插入点，无 Storage 调用。
    fake_compiler::state().throw_on_split = true;
    const auto thrown = database.execute_script(ExecuteScriptRequest{"whatever;"});
    CHECK(thrown.statements.empty());
    CHECK(has_script_error(thrown, ErrorKind::kInternal));
    CHECK(thrown.script_error->source.has_value());
    CHECK(is_empty_insertion_point(*thrown.script_error->source));
    CHECK(fake_compiler::state().compile_calls == 0);
    CHECK(fake::state().create_table_calls == 0);

    // 会话仍可用：下一次调用建立新的脚本执行门。
    fake_compiler::reset();
    std::deque<CompileResult> recovered_results;
    recovered_results.emplace_back(create_table_plan("events"));
    fake_compiler::set_compile_results(std::move(recovered_results));
    const auto recovered = database.execute_script(
        ExecuteScriptRequest{"create table events (id int);"});
    CHECK(status_at(recovered, 0) == StatementStatus::kExecuted);

    // 分句契约违反：sql 与 range 不一致 → kInternal + 空插入点，不编译。
    fake_compiler::reset();
    std::vector<SplitStatement> inconsistent{
        SplitStatement{"x;", SourceRange{SourceLocation{1, 1}, SourceLocation{1, 2}, 0, 1}}};
    fake_compiler::set_split_override(
        std::optional<std::vector<SplitStatement>>{std::move(inconsistent)});
    const auto violated = database.execute_script(ExecuteScriptRequest{"x;"});
    CHECK(violated.statements.empty());
    CHECK(has_script_error(violated, ErrorKind::kInternal));
    CHECK(violated.script_error->source.has_value());
    CHECK(is_empty_insertion_point(*violated.script_error->source));
    CHECK(fake_compiler::state().compile_calls == 0);

    // compile 返回越界范围：当前语句起 skipped，script_error 指向该语句。
    fake_compiler::reset();
    std::deque<CompileResult> bad_results;
    bad_results.emplace_back(CompileError{
        CompileStage::kSyntax,
        SourceRange{SourceLocation{1, 6}, SourceLocation{1, 7}, 5, 6},
        "out of bounds range",
        std::nullopt,
        std::nullopt});
    fake_compiler::set_compile_results(std::move(bad_results));
    const auto invalid_range =
        database.execute_script(ExecuteScriptRequest{"x;"});
    CHECK(invalid_range.statements.size() == 1);
    CHECK(status_at(invalid_range, 0) == StatementStatus::kSkippedExecution);
    CHECK(has_script_error(invalid_range, ErrorKind::kInternal));
    CHECK(invalid_range.script_error->source.has_value());
    CHECK(invalid_range.script_error->source->begin_offset == 0);
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_statement_count_limit() {
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({});

    Database database;
    CHECK(open_database(database));

    std::string allowed_text;
    allowed_text.reserve(tinydbms::core::kMaxStatementsPerScript * 2U);
    for (std::size_t index = 0; index < tinydbms::core::kMaxStatementsPerScript; ++index) {
        allowed_text += "x;";
    }

    // 恰好 4096 条：允许进入编译；stop 策略下第一条错误后其余全部跳过。
    const auto allowed = database.execute_script(ExecuteScriptRequest{allowed_text});
    CHECK(!allowed.script_error.has_value());
    CHECK(allowed.statements.size() == tinydbms::core::kMaxStatementsPerScript);
    CHECK(status_at(allowed, 0) == StatementStatus::kCompileError);
    CHECK(
        status_at(allowed, tinydbms::core::kMaxStatementsPerScript - 1U) ==
        StatementStatus::kSkippedExecution);
    CHECK(fake_compiler::state().compile_calls == 1);

    // 4097 条：脚本级 kCompile/kLex，statements 为空，不编译、不调用 Storage。
    fake_compiler::reset();
    const auto rejected =
        database.execute_script(ExecuteScriptRequest{allowed_text + "x;"});
    CHECK(rejected.statements.empty());
    CHECK(has_script_error(rejected, ErrorKind::kCompile));
    CHECK(rejected.script_error->compile_stage.has_value());
    CHECK(*rejected.script_error->compile_stage == CompileStage::kLex);
    CHECK(rejected.script_error->source.has_value());
    CHECK(is_empty_insertion_point(*rejected.script_error->source));
    CHECK(fake_compiler::state().split_calls == 1);
    CHECK(fake_compiler::state().compile_calls == 0);
    CHECK(fake::state().create_table_calls == 0);
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_crlf_and_utf8_ranges_are_absolutized() {
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({TableMeta{1, "users", {{"id", Type::kInt}}}});

    Database database;
    CHECK(open_database(database));

    const std::string statement_sql = "select 名字 from users;";
    std::deque<CompileResult> results;
    results.emplace_back(create_table_plan("events"));
    results.emplace_back(fake_compiler::make_compile_error(
        CompileStage::kSemantic,
        statement_sql,
        7,
        13,
        "unknown column \"名字\"",
        std::string{"did you mean \"name\"?"},
        FixIt{fake_compiler::make_range(statement_sql, 7, 13), "name"}));
    fake_compiler::set_compile_results(std::move(results));

    const std::string text = "create table events (id int);\r\n" + statement_sql;
    const auto script = database.execute_script(ExecuteScriptRequest{text});

    CHECK(script.statements.size() == 2);
    CHECK(status_at(script, 1) == StatementStatus::kCompileError);
    CHECK(script.statements[1].source().begin.line == 2);
    CHECK(script.statements[1].source().begin_offset == 31);

    const Error& error = error_at(script, 1);
    CHECK(error.source.has_value());
    CHECK(error.source->begin.line == 2);
    CHECK(error.source->begin.column == 8);
    CHECK(error.source->end.line == 2);
    CHECK(error.source->end.column == 14);
    CHECK(error.source->begin_offset == 38);
    CHECK(error.source->end_offset == 44);

    // fix-it 必须与主诊断使用同一套绝对换算。
    CHECK(error.fix_it.has_value());
    CHECK(error.fix_it->range.begin.line == 2);
    CHECK(error.fix_it->range.begin.column == 8);
    CHECK(error.fix_it->range.end.column == 14);
    CHECK(error.fix_it->range.begin_offset == 38);
    CHECK(error.fix_it->range.end_offset == 44);
    CHECK(error.fix_it->replacement == "name");
    CHECK(error.suggestion.has_value());
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_analyze_collects_multiple_compile_errors_in_order() {
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({TableMeta{1, "users", {{"id", Type::kInt}}}});

    Database database;
    CHECK(open_database(database));

    std::deque<CompileResult> results;
    results.emplace_back(fake_compiler::make_compile_error(
        CompileStage::kLex,
        "select 'unterminated;",
        7,
        20,
        "unterminated string"));
    results.emplace_back(fake_compiler::make_compile_error(
        CompileStage::kSyntax,
        "select from users;",
        0,
        6,
        "missing select list"));
    results.emplace_back(fake_compiler::make_compile_error(
        CompileStage::kSemantic,
        "select missing from users;",
        7,
        14,
        "unknown column \"missing\""));
    fake_compiler::set_compile_results(std::move(results));

    const auto script = database.execute_script(ExecuteScriptRequest{
        "select 'unterminated;\n"
        "select from users;\n"
        "select missing from users;",
        ScriptErrorPolicy::kAnalyzeRemaining});

    CHECK(!script.script_error.has_value());
    CHECK(script.statements.size() == 3);
    for (std::size_t index = 0; index < script.statements.size(); ++index) {
        CHECK(script.statements[index].statement_index() == index);
        CHECK(status_at(script, index) == StatementStatus::kCompileError);
        // 每条错误的绝对范围都落在自己语句的行内，不跨语句、不改写顺序。
        CHECK(script.statements[index].source().begin.line == static_cast<int>(index + 1));
        CHECK(error_at(script, index).source.has_value());
        CHECK(error_at(script, index).source->begin.line == static_cast<int>(index + 1));
        if (index != 0) {
            CHECK(
                error_at(script, index).source->begin_offset >
                error_at(script, index - 1U).source->begin_offset);
        }
    }
    CHECK((error_at(script, 1).compile_stage.has_value() &&
           *error_at(script, 1).compile_stage == CompileStage::kSyntax));
    CHECK((error_at(script, 2).compile_stage.has_value() &&
           *error_at(script, 2).compile_stage == CompileStage::kSemantic));
    CHECK(fake_compiler::state().compile_calls == 3);
    CHECK(fake::state().create_table_calls == 0);
    CHECK(fake::state().open_table_calls == 0);
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_cross_line_range_is_absolutized() {
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({TableMeta{1, "t", {{"id", Type::kInt}}}});

    Database database;
    CHECK(open_database(database));

    const std::string statement_sql = "select a\r\nfrom t;";
    std::deque<CompileResult> results;
    results.emplace_back(create_table_plan("events"));
    results.emplace_back(fake_compiler::make_compile_error(
        CompileStage::kSemantic,
        statement_sql,
        7,
        14,
        "unknown column \"a\""));
    fake_compiler::set_compile_results(std::move(results));

    const std::string text = "create table events (id int);\r\n" + statement_sql;
    const auto script = database.execute_script(ExecuteScriptRequest{text});

    CHECK(script.statements.size() == 2);
    const Error& error = error_at(script, 1);
    CHECK(error.source.has_value());
    // 语句从第 2 行第 1 列开始：相对首行叠加列偏移，其余行直接沿用相对列。
    CHECK(error.source->begin.line == 2);
    CHECK(error.source->begin.column == 8);
    CHECK(error.source->end.line == 3);
    CHECK(error.source->end.column == 5);
    CHECK(error.source->begin_offset == 38);
    CHECK(error.source->end_offset == 45);
    CHECK(!database.close().error.has_value());
    return true;
}

bool test_tab_columns_and_eof_insertion_point() {
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({TableMeta{1, "t", {{"id", Type::kInt}}}});

    Database database;
    CHECK(open_database(database));

    const std::string statement_sql = "select\tmissing from t;";
    std::deque<CompileResult> results;
    results.emplace_back(fake_compiler::make_compile_error(
        CompileStage::kSemantic,
        statement_sql,
        7,
        14,
        "unknown column \"missing\""));
    fake_compiler::set_compile_results(std::move(results));

    const auto script = database.execute_script(ExecuteScriptRequest{statement_sql});
    CHECK(script.statements.size() == 1);
    const Error& error = error_at(script, 0);
    CHECK(error.source.has_value());
    // Tab 按一字节一列参与列计数：offset 7 是列 8，offset 14 是列 15。
    CHECK(error.source->begin.line == 1);
    CHECK(error.source->begin.column == 8);
    CHECK(error.source->begin_offset == 7);
    CHECK(error.source->end.line == 1);
    CHECK(error.source->end.column == 15);
    CHECK(error.source->end_offset == 14);

    // EOF 空插入点（begin == end == text.size()）必须通过校验并原样换算。
    fake_compiler::reset();
    std::deque<CompileResult> eof_results;
    eof_results.emplace_back(fake_compiler::make_compile_error(
        CompileStage::kSyntax,
        statement_sql,
        statement_sql.size(),
        statement_sql.size(),
        "unexpected end of input"));
    fake_compiler::set_compile_results(std::move(eof_results));

    const auto eof_script = database.execute_script(ExecuteScriptRequest{statement_sql});
    CHECK(eof_script.statements.size() == 1);
    const Error& eof_error = error_at(eof_script, 0);
    CHECK(eof_error.source.has_value());
    CHECK(eof_error.source->begin_offset == statement_sql.size());
    CHECK(eof_error.source->end_offset == statement_sql.size());
    const SourceRange eof_relative = fake_compiler::make_range(
        statement_sql, statement_sql.size(), statement_sql.size());
    CHECK(eof_error.source->begin.line == eof_relative.begin.line);
    CHECK(eof_error.source->begin.column == eof_relative.begin.column);
    CHECK(!database.close().error.has_value());
    return true;
}

}  // namespace

int main() {
    const bool passed =
        test_stop_policy_skips_remaining_statements() &&
        test_analyze_policy_continues_with_shadow_catalog() &&
        test_analyze_reports_shadow_rejection_and_analysis_only() &&
        test_analyze_shadow_rejection_does_not_consume_candidate_id() &&
        test_analyze_shadow_rejects_reserved_table_id_without_storage() &&
        test_analyze_rejects_table_id_exhaustion_without_wraparound() &&
        test_compiler_exception_stops_script() &&
        test_storage_exception_is_indeterminate_and_stops_script() &&
        test_analyze_after_execution_error_keeps_gate_closed() &&
        test_split_failures_are_fatal_and_do_not_leak_state() &&
        test_statement_count_limit() &&
        test_crlf_and_utf8_ranges_are_absolutized() &&
        test_analyze_collects_multiple_compile_errors_in_order() &&
        test_cross_line_range_is_absolutized() &&
        test_tab_columns_and_eof_insertion_point();

    if (!passed) {
        return 1;
    }

    std::cout << "core script policy tests passed\n";
    return 0;
}
