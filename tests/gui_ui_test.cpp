#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QStatusBar>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "arguments.hpp"
#include "close_policy.hpp"
#include "diagnostic_list.hpp"
#include "fakes/gui_backend_fake.hpp"
#include "main_window.hpp"
#include "result_model.hpp"
#include "result_panel.hpp"
#include "schema_browser.hpp"
#include "script_editor.hpp"
#include "session_state.hpp"
#include "source_mapping.hpp"

namespace {

using tinydbms::CompileStage;
using tinydbms::FixIt;
using tinydbms::SourceLocation;
using tinydbms::SourceRange;
using tinydbms::Type;
using tinydbms::Value;
using tinydbms::core::CloseDatabaseResult;
using tinydbms::core::ColumnHeader;
using tinydbms::core::CommandResult;
using tinydbms::core::Error;
using tinydbms::core::ErrorKind;
using tinydbms::core::ExecuteResult;
using tinydbms::core::ExecuteScriptResult;
using tinydbms::core::OpenDatabaseResult;
using tinydbms::core::QueryResult;
using tinydbms::core::StatementResult;
using tinydbms::core::StatementStatus;
using tinydbms::gui::DiagnosticList;
using tinydbms::gui::EditorRange;
using tinydbms::gui::MainWindow;
using tinydbms::gui::ResultModel;
using tinydbms::gui::ResultPanel;
using tinydbms::gui::SchemaBrowser;
using tinydbms::gui::ScriptEditor;
using tinydbms::gui::SessionState;
using tinydbms::gui::StatementDisplay;
using tinydbms::gui::StatementView;
using tinydbms::testing::gui_fake::FakeBackend;

int failures = 0;

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " #condition \
                      << '\n';                                                              \
            return false;                                                                    \
        }                                                                                    \
    } while (false)

bool wait_until(const std::function<bool()>& predicate, int timeout_ms = 4000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeout_ms) {
            return false;
        }
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return true;
}

SourceRange range_of(std::size_t begin, std::size_t end) {
    return SourceRange{
        SourceLocation{1, static_cast<int>(begin) + 1},
        SourceLocation{1, static_cast<int>(end) + 1},
        begin,
        end};
}

QueryResult make_query() {
    QueryResult query;
    query.columns = std::vector<ColumnHeader>{
        ColumnHeader{"id", Type::kInt},
        ColumnHeader{"name", Type::kVarchar}};
    query.rows = std::vector<tinydbms::core::Row>{
        tinydbms::core::Row{Value{std::int32_t{1}}, Value{std::string{"甲"}}},
        tinydbms::core::Row{Value{std::int32_t{2}}, Value{std::string{"乙"}}}};
    return query;
}

StatementResult executed_query(std::size_t index, SourceRange range, QueryResult query) {
    return StatementResult::executed(index, range, ExecuteResult{std::move(query)});
}

StatementResult executed_command(std::size_t index, SourceRange range, std::uint64_t rows) {
    return StatementResult::executed(
        index,
        range,
        ExecuteResult{CommandResult{rows, std::nullopt}});
}

Error storage_error(SourceRange range) {
    return Error{
        ErrorKind::kStorage,
        std::nullopt,
        range,
        "write failed",
        std::nullopt,
        std::nullopt};
}

Error fix_it_error(SourceRange range) {
    FixIt fix;
    fix.range = range;
    fix.replacement = "SELECT";
    return Error{
        ErrorKind::kCompile,
        CompileStage::kSyntax,
        range,
        "unexpected identifier",
        std::string{"select 关键字拼写错误"},
        fix};
}

