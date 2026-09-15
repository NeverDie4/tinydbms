#include "chart_data.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <variant>

#include "result_model.hpp"

namespace tinydbms::gui {
namespace {

bool is_numeric_type(Type type) {
    return type == Type::kInt || type == Type::kBigInt || type == Type::kDouble;
}

std::optional<double> numeric_cell(const Value& value) {
    if (const auto* item = std::get_if<std::int32_t>(&value.data); item != nullptr) {
        return static_cast<double>(*item);
    }
    if (const auto* item = std::get_if<std::int64_t>(&value.data); item != nullptr) {
        return static_cast<double>(*item);
    }
    if (const auto* item = std::get_if<double>(&value.data); item != nullptr) {
        return *item;
    }
    return std::nullopt;
}

// x 轴标签复用表格的单元格渲染，避免同一份 Value 出现两套文本口径；
// 换行与制表符会破坏单行标签排版，统一折叠为空格。
QString category_label(const Value& value) {
    QString label = value_text(value);
    label.replace(QLatin1Char('\n'), QLatin1Char(' '));
    label.replace(QLatin1Char('\r'), QLatin1Char(' '));
    label.replace(QLatin1Char('\t'), QLatin1Char(' '));
    return label;
}

}  // namespace

ChartData build_chart_data(const tinydbms::core::QueryResult& query) {
    ChartData data;
    data.total_rows = query.rows.size();
    if (query.columns.empty() || query.rows.empty()) {
        return data;
    }

    const std::size_t shown_rows = std::min(query.rows.size(), kMaxChartRows);
    data.truncated = query.rows.size() > kMaxChartRows;

    std::vector<std::size_t> series_columns;
    bool first_column_is_axis = query.columns.size() > 1;
    if (first_column_is_axis) {
        for (std::size_t column = 1; column < query.columns.size(); ++column) {
            if (is_numeric_type(query.columns[column].type)) {
                series_columns.push_back(column);
            }
        }
        first_column_is_axis = !series_columns.empty();
    }
    if (!first_column_is_axis) {
        series_columns.clear();
        for (std::size_t column = 0; column < query.columns.size(); ++column) {
            if (is_numeric_type(query.columns[column].type)) {
                series_columns.push_back(column);
            }
        }
    }
    if (series_columns.empty()) {
        return data;
    }

    data.categories.reserve(shown_rows);
    for (std::size_t row = 0; row < shown_rows; ++row) {
        if (first_column_is_axis) {
            const tinydbms::core::Row& cells = query.rows[row];
            data.categories.push_back(
                cells.empty() ? QStringLiteral("(空行)") : category_label(cells[0]));
        } else {
            data.categories.push_back(QString::number(static_cast<qulonglong>(row + 1)));
        }
    }

    data.series.reserve(series_columns.size());
    for (const std::size_t column : series_columns) {
        ChartSeries series;
        series.name = QString::fromUtf8(query.columns[column].name);
        series.values.reserve(shown_rows);
        for (std::size_t row = 0; row < shown_rows; ++row) {
            const tinydbms::core::Row& cells = query.rows[row];
            series.values.push_back(
                column < cells.size() ? numeric_cell(cells[column]) : std::nullopt);
        }
        data.series.push_back(std::move(series));
    }
    return data;
}

}  // namespace tinydbms::gui
