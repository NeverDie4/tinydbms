#ifndef TINYDBMS_GUI_CHART_WIDGET_HPP
#define TINYDBMS_GUI_CHART_WIDGET_HPP

#include <QSize>
#include <QWidget>

#include "chart_data.hpp"

namespace tinydbms::gui {

// y 轴刻度文本：小数位数由刻度步长决定，避免出现 101.588 / 76.1906 这类噪声标签。
// 步长非法（<= 0 或非有限值）时退回通用格式，保证绘制路径始终拿得到文本。
QString axis_label_text(double value, double step);

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