ExecuteScriptResult schema_result() {
    QueryResult tables;
    tables.columns = std::vector<ColumnHeader>{
        ColumnHeader{"table_id", Type::kVarchar},
        ColumnHeader{"table_name", Type::kVarchar},
        ColumnHeader{"column_count", Type::kInt}};
    tables.rows = std::vector<tinydbms::core::Row>{tinydbms::core::Row{
        Value{std::string{"2"}},
        Value{std::string{"t1"}},
        Value{std::int32_t{2}}}};

    QueryResult columns;
    columns.columns = std::vector<ColumnHeader>{
        ColumnHeader{"table_id", Type::kVarchar},
        ColumnHeader{"column_ordinal", Type::kInt},
        ColumnHeader{"column_name", Type::kVarchar},
        ColumnHeader{"column_type", Type::kVarchar}};
    columns.rows = std::vector<tinydbms::core::Row>{
        tinydbms::core::Row{
            Value{std::string{"2"}},
            Value{std::int32_t{0}},
            Value{std::string{"id"}},
            Value{std::string{"INT"}}},
        tinydbms::core::Row{
            Value{std::string{"2"}},
            Value{std::int32_t{1}},
            Value{std::string{"name"}},
            Value{std::string{"VARCHAR"}}}};

    ExecuteScriptResult result;
    result.statements.push_back(executed_query(0, range_of(0, 30), std::move(tables)));
    result.statements.push_back(executed_query(1, range_of(31, 60), std::move(columns)));
    return result;
}

ExecuteScriptResult query_result() {
    ExecuteScriptResult result;
    result.statements.push_back(executed_query(0, range_of(0, 20), make_query()));
    return result;
}

// 两张表，其中表名以 "(2)" 结尾：父子关系必须按 table_id 关联，不能靠节点文本匹配。
ExecuteScriptResult schema_result_with_paren_name() {
    QueryResult tables;
    tables.columns = std::vector<ColumnHeader>{
        ColumnHeader{"table_id", Type::kVarchar},
        ColumnHeader{"table_name", Type::kVarchar},
        ColumnHeader{"column_count", Type::kInt}};
    tables.rows = std::vector<tinydbms::core::Row>{
        tinydbms::core::Row{
            Value{std::string{"2"}}, Value{std::string{"alpha"}}, Value{std::int32_t{1}}},
        tinydbms::core::Row{
            Value{std::string{"3"}}, Value{std::string{"beta (2)"}}, Value{std::int32_t{1}}}};

    QueryResult columns;
    columns.columns = std::vector<ColumnHeader>{
        ColumnHeader{"table_id", Type::kVarchar},
        ColumnHeader{"column_ordinal", Type::kInt},
        ColumnHeader{"column_name", Type::kVarchar},
        ColumnHeader{"column_type", Type::kVarchar}};
    columns.rows = std::vector<tinydbms::core::Row>{
        tinydbms::core::Row{
            Value{std::string{"3"}},
            Value{std::int32_t{0}},
            Value{std::string{"b1"}},
            Value{std::string{"INT"}}},
        tinydbms::core::Row{
            Value{std::string{"2"}},
            Value{std::int32_t{0}},
            Value{std::string{"a1"}},
            Value{std::string{"VARCHAR"}}}};

    ExecuteScriptResult result;
    result.statements.push_back(executed_query(0, range_of(0, 30), std::move(tables)));
    result.statements.push_back(executed_query(1, range_of(31, 60), std::move(columns)));
    return result;
}

ExecuteScriptResult compile_error_result() {
    ExecuteScriptResult result;
    result.statements.push_back(
        StatementResult::compile_error(0, range_of(0, 15), fix_it_error(range_of(0, 5))));
    return result;
}

ExecuteScriptResult internal_error_result() {
    ExecuteScriptResult result;
    result.script_error = Error{
        ErrorKind::kInternal,
        std::nullopt,
        std::nullopt,
        "session invalidated",
        std::nullopt,
        std::nullopt};
    return result;
}

ExecuteScriptResult compile_script_error_result() {
    ExecuteScriptResult result;
    result.script_error = Error{
        ErrorKind::kCompile,
        CompileStage::kLex,
        std::nullopt,
        "unterminated string literal",
        std::nullopt,
        std::nullopt};
    return result;
}

bool test_default_data_dir_matches_cli() {
    const QString cli_default = QString::fromUtf8(
        tinydbms::app::kDefaultDataDir.data(),
        static_cast<qsizetype>(tinydbms::app::kDefaultDataDir.size()));
    CHECK(tinydbms::gui::default_data_dir() == cli_default);
    return true;
}

