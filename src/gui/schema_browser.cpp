#include "schema_browser.hpp"

#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <cstddef>
#include <optional>

#include "result_model.hpp"

namespace tinydbms::gui {
namespace {

std::optional<std::size_t> column_index(
    const tinydbms::core::QueryResult& query,
    const char* name) {
    for (std::size_t index = 0; index < query.columns.size(); ++index) {
        if (query.columns[index].name == name) {
            return index;
        }
    }
    return std::nullopt;
}

QString cell_text(
    const tinydbms::core::QueryResult& query,
    std::size_t row,
    std::optional<std::size_t> column) {
    if (!column.has_value() || row >= query.rows.size()) {
        return QString();
    }
    const tinydbms::core::Row& values = query.rows[row];
    if (*column >= values.size()) {
        return QString();
    }
    return value_text(values[*column]);
}

}  // namespace

SchemaBrowser::SchemaBrowser(QWidget* parent) : QWidget{parent} {
    tree_ = new QTreeWidget{this};
    tree_->setColumnCount(2);
    tree_->setHeaderLabels(QStringList{QStringLiteral("对象"), QStringLiteral("类型")});
    tree_->setRootIsDecorated(true);

    refresh_button_ = new QPushButton{QStringLiteral("刷新表结构"), this};
    status_ = new QLabel{this};
    status_->setWordWrap(true);

    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(refresh_button_);
    layout->addWidget(tree_, 1);
    layout->addWidget(status_);

    connect(refresh_button_, &QPushButton::clicked, this, &SchemaBrowser::refresh_requested);
}

void SchemaBrowser::set_tables(const tinydbms::core::QueryResult& tables) {
    tree_->clear();
    table_items_.clear();
    column_count_ = 0;

    const auto id_column = column_index(tables, "table_id");
    const auto name_column = column_index(tables, "table_name");
    const auto count_column = column_index(tables, "column_count");

    for (std::size_t row = 0; row < tables.rows.size(); ++row) {
        const QString id = cell_text(tables, row, id_column);
        const QString name = cell_text(tables, row, name_column);
        const QString count = cell_text(tables, row, count_column);
        auto* item = new QTreeWidgetItem{tree_};
        item->setText(0, QStringLiteral("%1 (%2)").arg(name, id));
        item->setText(1, QStringLiteral("%1 列").arg(count));
        // 父子关系按 table_id 关联，不做文本匹配：表名本身可能以 "(数字)" 结尾。
        item->setData(0, Qt::UserRole, id);
        // 同一 table_id 重复出现时保留第一个，与旧的线性查找语义一致。
        if (!table_items_.contains(id)) {
            table_items_.insert(id, item);
        }
    }
}

void SchemaBrowser::set_columns(const tinydbms::core::QueryResult& columns) {
    const auto id_column = column_index(columns, "table_id");
    const auto ordinal_column = column_index(columns, "column_ordinal");
    const auto name_column = column_index(columns, "column_name");
    const auto type_column = column_index(columns, "column_type");

    for (std::size_t row = 0; row < columns.rows.size(); ++row) {
        const QString id = cell_text(columns, row, id_column);
        QTreeWidgetItem* parent = table_items_.value(id, nullptr);
        if (parent == nullptr) {
            continue;
        }
        auto* item = new QTreeWidgetItem{parent};
        item->setText(
            0,
            QStringLiteral("%1. %2")
                .arg(cell_text(columns, row, ordinal_column), cell_text(columns, row, name_column)));
        item->setText(1, cell_text(columns, row, type_column));
        ++column_count_;
    }
    tree_->expandAll();
    status_->clear();
}

void SchemaBrowser::set_status_text(const QString& text) {
    status_->setText(text);
}

void SchemaBrowser::set_refresh_enabled(bool enabled) {
    refresh_button_->setEnabled(enabled);
}

void SchemaBrowser::clear() {
    tree_->clear();
    table_items_.clear();
    status_->clear();
    column_count_ = 0;
}

int SchemaBrowser::table_count() const {
    return tree_->topLevelItemCount();
}

int SchemaBrowser::column_count() const {
    return column_count_;
}

QString SchemaBrowser::status_text() const {
    return status_->text();
}

bool SchemaBrowser::refresh_enabled() const {
    return refresh_button_->isEnabled();
}

}  // namespace tinydbms::gui
