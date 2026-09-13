#include "result_panel.hpp"

#include <QLabel>
#include <QPlainTextEdit>
#include <QStringList>
#include <QTableView>
#include <QVBoxLayout>

#include <variant>
#include <vector>

#include "result_model.hpp"
#include "session_state.hpp"

namespace tinydbms::gui {

ResultPanel::ResultPanel(QWidget* parent) : QWidget{parent} {
    banner_ = new QLabel{this};
    banner_->setWordWrap(true);
    banner_->setVisible(false);

    table_ = new QTableView{this};
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(true);

    model_ = new ResultModel{table_};
    table_->setModel(model_);

    statements_ = new QPlainTextEdit{this};
    statements_->setReadOnly(true);
    statements_->setLineWrapMode(QPlainTextEdit::NoWrap);

    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(banner_);
    layout->addWidget(table_, 3);
    layout->addWidget(statements_, 2);
}

void ResultPanel::show_result(
    const std::shared_ptr<const tinydbms::core::ExecuteScriptResult>& payload) {
    payload_ = payload;
    model_->set_query(nullptr);
    statements_->clear();

    if (payload_ == nullptr) {
        banner_->clear();
        banner_->setVisible(false);
        return;
    }

    const QString banner = script_error_banner(*payload_);
    banner_->setText(banner);
    banner_->setVisible(!banner.isEmpty());

    const std::vector<StatementView> views = build_statement_views(*payload_);
    QStringList lines;
    for (const StatementView& view : views) {
        lines.append(view.summary);
        if (!view.detail.isEmpty()) {
            for (const QString& detail_line : view.detail.split(QLatin1Char('\n'))) {
                lines.append(QStringLiteral("    %1").arg(detail_line));
            }
        }
        if (view.query != nullptr) {
            model_->set_query(view.query);
        }
    }
    if (lines.isEmpty()) {
        lines.append(QStringLiteral("没有可执行语句"));
    }
    statements_->setPlainText(lines.join(QLatin1Char('\n')));
}

void ResultPanel::show_notice(const QString& text) {
    payload_.reset();
    model_->set_query(nullptr);
    table_->clearSpans();
    statements_->clear();
    banner_->setText(text);
    banner_->setVisible(!text.isEmpty());
}

void ResultPanel::clear() {
    show_notice(QString());
}

QString ResultPanel::banner_text() const {
    return banner_->text();
}

QString ResultPanel::statement_text() const {
    return statements_->toPlainText();
}

ResultModel* ResultPanel::model() const noexcept {
    return model_;
}

}  // namespace tinydbms::gui