bool test_utf8_offset_mapping() {
    const QString mixed = QString::fromUtf8("ab中cd");
    CHECK(tinydbms::gui::utf8_offset_to_index(mixed, 0) == std::optional<int>{0});
    CHECK(tinydbms::gui::utf8_offset_to_index(mixed, 2) == std::optional<int>{2});
    CHECK(tinydbms::gui::utf8_offset_to_index(mixed, 3) == std::optional<int>{2});
    CHECK(tinydbms::gui::utf8_offset_to_index(mixed, 4) == std::optional<int>{2});
    CHECK(tinydbms::gui::utf8_offset_to_index(mixed, 5) == std::optional<int>{3});
    CHECK(tinydbms::gui::utf8_offset_to_index(mixed, 7) == std::optional<int>{5});
    CHECK(!tinydbms::gui::utf8_offset_to_index(mixed, 8).has_value());
    CHECK(!tinydbms::gui::utf8_offset_to_index(mixed, 100).has_value());

    // 代理对（4 字节 UTF-8 / 2 个 UTF-16 码元）
    const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
    CHECK(emoji.size() == 2);
    CHECK(tinydbms::gui::utf8_offset_to_index(emoji, 0) == std::optional<int>{0});
    CHECK(tinydbms::gui::utf8_offset_to_index(emoji, 2) == std::optional<int>{0});
    CHECK(tinydbms::gui::utf8_offset_to_index(emoji, 4) == std::optional<int>{2});
    CHECK(!tinydbms::gui::utf8_offset_to_index(emoji, 5).has_value());

    // CRLF 与 Tab 按字节计
    const QString crlf = QString::fromUtf8("a\r\nb");
    CHECK(tinydbms::gui::utf8_offset_to_index(crlf, 3) == std::optional<int>{3});
    CHECK(tinydbms::gui::utf8_offset_to_index(crlf, 4) == std::optional<int>{4});
    const QString tab = QString::fromUtf8("a\tb");
    CHECK(tinydbms::gui::utf8_offset_to_index(tab, 2) == std::optional<int>{2});

    // 空文本：只有偏移 0 合法
    const QString empty;
    CHECK(tinydbms::gui::utf8_offset_to_index(empty, 0) == std::optional<int>{0});
    CHECK(!tinydbms::gui::utf8_offset_to_index(empty, 1).has_value());

    // 区间换算：正常、空插入点、越界与反向区间
    const QString script = QString::fromUtf8("SELECT '中'");
    const auto full = tinydbms::gui::editor_range(
        script,
        range_of(0, static_cast<std::size_t>(script.toUtf8().size())));
    CHECK(full.has_value());
    CHECK(full->begin == 0);
    CHECK(full->end == script.size());
    const auto empty_range = tinydbms::gui::editor_range(script, range_of(3, 3));
    CHECK(empty_range.has_value());
    CHECK(empty_range->begin == 3);
    CHECK(empty_range->end == 3);
    CHECK(!tinydbms::gui::editor_range(script, range_of(0, 100)).has_value());
    CHECK(!tinydbms::gui::editor_range(script, range_of(5, 2)).has_value());
    return true;
}

