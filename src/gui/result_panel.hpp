#ifndef TINYDBMS_GUI_RESULT_PANEL_HPP
#define TINYDBMS_GUI_RESULT_PANEL_HPP

#include <memory>

#include <QWidget>
#include <QString>

#include "tinydbms/core.hpp"

class QLabel;
class QPlainTextEdit;
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

private:
    std::shared_ptr<const tinydbms::core::ExecuteScriptResult> payload_;
    QLabel* banner_ = nullptr;
    QTableView* table_ = nullptr;
    ChartWidget* chart_ = nullptr;
    QPlainTextEdit* statements_ = nullptr;
    ResultModel* model_ = nullptr;
};

}  // namespace tinydbms::gui

#endif  // TINYDBMS_GUI_RESULT_PANEL_HPP
