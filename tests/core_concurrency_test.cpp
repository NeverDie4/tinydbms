// U6 进程内并发的 core 侧验收：多个执行流共享同一个 Database 实例时，
// core 必须把公开方法串行化，storage 的调用序列不得交错。
//
// 取证方式：fake storage 在每次公共入口内部维护"同时进入的调用方数量"峰值
// （`fake::max_concurrent_calls()`），并用阻塞钩子把调用方停在 storage 内部，
// 给第二个执行流留出强行进入的窗口。core 串行化正确时峰值恒为 1、
// `call_order` 呈现两条完整且不交错的调用序列。
#include "fakes/compiler_fake.hpp"
#include "fakes/storage_fake.hpp"
#include "tinydbms/core.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using tinydbms::ColumnMeta;
using tinydbms::TableId;
using tinydbms::TableMeta;
using tinydbms::Type;
using tinydbms::Value;
using tinydbms::compiler::CompileResult;
using tinydbms::compiler::Plan;
using tinydbms::compiler::PlanNode;
using tinydbms::compiler::QueryOutput;
using tinydbms::compiler::QueryPlan;
using tinydbms::compiler::ScanColumn;
using tinydbms::compiler::SeqScanNode;
using tinydbms::core::CancelToken;
using tinydbms::core::Database;
using tinydbms::core::ExecuteScriptRequest;
using tinydbms::core::ExecuteScriptResult;
using tinydbms::core::QueryResult;
using tinydbms::core::StatementResult;
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