bool test_statement_views_cover_all_states() {
    ExecuteScriptResult result;
    result.statements.push_back(executed_query(0, range_of(0, 10), make_query()));
    result.statements.push_back(executed_command(1, range_of(11, 20), 0));
    result.statements.push_back(StatementResult::execution_error(
        2, range_of(21, 30), ExecuteResult{CommandResult{3, storage_error(range_of(21, 30))}}));
    result.statements.push_back(
        StatementResult::compile_error(3, range_of(31, 40), fix_it_error(range_of(31, 36))));
    result.statements.push_back(StatementResult::analysis_error(
        4,
        range_of(41, 50),
        Error{ErrorKind::kAnalysis, std::nullopt, std::nullopt, "shadow rejected", std::nullopt,
              std::nullopt}));
    result.statements.push_back(StatementResult::analysis_only(5, range_of(51, 60)));
    result.statements.push_back(StatementResult::skipped(6, range_of(61, 70)));
    result.statements.push_back(StatementResult::execution_indeterminate(7, range_of(71, 80)));

    const std::vector<StatementView> views = tinydbms::gui::build_statement_views(result);
    CHECK(views.size() == 8);
    CHECK(views[0].display == StatementDisplay::kQuery);
    CHECK(views[0].query != nullptr);
    CHECK(views[0].summary.contains(QStringLiteral("查询返回 2 行")));
    CHECK(views[1].display == StatementDisplay::kCommand);
    CHECK(views[1].summary.contains(QStringLiteral("影响 0 行")));
    CHECK(views[2].display == StatementDisplay::kPartialCommand);
    CHECK(views[2].summary.contains(QStringLiteral("已影响 3 行后失败")));
    CHECK(views[3].display == StatementDisplay::kCompileError);
    CHECK(views[3].fix_it.has_value());
    CHECK(views[3].detail.contains(QStringLiteral("建议：select 关键字拼写错误")));
    CHECK(views[4].display == StatementDisplay::kAnalysisError);
    CHECK(views[5].display == StatementDisplay::kAnalysisOnly);
    CHECK(views[6].display == StatementDisplay::kSkipped);
    CHECK(views[7].display == StatementDisplay::kIndeterminate);
    CHECK(views[7].summary.contains(QStringLiteral("状态未知")));

    // 只有带诊断的三条语句进入诊断列表
    CHECK(views[0].detail.isEmpty());
    CHECK(!views[2].detail.isEmpty());
    return true;
}

bool test_session_state_transitions() {
    const ExecuteScriptResult internal = internal_error_result();
    const ExecuteScriptResult compile = compile_script_error_result();
    const ExecuteScriptResult ok = query_result();

    CHECK(
        tinydbms::gui::next_session_state(SessionState::kOpen, internal) ==
        SessionState::kNeedsReopen);
    CHECK(
        tinydbms::gui::next_session_state(SessionState::kOpen, compile) == SessionState::kOpen);
    CHECK(tinydbms::gui::next_session_state(SessionState::kOpen, ok) == SessionState::kOpen);
    CHECK(
        tinydbms::gui::next_session_state(SessionState::kNeedsReopen, compile) ==
        SessionState::kNeedsReopen);
    return true;
}

// CoreBackend 的 close 过滤规则：open 从未成功时，"database is not open"（kExecute）表示
// cleanup 无事可做，算成功；storage/internal 失败必须上抛。
bool test_close_policy_filters_not_open() {
    const CloseDatabaseResult ok{std::nullopt};
    CHECK(tinydbms::gui::close_succeeded(true, ok));
    CHECK(tinydbms::gui::close_succeeded(false, ok));

    const Error not_open{
        ErrorKind::kExecute,
        std::nullopt,
        std::nullopt,
        "database is not open",
        std::nullopt,
        std::nullopt};
    CHECK(tinydbms::gui::close_succeeded(false, CloseDatabaseResult{not_open}));
    CHECK(!tinydbms::gui::close_succeeded(true, CloseDatabaseResult{not_open}));

    const Error storage = storage_error(range_of(0, 0));
    CHECK(!tinydbms::gui::close_succeeded(true, CloseDatabaseResult{storage}));
    CHECK(!tinydbms::gui::close_succeeded(false, CloseDatabaseResult{storage}));

    const Error internal{
        ErrorKind::kInternal,
        std::nullopt,
        std::nullopt,
        "cleanup failed",
        std::nullopt,
        std::nullopt};
    CHECK(!tinydbms::gui::close_succeeded(true, CloseDatabaseResult{internal}));
    CHECK(!tinydbms::gui::close_succeeded(false, CloseDatabaseResult{internal}));
    return true;
}

