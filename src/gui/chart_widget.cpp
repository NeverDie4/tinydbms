#include "chart_widget.hpp"

#include <QColor>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QRectF>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace tinydbms::gui {
namespace {

constexpr int kIntervalCount = 4;

const std::array<QColor, 6>& chart_palette() {
    static const std::array<QColor, 6> colors{
        QColor{47, 111, 237},
        QColor{232, 113, 60},
        QColor{42, 157, 143},
        QColor{156, 91, 214},
        QColor{214, 69, 92},
        QColor{107, 114, 128}};
    return colors;
}

}  // namespace

QString axis_label_text(double value, double step) {
    if (!std::isfinite(value)) {
        return QString();
    }
    if (!std::isfinite(step) || step <= 0.0) {
        return QString::number(value, 'g', 6);
    }
    // 步长 25.4 → 1 位小数，2.54 → 2 位，0.254 → 3 位；上限 3 位避免排不下。
    const int magnitude = static_cast<int>(std::floor(std::log10(step)));
    const int decimals = std::clamp(2 - magnitude, 0, 3);
    return QString::number(value, 'f', decimals);
}

ChartWidget::ChartWidget(QWidget* parent) : QWidget{parent} {
    setMinimumHeight(180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(true);
}

void ChartWidget::set_data(ChartData data) {
    data_ = std::move(data);
    update();
}

const ChartData& ChartWidget::data() const noexcept {
    return data_;
}

QSize ChartWidget::sizeHint() const {
    return QSize{520, 280};
}

void ChartWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QWidget::palette().base());

    const QRectF full{0.0, 0.0, static_cast<qreal>(width()), static_cast<qreal>(height())};
    if (data_.empty()) {
        painter.setPen(QWidget::palette().color(QPalette::PlaceholderText));
        const QString message = data_.total_rows == 0
            ? QStringLiteral("结果为空，暂无可绘制的数据")
            : QStringLiteral("没有可绘制的数值列");
        painter.drawText(full, static_cast<int>(Qt::AlignCenter), message);
        return;
    }

    const int top_margin = 26;
    const int left_margin = 64;
    const int right_margin = 16;
    const int bottom_margin = data_.truncated ? 44 : 28;
    const QRectF plot{
        static_cast<qreal>(left_margin),
        static_cast<qreal>(top_margin),
        std::max<qreal>(1.0, full.width() - left_margin - right_margin),
        std::max<qreal>(1.0, full.height() - top_margin - bottom_margin)};

    double data_min = std::numeric_limits<double>::infinity();
    double data_max = -std::numeric_limits<double>::infinity();
    for (const ChartSeries& series : data_.series) {
        for (const std::optional<double>& value : series.values) {
            if (!value.has_value()) {
                continue;
            }
            data_min = std::min(data_min, *value);
            data_max = std::max(data_max, *value);
        }
    }
    if (!std::isfinite(data_min) || !std::isfinite(data_max)) {
        painter.setPen(QWidget::palette().color(QPalette::PlaceholderText));
        painter.drawText(full, static_cast<int>(Qt::AlignCenter), QStringLiteral("没有可绘制的数值"));
        return;
    }
    if (data_min > 0.0) {
        data_min = 0.0;
    }
    if (data_max < 0.0) {
        data_max = 0.0;
    }
    if (data_min == data_max) {
        data_min -= 1.0;
        data_max += 1.0;
    } else {
        // 只向远离 0 基线的一侧留白，避免全正数据出现负向刻度。
        const double padding = (data_max - data_min) * 0.05;
        if (data_max > 0.0) {
            data_max += padding;
        }
        if (data_min < 0.0) {
            data_min -= padding;
        }
    }

    const auto y_for = [&](double value) {
        const double ratio = (data_max - value) / (data_max - data_min);
        return plot.top() + plot.height() * ratio;
    };

    // 网格与 y 轴刻度
    const QColor grid_color = QWidget::palette().color(QPalette::Midlight);
    const QColor axis_color = QWidget::palette().color(QPalette::Dark);
    const QFontMetrics metrics{painter.font()};
    const double tick_step = (data_max - data_min) / kIntervalCount;
    for (int tick = 0; tick <= kIntervalCount; ++tick) {
        const double ratio = static_cast<double>(tick) / kIntervalCount;
        const double value = data_max - (data_max - data_min) * ratio;
        const qreal y = plot.top() + plot.height() * ratio;
        painter.setPen(grid_color);
        painter.drawLine(QPointF{plot.left(), y}, QPointF{plot.right(), y});
        painter.setPen(QWidget::palette().color(QPalette::Text));
        const QRectF label_rect{
            full.left() + 4.0,
            y - static_cast<qreal>(metrics.height()) / 2.0,
            static_cast<qreal>(left_margin) - 10.0,
            static_cast<qreal>(metrics.height())};
        painter.drawText(
            label_rect,
            static_cast<int>(Qt::AlignRight | Qt::AlignVCenter),
            axis_label_text(value, tick_step));
    }
    painter.setPen(axis_color);
    painter.drawLine(plot.bottomLeft(), plot.bottomRight());
    painter.drawLine(plot.topLeft(), plot.bottomLeft());

    // 柱状系列
    const std::size_t category_count = data_.categories.size();
    const std::size_t series_count = data_.series.size();
    const double group_width = plot.width() / static_cast<double>(category_count);
    const double bar_width = std::max(1.0, group_width * 0.8 / static_cast<double>(series_count));
    const qreal zero_y = y_for(0.0);
    for (std::size_t category = 0; category < category_count; ++category) {
        const double group_left =
            plot.left() + group_width * static_cast<double>(category) + group_width * 0.1;
        for (std::size_t index = 0; index < series_count; ++index) {
            const std::vector<std::optional<double>>& values = data_.series[index].values;
            // ChartData 是公开结构：绘图不假设调用方一定按 categories 对齐 values。
            if (category >= values.size()) {
                continue;
            }
            const std::optional<double>& value = values[category];
            if (!value.has_value()) {
                continue;
            }
            const qreal value_y = y_for(*value);
            const QRectF bar{
                group_left + bar_width * static_cast<double>(index),
                std::min(zero_y, value_y),
                std::max(1.0, bar_width - 1.0),
                std::max<qreal>(1.0, std::abs(value_y - zero_y))};
            painter.fillRect(
                bar,
                chart_palette()[index % chart_palette().size()]);
        }
    }

    // x 轴标签：数量多时按步长抽稀，避免文字重叠
    const std::size_t label_step =
        category_count <= 12 ? 1 : (category_count + 11) / 12;
    painter.setPen(QWidget::palette().color(QPalette::Text));
    for (std::size_t category = 0; category < category_count; category += label_step) {
        const double label_width = group_width * static_cast<double>(
            std::min(label_step, category_count - category));
        const QRectF label_rect{
            plot.left() + group_width * static_cast<double>(category),
            plot.bottom() + 4.0,
            label_width,
            static_cast<qreal>(metrics.height())};
        const QString text = metrics.elidedText(
            data_.categories[category],
            Qt::ElideRight,
            static_cast<int>(std::max<qreal>(0.0, label_width - 4.0)));
        painter.drawText(label_rect, static_cast<int>(Qt::AlignHCenter), text);
    }

    // 图例
    qreal legend_x = plot.left();
    const qreal legend_y = full.top() + 6.0;
    for (std::size_t index = 0; index < series_count; ++index) {
        const QString label = data_.series[index].name;
        const int text_width = metrics.horizontalAdvance(label);
        const qreal entry_width = static_cast<qreal>(text_width) + 24.0;
        if (legend_x + entry_width > plot.right()) {
            break;
        }
        painter.fillRect(
            QRectF{legend_x, legend_y + 3.0, 10.0, 10.0},
            chart_palette()[index % chart_palette().size()]);
        painter.setPen(QWidget::palette().color(QPalette::Text));
        painter.drawText(
            QRectF{legend_x + 14.0, legend_y, static_cast<qreal>(text_width), 16.0},
            static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter),
            label);
        legend_x += entry_width;
    }

    if (data_.truncated) {
        painter.setPen(QWidget::palette().color(QPalette::PlaceholderText));
        painter.drawText(
            QRectF{plot.left(), full.bottom() - 20.0, plot.width(), 16.0},
            static_cast<int>(Qt::AlignRight | Qt::AlignVCenter),
            QStringLiteral("仅绘制前 %1 行，共 %2 行（表格视图展示完整结果）")
                .arg(static_cast<qulonglong>(kMaxChartRows))
                .arg(static_cast<qulonglong>(data_.total_rows)));
    }
}

}  // namespace tinydbms::gui
