#ifndef TINYDBMS_GUI_CHART_WIDGET_HPP
#define TINYDBMS_GUI_CHART_WIDGET_HPP

#include <QSize>
#include <QWidget>

#include "chart_data.hpp"

namespace tinydbms::gui {

// 自绘柱状图：不引入 Qt Charts 依赖。数据为空或没有数值列时绘制明确的空状态文案，
// 不伪造图形；截断信息显式绘制在图表右下角。
class ChartWidget : public QWidget {
    Q_OBJECT

public:
    explicit ChartWidget(QWidget* parent = nullptr);

    void set_data(ChartData data);
    const ChartData& data() const noexcept;

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    ChartData data_;
};

}  // namespace tinydbms::gui

#endif  // TINYDBMS_GUI_CHART_WIDGET_HPP