bool test_window_open_query_flow() {
    FakeBackend backend;
    backend.queue_open(OpenDatabaseResult{std::nullopt});
    backend.queue_script(schema_result());
    backend.queue_script(query_result());

    MainWindow window{backend};
    window.show();
    CHECK(!window.execute_enabled());

    window.script_editor()->setPlainText(QStringLiteral("SELECT * FROM t1"));
    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-a"));
    CHECK(wait_until([&] {
        return window.session_state() == SessionState::kOpen && !window.is_busy() &&
            window.schema_browser()->table_count() == 1;
    }));
    CHECK(window.session_state_text().contains(QStringLiteral("已打开")));
    CHECK(window.session_state_text().contains(QStringLiteral("/tmp/tdb-gui-test-a")));
    CHECK(window.schema_browser()->column_count() == 2);
    CHECK(window.execute_enabled());

    window.execute_editor_text();
    CHECK(wait_until([&] { return !window.is_busy(); }));
    // 连接状态常驻标签与临时消息互不覆盖：耗时提示必须留在状态栏上。
    CHECK(window.statusBar()->currentMessage().contains(QStringLiteral("执行完成")));

    ResultPanel* panel = window.result_panel();
    ResultModel* model = panel->model();
    CHECK(panel->banner_text().isEmpty());
    CHECK(panel->statement_text().contains(QStringLiteral("查询返回 2 行")));
    CHECK(model->rowCount() == 2);
    CHECK(model->columnCount() == 2);
    CHECK(model->headerData(1, Qt::Horizontal).toString() == QStringLiteral("name"));
    CHECK(
        model->data(model->index(0, 1), Qt::DisplayRole).toString() ==
        QString::fromUtf8("甲"));

    // 切换数据库：先 close 再 open
    backend.queue_close(CloseDatabaseResult{std::nullopt});
    backend.queue_open(OpenDatabaseResult{std::nullopt});
    backend.queue_script(schema_result());
    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-b"));
    CHECK(wait_until([&] {
        return window.session_state() == SessionState::kOpen && !window.is_busy();
    }));
    CHECK(backend.close_calls() == 1);
    CHECK(backend.opened_dirs().size() == 2);
    CHECK(backend.opened_dirs().back() == "/tmp/tdb-gui-test-b");

    backend.queue_close(CloseDatabaseResult{std::nullopt});
    window.close();
    CHECK(wait_until([&] { return !window.isVisible(); }));
    CHECK(backend.close_calls() == 2);
    return true;
}

bool test_schema_browser_groups_columns_by_table_id() {
    FakeBackend backend;
    backend.queue_open(OpenDatabaseResult{std::nullopt});
    backend.queue_script(schema_result_with_paren_name());

    MainWindow window{backend};
    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-schema"));
    CHECK(wait_until([&] {
        return window.session_state() == SessionState::kOpen && !window.is_busy() &&
            window.schema_browser()->table_count() == 2;
    }));

    SchemaBrowser* schema = window.schema_browser();
    auto* tree = schema->findChild<QTreeWidget*>();
    CHECK(tree != nullptr);
    CHECK(tree->topLevelItemCount() == 2);
    CHECK(schema->column_count() == 2);
    CHECK(tree->topLevelItem(0)->text(0) == QStringLiteral("alpha (2)"));
    CHECK(tree->topLevelItem(1)->text(0) == QStringLiteral("beta (2) (3)"));
    // 列按 table_id 归属：表名里的 "(2)" 不能影响父子匹配。
    CHECK(tree->topLevelItem(0)->childCount() == 1);
    CHECK(tree->topLevelItem(0)->child(0)->text(0).contains(QStringLiteral("a1")));
    CHECK(tree->topLevelItem(1)->childCount() == 1);
    CHECK(tree->topLevelItem(1)->child(0)->text(0).contains(QStringLiteral("b1")));
    CHECK(schema->status_text().isEmpty());
    return true;
}

