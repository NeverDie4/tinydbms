#include "diagnostic_list.hpp"

#include <QHeaderView>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "session_state.hpp"

namespace tinydbms::gui {

DiagnosticList::DiagnosticList(QWidget* parent) : QWidget{parent} {
    table_ = new QTableWidget{0, 4, this};
    table_->setHorizontalHeaderLabels(
        QStringList{QStringLiteral("语句"), QStringLiteral("阶段"),
                    QStringLiteral("位置"), QStringLiteral("说明")});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);

    fix_button_ = new QPushButton{QStringLiteral("应用修复"), this};
    fix_button_->setEnabled(false);

    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(table_, 1);
    layout->addWidget(fix_button_);

    connect(table_, &QTableWidget::itemSelectionChanged, this, [this] { update_button_state(); });
    connect(fix_button_, &QPushButton::clicked, this, [this] {
        activate_fix(table_->currentRow());
    });
}

void DiagnosticList::set_views(const std::vector<StatementView>& views) {
    rows_.clear();
    for (const StatementView& view : views) {
        if (view.detail.isEmpty()) {
            continue;
        }
        Row row;
        row.statement_index = static_cast<int>(view.index);
        row.fix_it = view.fix_it;
        row.stage = view.stage;
        row.range_text = view.range.has_value() ? format_range(*view.range) : QString();
        row.detail = view.detail;
        rows_.push_back(std::move(row));
    }

    table_->setRowCount(static_cast<int>(rows_.size()));
    for (int row = 0; row < static_cast<int>(rows_.size()); ++row) {
        const Row& entry = rows_[static_cast<std::size_t>(row)];
        const QStringList cells{
            QString::number(entry.statement_index + 1),
            entry.stage,
            entry.range_text,
            entry.detail.split(QLatin1Char('\n')).join(QStringLiteral(" "))};
        for (int column = 0; column < 4; ++column) {
            auto* item = new QTableWidgetItem{cells[column]};
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            table_->setItem(row, column, item);
        }
    }
    if (!rows_.empty()) {
        table_->selectRow(0);
    }
    update_button_state();
}

void DiagnosticList::clear() {
    rows_.clear();
    table_->setRowCount(0);
    update_button_state();
}

void DiagnosticList::set_fix_enabled(bool enabled) {
    fix_enabled_ = enabled;
    update_button_state();
}

int DiagnosticList::row_count() const {
    return static_cast<int>(rows_.size());
}

QString DiagnosticList::row_stage(int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) {
        return QString();
    }
    return rows_[static_cast<std::size_t>(row)].stage;
}

QString DiagnosticList::row_text(int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) {
        return QString();
    }
    return rows_[static_cast<std::size_t>(row)].detail;
}

bool DiagnosticList::row_has_fix(int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) {
        return false;
    }
    return rows_[static_cast<std::size_t>(row)].fix_it.has_value();
}

bool DiagnosticList::fix_button_enabled() const {
    return fix_button_->isEnabled();
}

void DiagnosticList::activate_fix(int row) {
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size()) {
        return;
    }
    const Row& entry = rows_[static_cast<std::size_t>(row)];
    if (!entry.fix_it.has_value() || !fix_enabled_) {
        return;
    }
    emit fix_requested(entry.statement_index);
}

void DiagnosticList::update_button_state() {
    const int row = table_->currentRow();
    const bool has_fix = row >= 0 && static_cast<std::size_t>(row) < rows_.size() &&
        rows_[static_cast<std::size_t>(row)].fix_it.has_value();
    fix_button_->setEnabled(has_fix && fix_enabled_);
}

}  // namespace tinydbms::gui
