#ifndef TINYDBMS_GUI_RESULT_PANEL_HPP
#define TINYDBMS_GUI_RESULT_PANEL_HPP

#include <memory>

#include <QWidget>
#include <QString>

#include "tinydbms/core.hpp"

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableView;

namespace tinydbms::gui {

struct ChartData;
class ChartWidget;
class ResultModel;

// 结果区：脚本级横幅、结果表格、逐语句摘要行。
class ResultPanel : public QWidget {
    Q_OBJECT

public:
    explicit ResultPanel(QWidget* parent = nullptr);

    // payload 会被持有：模型与视图只借用其中的 QueryResult。
    void show_result(
        const std::shared_ptr<const tinydbms::core::ExecuteScriptResult>& payload);
    // 与 SQL 无关的提示（open 失败、会话需要重开等）。
    void show_notice(const QString& text);
    void clear();

    QString banner_text() const;
    QString statement_text() const;
    ResultModel* model() const noexcept;
    ChartWidget* chart_widget() const noexcept;
    // 图表数据由当前展示的查询结果派生，规则见 chart_data.hpp。
    const ChartData& chart_data() const noexcept;

    // 导出与复制都基于当前展示的最后一条查询结果；没有结果时返回空字符串。
    QString export_csv_text() const;
    QString clipboard_text() const;
    // 写盘内容为 UTF-8 + BOM；路径不可写时返回 false。
    bool export_csv_to(const QString& path) const;

private:
    void export_csv();
    void copy_to_clipboard();
    void update_export_buttons();

    std::shared_ptr<const tinydbms::core::ExecuteScriptResult> payload_;
    QLabel* banner_ = nullptr;
    QTableView* table_ = nullptr;
    ChartWidget* chart_ = nullptr;
    QPlainTextEdit* statements_ = nullptr;
    QPushButton* export_button_ = nullptr;
    QPushButton* copy_button_ = nullptr;
    ResultModel* model_ = nullptr;
};

}  // namespace tinydbms::gui

#endif  // TINYDBMS_GUI_RESULT_PANEL_HPP