bool test_compile_error_and_fix_it() {
    FakeBackend backend;
    backend.queue_open(OpenDatabaseResult{std::nullopt});
    backend.queue_script(schema_result());
    backend.queue_script(compile_error_result());
    backend.queue_script(compile_error_result());

    MainWindow window{backend};
    window.script_editor()->setPlainText(QStringLiteral("SELCT id FROM t1"));
    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-fix"));
    CHECK(wait_until([&] {
        return window.session_state() == SessionState::kOpen && !window.is_busy();
    }));

    window.execute_editor_text();
    CHECK(wait_until([&] { return !window.is_busy(); }));
    DiagnosticList* diagnostics = window.diagnostic_list();
    CHECK(diagnostics->row_count() == 1);
    CHECK(diagnostics->row_has_fix(0));
    CHECK(diagnostics->fix_button_enabled());
    CHECK(window.script_editor()->toPlainText() == QStringLiteral("SELCT id FROM t1"));

    // 文本被手工修改后，旧快照的修复不再可用
    window.script_editor()->setPlainText(QStringLiteral("SELCT id FROM t2"));
    CHECK(!diagnostics->fix_button_enabled());

    // 重新执行后可以应用修复
    window.execute_editor_text();
    CHECK(wait_until([&] { return !window.is_busy(); }));
    CHECK(diagnostics->fix_button_enabled());
    diagnostics->activate_fix(0);
    CHECK(window.script_editor()->toPlainText() == QStringLiteral("SELECT id FROM t2"));
    CHECK(!diagnostics->fix_button_enabled());
    return true;
}

bool test_internal_error_requires_reopen() {
    FakeBackend backend;
    backend.queue_open(OpenDatabaseResult{std::nullopt});
    backend.queue_script(schema_result());
    backend.queue_script(internal_error_result());

    MainWindow window{backend};
    window.script_editor()->setPlainText(QStringLiteral("SELECT * FROM t1"));
    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-internal"));
    CHECK(wait_until([&] {
        return window.session_state() == SessionState::kOpen && !window.is_busy();
    }));

    window.execute_editor_text();
    CHECK(wait_until([&] { return !window.is_busy(); }));
    CHECK(window.session_state() == SessionState::kNeedsReopen);
    CHECK(!window.execute_enabled());
    // 会话失效后旧结构不再可信：表浏览器必须清空而不是继续展示
    CHECK(window.schema_browser()->table_count() == 0);
    CHECK(!window.schema_browser()->status_text().isEmpty());
    CHECK(
        window.result_panel()->banner_text().contains(QStringLiteral("内部错误")));

    // 重新打开：先 close（cleanup 重试）再 open
    backend.queue_close(CloseDatabaseResult{std::nullopt});
    backend.queue_open(OpenDatabaseResult{std::nullopt});
    backend.queue_script(schema_result());
    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-internal"));
    CHECK(wait_until([&] {
        return window.session_state() == SessionState::kOpen && !window.is_busy();
    }));
    CHECK(backend.close_calls() == 1);
    CHECK(backend.open_calls() == 2);
    return true;
}

bool test_compile_script_error_keeps_session() {
    FakeBackend backend;
    backend.queue_open(OpenDatabaseResult{std::nullopt});
    backend.queue_script(schema_result());
    backend.queue_script(compile_script_error_result());

    MainWindow window{backend};
    window.script_editor()->setPlainText(QStringLiteral("SELECT 'unterminated"));
    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-compile"));
    CHECK(wait_until([&] {
        return window.session_state() == SessionState::kOpen && !window.is_busy();
    }));

    window.execute_editor_text();
    CHECK(wait_until([&] { return !window.is_busy(); }));
    CHECK(window.session_state() == SessionState::kOpen);
    CHECK(window.execute_enabled());
    CHECK(!window.result_panel()->banner_text().isEmpty());
    CHECK(
        window.result_panel()->statement_text().contains(QStringLiteral("没有可执行语句")));
    return true;
}

bool test_open_failure_disables_editor() {
    FakeBackend backend;
    backend.queue_open(OpenDatabaseResult{storage_error(range_of(0, 0))});

    MainWindow window{backend};
    window.script_editor()->setPlainText(QStringLiteral("SELECT * FROM t1"));
    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-openfail"));
    CHECK(wait_until([&] { return !window.is_busy(); }));
    CHECK(window.session_state() == SessionState::kClosed);
    CHECK(!window.execute_enabled());
    CHECK(window.script_editor()->isReadOnly());
    CHECK(window.result_panel()->banner_text().contains(QStringLiteral("打开失败")));
    CHECK(backend.execute_calls() == 0);
    return true;
}

