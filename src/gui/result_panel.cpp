#include "result_panel.hpp"

#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QStringList>
#include <QTabWidget>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <variant>
#include <vector>

#include "chart_widget.hpp"
#include "result_export.hpp"
#include "result_model.hpp"
#include "session_state.hpp"

namespace tinydbms::gui {
namespace {

// 小结果自动列宽：默认宽度会让较长的值整列以省略号呈现，而测量内容宽度的成本与结果规模
// 成正比。只在行数与列数都不大时测量，大结果保持固定宽度，由用户按需拖动列宽。
constexpr std::size_t kAutoFitMaxRows = 200;
constexpr std::size_t kAutoFitMaxColumns = 16;
constexpr int kAutoFitMinWidth = 60;
constexpr int kAutoFitMaxWidth = 320;
constexpr int kFixedColumnWidth = 100;

void apply_column_widths(QTableView& table, const ResultModel& model) {
    const bool auto_fit = static_cast<std::size_t>(model.rowCount()) <= kAutoFitMaxRows &&
        static_cast<std::size_t>(model.columnCount()) <= kAutoFitMaxColumns &&
        model.columnCount() > 0;
    if (auto_fit) {
        table.resizeColumnsToContents();
    }
    for (int column = 0; column < model.columnCount(); ++column) {
        const int width = auto_fit
            ? std::clamp(table.columnWidth(column), kAutoFitMinWidth, kAutoFitMaxWidth)
            : kFixedColumnWidth;
        table.setColumnWidth(column, width);
    }
}

}  // namespace

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

    chart_ = new ChartWidget{this};

    // 导出与复制只作用于当前展示的结果；没有结果时按钮禁用。
    export_button_ = new QPushButton{QStringLiteral("导出 CSV"), this};
    copy_button_ = new QPushButton{QStringLiteral("复制"), this};
    copy_button_->setToolTip(QStringLiteral("把整份结果按制表符分隔复制到剪贴板"));
    auto* table_actions = new QHBoxLayout{};
    table_actions->setContentsMargins(0, 0, 0, 0);
    table_actions->addWidget(export_button_);
    table_actions->addWidget(copy_button_);
    table_actions->addStretch(1);

    auto* table_page = new QWidget{this};
    auto* table_layout = new QVBoxLayout{table_page};
    table_layout->setContentsMargins(0, 0, 0, 0);
    table_layout->addLayout(table_actions);
    table_layout->addWidget(table_, 1);

    // 表格是默认视图；图表是同一份结果的派生展示，两者共享 payload 生命周期。
    auto* views = new QTabWidget{this};
    views->addTab(table_page, QStringLiteral("表格"));
    views->addTab(chart_, QStringLiteral("图表"));

    connect(export_button_, &QPushButton::clicked, this, &ResultPanel::export_csv);
    connect(copy_button_, &QPushButton::clicked, this, &ResultPanel::copy_to_clipboard);
    update_export_buttons();

    statements_ = new QPlainTextEdit{this};
    statements_->setReadOnly(true);
    statements_->setLineWrapMode(QPlainTextEdit::NoWrap);

    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(banner_);
    layout->addWidget(views, 3);
    layout->addWidget(statements_, 2);
}

void ResultPanel::show_result(
    const std::shared_ptr<const tinydbms::core::ExecuteScriptResult>& payload) {
    payload_ = payload;
    statements_->clear();
    chart_->set_data(ChartData{});

    if (payload_ == nullptr) {
        model_->set_query(nullptr);
        banner_->clear();
        banner_->setVisible(false);
        update_export_buttons();
        return;
    }

    const QString banner = script_error_banner(*payload_);
    banner_->setText(banner);
    banner_->setVisible(!banner.isEmpty());

    const std::vector<StatementView> views = build_statement_views(*payload_);
    QStringList lines;
    const tinydbms::core::QueryResult* last_query = nullptr;
    for (const StatementView& view : views) {
        lines.append(view.summary);
        if (!view.detail.isEmpty()) {
            for (const QString& detail_line : view.detail.split(QLatin1Char('\n'))) {
                lines.append(QStringLiteral("    %1").arg(detail_line));
            }
        }
        if (view.query != nullptr) {
            last_query = view.query;
        }
    }
    // 多条查询时只展示最后一条：模型每次刷新只重置一次，
    // 不随语句数量反复重置视图。
    model_->set_query(last_query);
    apply_column_widths(*table_, *model_);
    chart_->set_data(last_query != nullptr ? build_chart_data(*last_query) : ChartData{});
    update_export_buttons();
    if (lines.isEmpty()) {
        // 脚本级错误（分句失败、内部错误）不会产生任何语句结果，此时说“没有可执行语句”
        // 会被读成输入问题；横幅已经给出原因，这里只说明没有语句结果。
        lines.append(
            payload_->script_error.has_value()
                ? QStringLiteral("脚本已在执行前中止，没有语句结果")
                : QStringLiteral("没有可执行语句"));
    }
    statements_->setPlainText(lines.join(QLatin1Char('\n')));
}

void ResultPanel::show_notice(const QString& text) {
    payload_.reset();
    model_->set_query(nullptr);
    chart_->set_data(ChartData{});
    table_->clearSpans();
    statements_->clear();
    banner_->setText(text);
    banner_->setVisible(!text.isEmpty());
    update_export_buttons();
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

ChartWidget* ResultPanel::chart_widget() const noexcept {
    return chart_;
}

const ChartData& ResultPanel::chart_data() const noexcept {
    return chart_->data();
}

QString ResultPanel::export_csv_text() const {
    const tinydbms::core::QueryResult* query = model_->query();
    return query != nullptr ? csv_text(*query) : QString();
}

QString ResultPanel::clipboard_text() const {
    const tinydbms::core::QueryResult* query = model_->query();
    return query != nullptr ? tsv_text(*query) : QString();
}

bool ResultPanel::export_csv_to(const QString& path) const {
    const QByteArray bytes = [&] {
        const tinydbms::core::QueryResult* query = model_->query();
        return query != nullptr ? csv_bytes(*query) : QByteArray{};
    }();
    if (bytes.isEmpty()) {
        return false;
    }
    // QSaveFile：先写临时文件，commit 成功才替换目标，写失败不会留下半份结果。
    QSaveFile file{path};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

void ResultPanel::export_csv() {
    if (model_->query() == nullptr || model_->query()->columns.empty()) {
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出 CSV"),
        QStringLiteral("result.csv"),
        QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty()) {
        return;
    }
    if (!export_csv_to(path)) {
        QMessageBox::warning(
            this,
            QStringLiteral("导出失败"),
            QStringLiteral("无法写入文件：%1").arg(path));
    }
}

void ResultPanel::copy_to_clipboard() {
    const QString text = clipboard_text();
    if (text.isEmpty()) {
        return;
    }
    QApplication::clipboard()->setText(text);
}

void ResultPanel::update_export_buttons() {
    const tinydbms::core::QueryResult* query = model_->query();
    const bool available = query != nullptr && !query->columns.empty();
    export_button_->setEnabled(available);
    copy_button_->setEnabled(available);
}

}  // namespace tinydbms::gui
