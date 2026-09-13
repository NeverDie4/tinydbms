#include "result_model.hpp"

#include <QString>

#include <cstdint>
#include <variant>

namespace tinydbms::gui {

QString value_text(const tinydbms::Value& value) {
    if (const auto* number = std::get_if<std::int32_t>(&value.data); number != nullptr) {
        return QString::number(*number);
    }
    return QString::fromUtf8(std::get<std::string>(value.data));
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