// open 失败可能留下 cleanup-pending：重试时必须先 close，成功后才 open。
bool test_open_failure_retries_close_before_reopen() {
    FakeBackend backend;
    backend.queue_open(OpenDatabaseResult{storage_error(range_of(0, 0))});
    backend.queue_close(CloseDatabaseResult{std::nullopt});
    backend.queue_open(OpenDatabaseResult{std::nullopt});
    backend.queue_script(schema_result());

    MainWindow window{backend};
    window.script_editor()->setPlainText(QStringLiteral("SELECT * FROM t1"));
    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-retry"));
    CHECK(wait_until([&] { return !window.is_busy(); }));
    CHECK(window.session_state() == SessionState::kClosed);
    CHECK(window.result_panel()->banner_text().contains(QStringLiteral("打开失败")));

    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-retry"));
    CHECK(wait_until([&] {
        return window.session_state() == SessionState::kOpen && !window.is_busy();
    }));
    CHECK(backend.close_calls() == 1);
    CHECK(backend.open_calls() == 2);
    CHECK(backend.opened_dirs().back() == "/tmp/tdb-gui-test-retry");
    return true;
}

// 清理重试同样失败时不允许继续 open；清理成功后恢复。
bool test_open_failure_cleanup_failure_blocks_reopen() {
    FakeBackend backend;
    backend.queue_open(OpenDatabaseResult{storage_error(range_of(0, 0))});
    backend.queue_close(CloseDatabaseResult{storage_error(range_of(0, 0))});
    backend.queue_close(CloseDatabaseResult{std::nullopt});
    backend.queue_open(OpenDatabaseResult{std::nullopt});
    backend.queue_script(schema_result());

    MainWindow window{backend};
    window.script_editor()->setPlainText(QStringLiteral("SELECT * FROM t1"));
    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-cleanup"));
    CHECK(wait_until([&] { return !window.is_busy(); }));
    CHECK(window.session_state() == SessionState::kClosed);

    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-cleanup"));
    CHECK(wait_until([&] { return !window.is_busy(); }));
    CHECK(window.session_state() == SessionState::kNeedsReopen);
    CHECK(backend.close_calls() == 1);
    CHECK(backend.open_calls() == 1);
    CHECK(
        window.result_panel()->banner_text().contains(QStringLiteral("关闭失败")));

    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-cleanup"));
    CHECK(wait_until([&] {
        return window.session_state() == SessionState::kOpen && !window.is_busy();
    }));
    CHECK(backend.close_calls() == 2);
    CHECK(backend.open_calls() == 2);
    return true;
}

bool test_empty_editor_and_analyze_mode() {
    FakeBackend backend;
    backend.queue_open(OpenDatabaseResult{std::nullopt});
    backend.queue_script(schema_result());
    backend.queue_script(query_result());

    MainWindow window{backend};
    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-empty"));
    CHECK(wait_until([&] {
        return window.session_state() == SessionState::kOpen && !window.is_busy();
    }));
    CHECK(!window.execute_enabled());

    window.script_editor()->setPlainText(QStringLiteral("   \n  "));
    window.execute_editor_text();
    CHECK(window.statusBar()->currentMessage().contains(QStringLiteral("没有可执行语句")));
    CHECK(backend.execute_calls() == 1);  // 只有表结构刷新那一次

    window.set_analyze_mode(true);
    window.script_editor()->setPlainText(QStringLiteral("SELECT * FROM t1"));
    window.execute_editor_text();
    CHECK(wait_until([&] { return !window.is_busy(); }));
    CHECK(backend.last_analyze_mode());
    return true;
}