// 测试侧门闩：让一次 storage 调用在"调用方仍持有 Database 锁"时停住，
// 从而制造第二个执行流尝试进入的窗口。钩子可能被两个执行流调用，因此自身必须是
// 线程安全的；release() 之后所有后续 block() 立即返回，避免顺序执行的执行流被卡住。
class Gate {
public:
    void block() {
        std::unique_lock<std::mutex> lock{mutex_};
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    bool wait_until_blocked(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock{mutex_};
        return condition_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            released_ = true;
        }
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool released_ = false;
};

TableMeta users_table() {
    return TableMeta{
        1,
        "users",
        std::vector<ColumnMeta>{
            {"id", Type::kInt},
            {"name", Type::kVarchar},
            {"age", Type::kInt}}};
}

Value int_value(std::int32_t value) {
    return Value{value};
}

Value text_value(std::string value) {
    return Value{std::move(value)};
}

tinydbms::storage::Record users_record(
    std::uint64_t rid,
    std::int32_t id,
    std::string name,
    std::int32_t age) {
    return tinydbms::storage::Record{
        tinydbms::storage::RecordId{rid},
        std::vector<Value>{int_value(id), text_value(std::move(name)), int_value(age)}};
}

std::vector<ScanColumn> default_scan_columns() {
    return {{0, 0}, {1, 1}, {2, 2}};
}

std::vector<QueryOutput> default_query_outputs() {
    return {
        {0, "id", Type::kInt, false},
        {1, "name", Type::kVarchar, false},
        {2, "age", Type::kInt, false}};
}

Plan select_users_plan() {
    return Plan{QueryPlan{
        std::make_unique<PlanNode>(
            SeqScanNode{1, default_scan_columns()}),
        default_query_outputs()}};
}

bool open_database(Database& database) {
    const auto result = database.open(tinydbms::core::OpenDatabaseRequest{"test-data"});
    return !result.error.has_value();
}

bool close_database(Database& database) {
    return !database.close().error.has_value();
}

// 打开一个带 3 行 users 数据的 fake 会话；fake 侧计数与调用序列同时清零。
bool start_database(Database& database) {
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({users_table()});
    fake::set_records_for_table(
        users_table().table_id,
        {users_record(1, 1, "alice", 30),
         users_record(2, 2, "bob", 41),
         users_record(3, 3, "carol", 52)});
    return open_database(database);
}

// 为 count 条单语句脚本排队编译结果。fake 的队列是所有执行流共享的，
// 串行化正确时它只在 Database 锁内被访问。
void queue_select_plans(std::size_t count) {
    fake_compiler::reset();
    std::deque<CompileResult> plans;
    for (std::size_t index = 0; index < count; ++index) {
        plans.emplace_back(select_users_plan());
    }
    fake_compiler::set_compile_results(std::move(plans));
}

const QueryResult* query_of(const StatementResult& statement) {
    if (!statement.outcome().has_value()) {
        return nullptr;
    }
    return std::get_if<QueryResult>(&statement.outcome()->outcome);
}

// 取 call_order 中从 baseline 起的片段，便于忽略打开会话阶段已经记录的调用。
std::vector<std::string> calls_since(std::size_t baseline) {
    const std::vector<std::string>& all = fake::state().call_order;
    if (baseline >= all.size()) {
        return {};
    }
    return std::vector<std::string>{
        all.begin() + static_cast<std::ptrdiff_t>(baseline),
        all.end()};
}

// 一次 SELECT 在 fake 上留下的完整调用序列：3 行数据 + 一次扫描结束调用。
std::vector<std::string> expected_select_calls() {
    return {"open_table", "scan_next", "scan_next", "scan_next", "scan_next", "close_cursor"};
}

// 两个执行流并发执行相同脚本：storage 调用序列必须严格不交错，两次结果都完整。
bool test_concurrent_scripts_serialize_storage_calls() {
    Database database;
    CHECK(start_database(database));

    const std::size_t baseline = fake::state().call_order.size();
    Gate gate;
    fake::set_on_storage_call([&gate](const char* call) {
        if (std::string_view{call} == "open_table") {
            gate.block();
        }
    });
    queue_select_plans(2);

    std::vector<ExecuteScriptResult> results(2);
    std::thread first{[&database, &results] {
        results[0] = database.execute_script(ExecuteScriptRequest{"statement;"});
    }};
    const bool first_inside_storage = gate.wait_until_blocked(std::chrono::seconds{5});
    std::thread second{[&database, &results] {
        results[1] = database.execute_script(ExecuteScriptRequest{"statement;"});
    }};

    // 第一个执行流仍停在 storage 内部时，第二个执行流已经发起调用；
    // core 串行化正确时它只能等在锁外，无法进入 storage。
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    gate.release();
    first.join();
    second.join();
    fake::set_on_storage_call(nullptr);

    CHECK(first_inside_storage);
    CHECK(!fake::overlap_detected());
    CHECK(fake::max_concurrent_calls() == 1U);
    for (const ExecuteScriptResult& result : results) {
        CHECK(!result.script_error.has_value());
        CHECK(result.statements.size() == 1U);
        CHECK(result.statements.front().status() == StatementStatus::kExecuted);
        const QueryResult* rows = query_of(result.statements.front());
        CHECK(rows != nullptr && rows->rows.size() == 3U);
    }

    std::vector<std::string> expected = expected_select_calls();
    const std::vector<std::string> second_script_calls = expected_select_calls();
    expected.insert(expected.end(), second_script_calls.begin(), second_script_calls.end());
    CHECK(calls_since(baseline) == expected);
    CHECK(close_database(database));
    return true;
}

// 取消隔离：A 的令牌被请求取消时，B 的语句状态与结果不受影响。
bool test_cancellation_is_isolated_between_executions() {
    Database database;
    CHECK(start_database(database));

    Gate gate;
    fake::set_on_storage_call([&gate](const char* call) {
        if (std::string_view{call} == "scan_next") {
            gate.block();
        }
    });
    queue_select_plans(4);

    CancelToken cancelled_token;
    ExecuteScriptResult cancelled_result;
    std::thread cancelled_thread{[&] {
        ExecuteScriptRequest request{"statement;statement;"};
        request.cancel = cancelled_token;
        cancelled_result = database.execute_script(request);
    }};
    const bool blocked = gate.wait_until_blocked(std::chrono::seconds{5});
    cancelled_token.request_cancel();

    CancelToken healthy_token;
    ExecuteScriptResult healthy_result;
    std::thread healthy_thread{[&] {
        ExecuteScriptRequest request{"statement;"};
        request.cancel = healthy_token;
        healthy_result = database.execute_script(request);
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    gate.release();
    cancelled_thread.join();
    healthy_thread.join();
    fake::set_on_storage_call(nullptr);

    CHECK(blocked);
    CHECK(!fake::overlap_detected());
    // A：当前语句在检查点取消，剩余语句既不编译也不执行。
    CHECK(!cancelled_result.script_error.has_value());
    CHECK(cancelled_result.statements.size() == 2U);
    CHECK(cancelled_result.statements[0].status() == StatementStatus::kCancelled);
    CHECK(cancelled_result.statements[1].status() == StatementStatus::kCancelled);
    // B：自己的令牌没有被 A 的取消影响。
    CHECK(!healthy_result.script_error.has_value());
    CHECK(healthy_result.statements.size() == 1U);
    CHECK(healthy_result.statements.front().status() == StatementStatus::kExecuted);
    const QueryResult* rows = query_of(healthy_result.statements.front());
    CHECK(rows != nullptr && rows->rows.size() == 3U);
    CHECK(close_database(database));
    return true;
}

// close 与在途执行互斥：close 必须等在途语句结束后才关闭 storage。
bool test_concurrent_close_waits_for_in_flight_execution() {
    Database database;
    CHECK(start_database(database));

    const std::size_t baseline = fake::state().call_order.size();
    Gate gate;
    fake::set_on_storage_call([&gate](const char* call) {
        if (std::string_view{call} == "scan_next") {
            gate.block();
        }
    });
    queue_select_plans(1);

    ExecuteScriptResult execution;
    std::thread executing_thread{[&] {
        execution = database.execute_script(ExecuteScriptRequest{"statement;"});
    }};
    const bool blocked = gate.wait_until_blocked(std::chrono::seconds{5});
    bool close_failed = false;
    std::thread closing_thread{[&] { close_failed = !close_database(database); }};

    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    gate.release();
    executing_thread.join();
    closing_thread.join();
    fake::set_on_storage_call(nullptr);

    CHECK(blocked);
    CHECK(!fake::overlap_detected());
    CHECK(!close_failed);
    CHECK(!execution.script_error.has_value());
    CHECK(execution.statements.size() == 1U);
    const QueryResult* rows = query_of(execution.statements.front());
    CHECK(rows != nullptr && rows->rows.size() == 3U);

    // 顺序证据：先关 cursor，再关 storage；close 没有插进扫描中间。
    std::vector<std::string> expected = expected_select_calls();
    expected.emplace_back("close_storage");
    CHECK(calls_since(baseline) == expected);
    return true;
}

// open 与 execute_script 互斥：open 仍在 storage 内部时发起的执行必须等到 open 完成，
// 看到的是完整打开的会话，而不是半初始化的状态。
bool test_concurrent_open_and_execute_are_serialized() {
    fake::reset();
    fake_compiler::reset();
    fake::set_tables({users_table()});
    fake::set_records_for_table(
        users_table().table_id,
        {users_record(1, 1, "alice", 30)});

    Gate gate;
    fake::set_on_storage_call([&gate](const char* call) {
        if (std::string_view{call} == "open_storage") {
            gate.block();
        }
    });
    queue_select_plans(1);

    Database database;
    bool opened = false;
    std::thread opening_thread{[&] {
        opened = open_database(database);
    }};
    const bool blocked = gate.wait_until_blocked(std::chrono::seconds{5});

    // 第三个执行流发起调用：open 仍在 storage 内部，它只能等在锁外。
    ExecuteScriptResult execution;
    std::thread executing_thread{[&] {
        execution = database.execute_script(ExecuteScriptRequest{"statement;"});
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    gate.release();
    opening_thread.join();
    executing_thread.join();
    fake::set_on_storage_call(nullptr);

    CHECK(blocked);
    CHECK(opened);
    CHECK(!fake::overlap_detected());
    CHECK(fake::max_concurrent_calls() == 1U);
    // execute_script 在 open 之后执行：拿到的是已经打开、Catalog 已恢复的会话。
    CHECK(!execution.script_error.has_value());
    CHECK(execution.statements.size() == 1U);
    const QueryResult* rows = query_of(execution.statements.front());
    CHECK(rows != nullptr && rows->rows.size() == 1U);
    // 调用序列同样给出顺序证据：open 自己的调用全部结束后才出现查询的调用。
    CHECK(fake::state().call_order ==
          (std::vector<std::string>{
              "open_storage",
              "list_tables",
              "open_table",
              "scan_next",
              "scan_next",
              "close_cursor"}));
    CHECK(close_database(database));
    return true;
}

// 未打开的会话上并发发起 close 与 execute_script：两者都必须返回与顺序执行一致的
// "database is not open"，且不产生任何 storage 调用。
bool test_concurrent_calls_on_unopened_database_report_consistent_errors() {
    fake::reset();
    fake_compiler::reset();
    queue_select_plans(1);

    Database database;
    bool close_failed = false;
    ExecuteScriptResult execution;
    std::thread closing_thread{[&] { close_failed = !close_database(database); }};
    std::thread executing_thread{[&] {
        execution = database.execute_script(ExecuteScriptRequest{"statement;"});
    }};
    closing_thread.join();
    executing_thread.join();

    CHECK(close_failed);
    CHECK(execution.script_error.has_value());
    CHECK(execution.script_error->kind == tinydbms::core::ErrorKind::kExecute);
    CHECK(execution.statements.empty());
    CHECK(fake::state().call_order.empty());
    CHECK(fake_compiler::state().compile_calls == 0U);
    return true;
}

// 预置令牌：请求在取得执行权后立即把全部语句记为取消，不编译、不触碰 storage。
bool test_pre_cancelled_request_skips_storage() {
    Database database;
    CHECK(start_database(database));

    Gate gate;
    fake::set_on_storage_call([&gate](const char* call) {
        if (std::string_view{call} == "open_table") {
            gate.block();
        }
    });
    queue_select_plans(4);

    ExecuteScriptResult running;
    std::thread running_thread{[&] {
        running = database.execute_script(ExecuteScriptRequest{"statement;"});
    }};
    const bool blocked = gate.wait_until_blocked(std::chrono::seconds{5});

    CancelToken pre_cancelled;
    pre_cancelled.request_cancel();
    ExecuteScriptResult skipped;
    std::thread skipped_thread{[&] {
        ExecuteScriptRequest request{"statement;"};
        request.cancel = pre_cancelled;
        skipped = database.execute_script(request);
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    gate.release();
    running_thread.join();
    skipped_thread.join();
    fake::set_on_storage_call(nullptr);

    CHECK(blocked);
    CHECK(!fake::overlap_detected());
    CHECK(!skipped.script_error.has_value());
    CHECK(skipped.statements.size() == 1U);
    CHECK(skipped.statements.front().status() == StatementStatus::kCancelled);
    // 只有正在运行的执行流编译并访问了 storage。
    CHECK(fake_compiler::state().compile_calls == 1U);
    CHECK(fake::state().open_table_calls == 1U);

    CHECK(!running.script_error.has_value());
    const QueryResult* rows = query_of(running.statements.front());
    CHECK(rows != nullptr && rows->rows.size() == 3U);
    CHECK(close_database(database));
    return true;
}

}  // namespace

int main() {
    const bool passed =
        test_concurrent_scripts_serialize_storage_calls() &&
        test_cancellation_is_isolated_between_executions() &&
        test_concurrent_close_waits_for_in_flight_execution() &&
        test_concurrent_open_and_execute_are_serialized() &&
        test_concurrent_calls_on_unopened_database_report_consistent_errors() &&
        test_pre_cancelled_request_skips_storage();

    if (!passed) {
        return 1;
    }

    std::cout << "core concurrency tests passed\n";
    return 0;
}
