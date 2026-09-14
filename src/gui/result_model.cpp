#include "result_model.hpp"

#include <QString>

#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>

namespace tinydbms::gui {

namespace {

// 与 CLI 的输出保持一致：DOUBLE 使用最短往返表示，整数形态补 ".0"，
// 不使用 QString::number 的默认 6 位有效数字（会丢精度）。
QString double_text(double value) {
    std::array<char, 64> buffer{};
    const auto [end, error] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc{}) {
        return QStringLiteral("<unsupported DOUBLE value>");
    }
    std::string text(buffer.data(), end);
    if (text.find_first_of(".eE") == std::string::npos) {
        text += ".0";
    }
    return QString::fromLatin1(text.c_str(), static_cast<int>(text.size()));
}

}  // namespace

QString value_text(const tinydbms::Value& value) {
    return std::visit(
        [](const auto& item) -> QString {
            using Item = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Item, std::monostate>) {
                return QStringLiteral("NULL");
            } else if constexpr (std::is_same_v<Item, std::int32_t>) {
                return QString::number(item);
            } else if constexpr (std::is_same_v<Item, std::int64_t>) {
                return QString::number(static_cast<qlonglong>(item));
            } else if constexpr (std::is_same_v<Item, double>) {
                return double_text(item);
            } else if constexpr (std::is_same_v<Item, bool>) {
                return item ? QStringLiteral("TRUE") : QStringLiteral("FALSE");
            } else {
                return QString::fromUtf8(item);
            }
        },
        value.data);
}

ResultModel::ResultModel(QObject* parent) : QAbstractTableModel{parent} {}

void ResultModel::set_query(const tinydbms::core::QueryResult* query) {
    beginResetModel();
    query_ = query;
    endResetModel();
}

const tinydbms::core::QueryResult* ResultModel::query() const noexcept {
    return query_;
}

int ResultModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid() || query_ == nullptr) {
        return 0;
    }
    return static_cast<int>(query_->rows.size());
}

int ResultModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid() || query_ == nullptr) {
        return 0;
    }
    return static_cast<int>(query_->columns.size());
}

QVariant ResultModel::data(const QModelIndex& index, int role) const {
    if (query_ == nullptr || !index.isValid()) {
        return QVariant{};
    }
    const auto row = static_cast<std::size_t>(index.row());
    const auto column = static_cast<std::size_t>(index.column());
    if (row >= query_->rows.size() || column >= query_->rows[row].size()) {
        return QVariant{};
    }
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
        return value_text(query_->rows[row][column]);
    }
    return QVariant{};
}

QVariant ResultModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole || query_ == nullptr) {
        return QVariant{};
    }
    const auto index = static_cast<std::size_t>(section);
    if (index >= query_->columns.size()) {
        return QVariant{};
    }
    return QString::fromUtf8(query_->columns[index].name);
}

}  // namespace tinydbms::gui