bool test_close_failure_needs_reopen() {
    FakeBackend backend;
    backend.queue_open(OpenDatabaseResult{std::nullopt});
    backend.queue_script(schema_result());
    backend.queue_close(CloseDatabaseResult{storage_error(range_of(0, 0))});
    backend.queue_close(CloseDatabaseResult{std::nullopt});
    backend.queue_open(OpenDatabaseResult{std::nullopt});
    backend.queue_script(schema_result());

    MainWindow window{backend};
    window.script_editor()->setPlainText(QStringLiteral("SELECT * FROM t1"));
    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-close-a"));
    CHECK(wait_until([&] {
        return window.session_state() == SessionState::kOpen && !window.is_busy();
    }));

    // 切换数据库时 close 失败：不继续 open，进入需要重新打开
    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-close-b"));
    CHECK(wait_until([&] { return !window.is_busy(); }));
    CHECK(window.session_state() == SessionState::kNeedsReopen);
    CHECK(backend.open_calls() == 1);
    CHECK(window.schema_browser()->table_count() == 0);
    CHECK(
        window.result_panel()->banner_text().contains(QStringLiteral("关闭失败")));
    CHECK(window.session_state_text().contains(QStringLiteral("会话需要重新打开")));

    // 再次重试：close 成功后才 open 新目录
    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-close-b"));
    CHECK(wait_until([&] {
        return window.session_state() == SessionState::kOpen && !window.is_busy();
    }));
    CHECK(backend.close_calls() == 2);
    CHECK(backend.open_calls() == 2);
    CHECK(backend.opened_dirs().back() == "/tmp/tdb-gui-test-close-b");
    return true;
}

bool test_window_close_while_busy_is_rejected() {
    FakeBackend backend;
    backend.queue_open(OpenDatabaseResult{std::nullopt});
    backend.queue_script(schema_result());
    backend.queue_script(query_result());

    MainWindow window{backend};
    window.show();
    window.script_editor()->setPlainText(QStringLiteral("SELECT * FROM t1"));
    window.open_directory(QStringLiteral("/tmp/tdb-gui-test-busy"));
    CHECK(wait_until([&] {
        return window.session_state() == SessionState::kOpen && !window.is_busy();
    }));

    window.execute_editor_text();
    window.close();
    CHECK(window.isVisible());

    CHECK(wait_until([&] { return !window.is_busy(); }));
    backend.queue_close(CloseDatabaseResult{std::nullopt});
    window.close();
    CHECK(wait_until([&] { return !window.isVisible(); }));
    return true;
}

void run(const char* name, bool (*test)()) {
    if (test()) {
        std::cout << "PASS " << name << '\n';
        return;
    }
    std::cerr << "FAIL " << name << '\n';
    ++failures;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication application{argc, argv};
    qRegisterMetaType<tinydbms::gui::ExecutionPayload>();
    qRegisterMetaType<tinydbms::gui::LifecyclePayload>();
    qRegisterMetaType<tinydbms::gui::LifecycleAction>();

    run("default_data_dir_matches_cli", test_default_data_dir_matches_cli);
    run("utf8_offset_mapping", test_utf8_offset_mapping);
    run("statement_views_cover_all_states", test_statement_views_cover_all_states);
    run("session_state_transitions", test_session_state_transitions);
    run("close_policy_filters_not_open", test_close_policy_filters_not_open);
    run("window_open_query_flow", test_window_open_query_flow);
    run("schema_browser_groups_columns_by_table_id",
        test_schema_browser_groups_columns_by_table_id);
    run("compile_error_and_fix_it", test_compile_error_and_fix_it);
    run("internal_error_requires_reopen", test_internal_error_requires_reopen);
    run("compile_script_error_keeps_session", test_compile_script_error_keeps_session);
    run("open_failure_disables_editor", test_open_failure_disables_editor);
    run("open_failure_retries_close_before_reopen",
        test_open_failure_retries_close_before_reopen);
    run("open_failure_cleanup_failure_blocks_reopen",
        test_open_failure_cleanup_failure_blocks_reopen);
    run("empty_editor_and_analyze_mode", test_empty_editor_and_analyze_mode);
    run("close_failure_needs_reopen", test_close_failure_needs_reopen);
    run("window_close_while_busy_is_rejected", test_window_close_while_busy_is_rejected);

    if (failures != 0) {
        std::cerr << failures << " GUI test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all GUI tests passed\n";
    return EXIT_SUCCESS;
}
