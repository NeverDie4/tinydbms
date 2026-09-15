#ifndef TINYDBMS_GUI_MAIN_WINDOW_HPP
#define TINYDBMS_GUI_MAIN_WINDOW_HPP

#include <cstdint>
#include <memory>
#include <optional>

#include <QElapsedTimer>
#include <QMainWindow>
#include <QString>

#include "execution_payload.hpp"
#include "session_state.hpp"

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QPushButton;
class QThread;

namespace tinydbms::gui {

class Backend;
class DiagnosticList;
class ResultPanel;
class SchemaBrowser;
class ScriptEditor;
class Worker;

// 默认数据目录：与 CLI 的 kDefaultDataDir 保持一致，由测试交叉断言防止漂移。
QString default_data_dir();

// 单窗口前端：工具栏 + 编辑器 + 结果区 + 诊断列表 + 表浏览器。
// 所有 core 调用都经 Worker 在工作线程串行执行，窗口只处理展示与状态机。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(Backend& backend, QWidget* parent = nullptr);
    ~MainWindow() override;

    // 手工路径与测试共用的入口：直接以给定目录打开，不弹对话框。
    void open_directory(const QString& path);
    void execute_editor_text();
    // 请求取消当前执行：只置位令牌，core 在下一个检查点生效，不强制中断 storage 调用。
    void request_cancel();
    void set_analyze_mode(bool enabled);
    void set_plan_mode(bool enabled);
    // 选中与给定上限匹配的档位；入口只提供固定档位，无匹配项时忽略。
    void set_max_rows(quint64 rows);

    ScriptEditor* script_editor() const noexcept;
    ResultPanel* result_panel() const noexcept;
    DiagnosticList* diagnostic_list() const noexcept;
    SchemaBrowser* schema_browser() const noexcept;

    SessionState session_state() const noexcept;
    // 状态栏常驻标签的文本（测试与手工排查共用，不影响临时消息）。
    QString session_state_text() const;
    bool execute_enabled() const;
    bool cancel_enabled() const;
    bool is_busy() const noexcept;

signals:
    void request_open(const QString& path);
    void request_execute(
        const QString& text,
        bool analyze_mode,
        bool plan_only,
        quint64 max_rows,
        quint64 snapshot_id,
        tinydbms::core::CancelToken cancel);
    void request_close();

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void on_lifecycle_finished(const tinydbms::gui::LifecyclePayload& payload);
    void on_script_finished(const tinydbms::gui::ExecutionPayload& payload);
    void on_editor_text_edited();
    void on_fix_requested(int statement_index);

private:
    void choose_directory();
    void refresh_schema_browser();
    void apply_script_result(const ExecutionPayload& payload);
    void update_controls();
    // 唯一的会话状态入口：改状态后立即刷新控件可用性与状态栏文字。
    void set_session_state(SessionState state);
    std::uint64_t next_snapshot_id();
    // 下拉框当前档位对应的单语句物化上限；始终是合法值（>= 1）。
    quint64 selected_max_rows() const;
    static QString error_text(const std::shared_ptr<const tinydbms::core::Error>& error);

    Backend* backend_ = nullptr;
    QThread* thread_ = nullptr;
    Worker* worker_ = nullptr;

    ScriptEditor* editor_ = nullptr;
    ResultPanel* result_panel_ = nullptr;
    DiagnosticList* diagnostics_ = nullptr;
    SchemaBrowser* schema_ = nullptr;
    QPushButton* open_button_ = nullptr;
    QPushButton* execute_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QCheckBox* analyze_box_ = nullptr;
    QCheckBox* plan_box_ = nullptr;
    QComboBox* rows_box_ = nullptr;
    QLabel* path_label_ = nullptr;
    QLabel* status_state_ = nullptr;

    SessionState state_ = SessionState::kClosed;
    QString data_dir_;
    std::optional<QString> pending_directory_;
    std::optional<std::uint64_t> schema_snapshot_;
    std::shared_ptr<const tinydbms::core::ExecuteScriptResult> last_payload_;
    std::uint64_t snapshot_counter_ = 0;
    std::uint64_t executed_snapshot_id_ = 0;
    QString executed_snapshot_;
    tinydbms::core::CancelToken active_cancel_;
    QElapsedTimer timer_;
    bool busy_ = false;
    bool cancel_pending_ = false;
    bool closing_ = false;
    bool force_close_ = false;
    // open 失败后公共契约无法区分是否留下 cleanup-pending，下一次打开前先走一次 close。
    bool cleanup_may_be_pending_ = false;
};

}  // namespace tinydbms::gui

#endif  // TINYDBMS_GUI_MAIN_WINDOW_HPP
