#include "main_window.hpp"

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QThread>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>
#include <vector>
#include <variant>

#include "backend.hpp"
#include "diagnostic_list.hpp"
#include "result_panel.hpp"
#include "schema_browser.hpp"
#include "script_editor.hpp"
#include "worker.hpp"

namespace tinydbms::gui {
namespace {

// CLI 的默认数据目录（src/app/arguments.hpp 的 kDefaultDataDir）。
// GUI 使用自己的常量，并由 tests/gui_ui_test.cpp 交叉断言两者一致。
constexpr const char* kDefaultDataDir = "./tinydbms-data";

constexpr const char* kSchemaQuery =
    "SELECT * FROM tdb_sys_tables; SELECT * FROM tdb_sys_columns";

}  // namespace

QString default_data_dir() {
    return QString::fromUtf8(kDefaultDataDir);
}

MainWindow::MainWindow(Backend& backend, QWidget* parent)
    : QMainWindow{parent}, backend_{&backend} {
    setWindowTitle(QStringLiteral("tinydbms"));

    editor_ = new ScriptEditor{this};
    result_panel_ = new ResultPanel{this};
    diagnostics_ = new DiagnosticList{this};
    schema_ = new SchemaBrowser{this};

    auto* left = new QWidget{this};
    auto* left_layout = new QVBoxLayout{left};
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->addWidget(editor_, 3);
    left_layout->addWidget(diagnostics_, 2);

    auto* central = new QSplitter{Qt::Horizontal, this};
    central->addWidget(left);
    central->addWidget(result_panel_);
    central->setStretchFactor(0, 1);
    central->setStretchFactor(1, 1);
    setCentralWidget(central);
    auto* schema_dock = new QDockWidget{QStringLiteral("表结构"), this};
    schema_dock->setWidget(schema_);
    addDockWidget(Qt::RightDockWidgetArea, schema_dock);

    auto* toolbar = addToolBar(QStringLiteral("主工具栏"));
    open_button_ = new QPushButton{QStringLiteral("打开/切换数据库"), this};
    execute_button_ = new QPushButton{QStringLiteral("执行 (Ctrl+Enter)"), this};
    analyze_box_ = new QCheckBox{
        QStringLiteral("出错后继续分析剩余语句（首错后的语句只分析、不执行）"), this};
    path_label_ = new QLabel{this};
    // 连接状态常驻状态栏右侧；状态栏左侧留给「没有可执行语句」「执行完成，用时 N ms」
    // 这类临时消息，两者互不覆盖。
    status_state_ = new QLabel{this};
    statusBar()->addPermanentWidget(status_state_);

    toolbar->addWidget(open_button_);
    toolbar->addWidget(execute_button_);
    toolbar->addWidget(analyze_box_);
    toolbar->addWidget(path_label_);

    auto* execute_action = new QAction{this};
    execute_action->setShortcut(QKeySequence{QStringLiteral("Ctrl+Return")});
    connect(execute_action, &QAction::triggered, this, &MainWindow::execute_editor_text);
    addAction(execute_action);

    thread_ = new QThread{this};
    worker_ = new Worker{backend_};
    worker_->moveToThread(thread_);

    connect(this, &MainWindow::request_open, worker_, &Worker::open_database);
    connect(this, &MainWindow::request_execute, worker_, &Worker::execute_script);
    connect(this, &MainWindow::request_close, worker_, &Worker::close_database);
    connect(worker_, &Worker::lifecycle_finished, this, &MainWindow::on_lifecycle_finished);
    connect(worker_, &Worker::script_finished, this, &MainWindow::on_script_finished);

    connect(open_button_, &QPushButton::clicked, this, &MainWindow::choose_directory);
    connect(execute_button_, &QPushButton::clicked, this, &MainWindow::execute_editor_text);
    connect(analyze_box_, &QCheckBox::toggled, this, [this](bool) { update_controls(); });
    connect(editor_, &ScriptEditor::text_edited, this, &MainWindow::on_editor_text_edited);
    connect(diagnostics_, &DiagnosticList::fix_requested, this, &MainWindow::on_fix_requested);
    connect(schema_, &SchemaBrowser::refresh_requested, this, [this] { refresh_schema_browser(); });

    thread_->start();
    update_controls();
}

MainWindow::~MainWindow() {
    thread_->quit();
    thread_->wait();
    delete worker_;
    worker_ = nullptr;
}

std::uint64_t MainWindow::next_snapshot_id() {
    ++snapshot_counter_;
    return snapshot_counter_;
}

void MainWindow::open_directory(const QString& path) {
    if (busy_) {
        return;
    }
    if (state_ == SessionState::kOpen || state_ == SessionState::kNeedsReopen ||
        cleanup_may_be_pending_) {
        // 切换、重开以及 open 失败后的重试都先 close：close 是 cleanup-pending 的唯一
        // 重试入口。最后一种情况不能靠 core 的返回值区分，只能先清一次。
        pending_directory_ = path;
        busy_ = true;
        update_controls();
        emit request_close();
        return;
    }
    busy_ = true;
    update_controls();
    emit request_open(path);
}

void MainWindow::choose_directory() {
    const QString start = data_dir_.isEmpty() ? QString::fromUtf8(kDefaultDataDir) : data_dir_;
    const QString directory =
        QFileDialog::getExistingDirectory(this, QStringLiteral("选择数据目录"), start);
    if (directory.isEmpty()) {
        return;
    }
    open_directory(directory);
}

void MainWindow::execute_editor_text() {
    if (state_ != SessionState::kOpen || busy_) {
        return;
    }
    const QString text = editor_->toPlainText();
    if (text.trimmed().isEmpty()) {
        // 不覆盖上一次的结果，只在状态栏提示。
        statusBar()->showMessage(QStringLiteral("没有可执行语句"), 5000);
        return;
    }

    executed_snapshot_ = text;
    executed_snapshot_id_ = next_snapshot_id();
    busy_ = true;
    timer_.start();
    update_controls();
    emit request_execute(text, analyze_box_->isChecked(), executed_snapshot_id_);
}

void MainWindow::refresh_schema_browser() {
    if (state_ != SessionState::kOpen || busy_) {
        return;
    }
    schema_snapshot_ = next_snapshot_id();
    busy_ = true;
    update_controls();
    emit request_execute(
        QString::fromUtf8(kSchemaQuery),
        false,
        *schema_snapshot_);
}

void MainWindow::set_analyze_mode(bool enabled) {
    analyze_box_->setChecked(enabled);
}

ScriptEditor* MainWindow::script_editor() const noexcept {
    return editor_;
}

ResultPanel* MainWindow::result_panel() const noexcept {
    return result_panel_;
}

DiagnosticList* MainWindow::diagnostic_list() const noexcept {
    return diagnostics_;
}

SchemaBrowser* MainWindow::schema_browser() const noexcept {
    return schema_;
}

SessionState MainWindow::session_state() const noexcept {
    return state_;
}

QString MainWindow::session_state_text() const {
    return status_state_->text();
}

bool MainWindow::execute_enabled() const {
    return execute_button_->isEnabled();
}

bool MainWindow::is_busy() const noexcept {
    return busy_;
}

QString MainWindow::error_text(const std::shared_ptr<const tinydbms::core::Error>& error) {
    if (error == nullptr) {
        return QStringLiteral("未知错误");
    }
    QString text = error_kind_label(*error);
    text += QStringLiteral("：%1").arg(QString::fromUtf8(error->message));
    if (error->suggestion.has_value()) {
        text += QStringLiteral("\n建议：%1").arg(QString::fromUtf8(*error->suggestion));
    }
    return text;
}

void MainWindow::on_lifecycle_finished(const LifecyclePayload& payload) {
    busy_ = false;
    switch (payload.action) {
    case LifecycleAction::kOpened:
        data_dir_ = QString::fromUtf8(payload.data_dir);
        path_label_->setText(data_dir_);
        result_panel_->clear();
        schema_->clear();
        set_session_state(SessionState::kOpen);
        refresh_schema_browser();
        break;
    case LifecycleAction::kOpenFailed:
        // open 失败可能伴随 cleanup-pending（storage 清理也失败）。公共契约不暴露该标志，
        // 所以这里一律按「可能脏」处理：下一次打开前先 close 重试，成功才继续 open。
        cleanup_may_be_pending_ = true;
        data_dir_.clear();
        path_label_->clear();
        result_panel_->show_notice(
            QStringLiteral("打开失败\n%1").arg(error_text(payload.error)));
        set_session_state(SessionState::kClosed);
        break;
    case LifecycleAction::kClosed:
        // close 成功即代表没有待清理的 storage 生命周期。
        cleanup_may_be_pending_ = false;
        data_dir_.clear();
        path_label_->clear();
        schema_->clear();
        set_session_state(SessionState::kClosed);
        if (pending_directory_.has_value()) {
            const QString directory = *pending_directory_;
            pending_directory_.reset();
            open_directory(directory);
            break;
        }
        if (closing_) {
            force_close_ = true;
            close();
        }
        break;
    case LifecycleAction::kCloseFailed:
        pending_directory_.reset();
        // 会话可能仍持有 cursor 或未完成的写入：旧结构不再可信。
        schema_->clear();
        result_panel_->show_notice(
            QStringLiteral("关闭失败，会话需要重新打开\n%1").arg(error_text(payload.error)));
        set_session_state(SessionState::kNeedsReopen);
        if (closing_) {
            closing_ = false;
            force_close_ = true;
            close();
        }
        break;
    }
}

void MainWindow::on_script_finished(const ExecutionPayload& payload) {
    busy_ = false;

    if (schema_snapshot_.has_value() && payload.snapshot_id == *schema_snapshot_) {
        schema_snapshot_.reset();
        SessionState next = state_;
        if (payload.result != nullptr) {
            const auto& result = *payload.result;
            next = next_session_state(state_, result);
            if (next == SessionState::kNeedsReopen) {
                // 会话已失效：清掉可能过期的旧结构，只保留失败说明。
                schema_->clear();
            }
            if (result.script_error.has_value()) {
                schema_->set_status_text(script_error_banner(result));
                result_panel_->show_notice(script_error_banner(result));
            } else if (result.statements.size() == 2 &&
                result.statements[0].status() == tinydbms::core::StatementStatus::kExecuted &&
                result.statements[1].status() == tinydbms::core::StatementStatus::kExecuted) {
                const auto& tables = std::get<tinydbms::core::QueryResult>(
                    result.statements[0].outcome()->outcome);
                const auto& columns = std::get<tinydbms::core::QueryResult>(
                    result.statements[1].outcome()->outcome);
                schema_->set_tables(tables);
                schema_->set_columns(columns);
            } else {
                schema_->set_status_text(QStringLiteral("无法读取系统表"));
            }
        }
        set_session_state(next);
        return;
    }

    if (payload.snapshot_id != executed_snapshot_id_) {
        update_controls();
        return;
    }
    apply_script_result(payload);
    update_controls();
}

void MainWindow::apply_script_result(const ExecutionPayload& payload) {
    last_payload_ = payload.result;
    result_panel_->show_result(payload.result);

    if (payload.result == nullptr) {
        diagnostics_->clear();
        editor_->clear_diagnostics();
        return;
    }

    const auto& result = *payload.result;
    const std::vector<StatementView> views = build_statement_views(result);
    diagnostics_->set_views(views);

    std::vector<DiagnosticSpan> spans;
    spans.reserve(views.size());
    for (const StatementView& view : views) {
        if (!view.range.has_value()) {
            continue;
        }
        spans.push_back(DiagnosticSpan{*view.range, !view.detail.isEmpty()});
    }
    editor_->apply_diagnostics(spans);
    diagnostics_->set_fix_enabled(editor_->toPlainText() == executed_snapshot_);

    const SessionState next = next_session_state(state_, result);
    if (next == SessionState::kNeedsReopen) {
        schema_->clear();
        schema_->set_status_text(QStringLiteral("会话状态未知，重新打开数据库后再刷新表结构"));
    }
    set_session_state(next);
    const qint64 elapsed = timer_.isValid() ? timer_.elapsed() : 0;
    statusBar()->showMessage(QStringLiteral("执行完成，用时 %1 ms").arg(elapsed), 5000);
}

void MainWindow::on_editor_text_edited() {
    // 文本一旦变化，旧范围不再可信：清空高亮并禁用修复，诊断列表保留文字供参考。
    editor_->clear_diagnostics();
    diagnostics_->set_fix_enabled(false);
    update_controls();
}

void MainWindow::on_fix_requested(int statement_index) {
    if (last_payload_ == nullptr) {
        return;
    }
    const std::vector<StatementView> views = build_statement_views(*last_payload_);
    for (const StatementView& view : views) {
        if (static_cast<int>(view.index) != statement_index || !view.fix_it.has_value()) {
            continue;
        }
        if (editor_->apply_fix(*view.fix_it)) {
            editor_->clear_diagnostics();
            diagnostics_->set_fix_enabled(false);
        }
        return;
    }
}

void MainWindow::set_session_state(SessionState state) {
    state_ = state;
    update_controls();
}

void MainWindow::update_controls() {
    const bool open = state_ == SessionState::kOpen;
    open_button_->setEnabled(!busy_);
    execute_button_->setEnabled(open && !busy_ && !editor_->toPlainText().trimmed().isEmpty());
    analyze_box_->setEnabled(!busy_);
    editor_->setReadOnly(!open);

    QString state_text;
    switch (state_) {
    case SessionState::kClosed:
        state_text = QStringLiteral("未打开");
        break;
    case SessionState::kOpen:
        state_text = QStringLiteral("已打开");
        break;
    case SessionState::kNeedsReopen:
        state_text = QStringLiteral("会话需要重新打开");
        break;
    }
    if (!data_dir_.isEmpty()) {
        state_text += QStringLiteral("：%1").arg(data_dir_);
    }
    if (busy_) {
        state_text += closing_ ? QStringLiteral("（正在关闭）") : QStringLiteral("（执行中）");
    }
    // 常驻标签承载连接状态，不会被 showMessage 的临时消息（「没有可执行语句」、
    // 「执行完成，用时 N ms」）覆盖，反之亦然。
    status_state_->setText(state_text);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (force_close_) {
        event->accept();
        return;
    }
    if (busy_) {
        result_panel_->show_notice(QStringLiteral("正在执行，请等待当前语句结束后再关闭"));
        event->ignore();
        return;
    }
    if (state_ == SessionState::kClosed) {
        event->accept();
        return;
    }
    // 关闭是一次异步往返：期间必须保持忙碌，避免用户在 close 完成前再次触发执行。
    busy_ = true;
    closing_ = true;
    update_controls();
    emit request_close();
    event->ignore();
}

}  // namespace tinydbms::gui
