#ifndef TINYDBMS_GUI_DIAGNOSTIC_LIST_HPP
#define TINYDBMS_GUI_DIAGNOSTIC_LIST_HPP

#include <optional>
#include <vector>

#include <QWidget>

#include "tinydbms/diagnostic.hpp"

class QPushButton;
class QTableWidget;

namespace tinydbms::gui {

struct StatementView;

// 诊断列表：按语句顺序展示阶段、范围、message 与 suggestion，
// 选中带 fix-it 的行时可以请求把修复应用到编辑器。
class DiagnosticList : public QWidget {
    Q_OBJECT

public:
    explicit DiagnosticList(QWidget* parent = nullptr);

    // 只保留带诊断（detail 非空）的语句。
    void set_views(const std::vector<StatementView>& views);
    void clear();
    void set_fix_enabled(bool enabled);

    int row_count() const;
    QString row_stage(int row) const;
    QString row_text(int row) const;
    bool row_has_fix(int row) const;
    bool fix_button_enabled() const;

    // 与「应用修复」按钮同一条路径；测试用它驱动同一次选中语义。
    void activate_fix(int row);

signals:
    void fix_requested(int statement_index);

private:
    struct Row {
        int statement_index = 0;
        std::optional<tinydbms::FixIt> fix_it;
        QString stage;
        QString range_text;
        QString detail;
    };

    void update_button_state();

    QTableWidget* table_ = nullptr;
    QPushButton* fix_button_ = nullptr;
    std::vector<Row> rows_;
    bool fix_enabled_ = false;
};

}  // namespace tinydbms::gui

#endif  // TINYDBMS_GUI_DIAGNOSTIC_LIST_HPP
