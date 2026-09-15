#ifndef TINYDBMS_GUI_CHART_DATA_HPP
#define TINYDBMS_GUI_CHART_DATA_HPP

#include <cstddef>
#include <optional>
#include <vector>

#include <QString>

#include "tinydbms/core.hpp"

namespace tinydbms::gui {

// 图表是纯展示层：行数超过上限时按行截断，并通过 truncated/total_rows 显式提示，
// 不静默改变数据语义；表格视图始终展示完整结果。
inline constexpr std::size_t kMaxChartRows = 200;

struct ChartSeries {
    QString name;
    // 与 ChartData::categories 等长；nullopt 表示该单元格不是数值（例如 SQL NULL）。
    std::vector<std::optional<double>> values;
};

struct ChartData {
    std::vector<QString> categories;
    std::vector<ChartSeries> series;
    std::size_t total_rows = 0;
    bool truncated = false;

    bool empty() const noexcept { return categories.empty() || series.empty(); }
};

// 提取规则（按列类型判断，不按单元格文本猜测）：
// - 多列且首列之后存在数值列（INT/BIGINT/DOUBLE）：首列作 x 轴标签，其余数值列作系列；
// - 否则退化为行号作 x 轴，全部数值列作系列；
// - 没有任何数值列时返回空 ChartData；
// - BOOLEAN 与 VARCHAR 不作数值列，只可能作为 x 轴标签。
ChartData build_chart_data(const tinydbms::core::QueryResult& query);

}  // namespace tinydbms::gui

#endif  // TINYDBMS_GUI_CHART_DATA_HPP
