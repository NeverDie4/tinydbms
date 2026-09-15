#ifndef TINYDBMS_GUI_SCHEMA_BROWSER_HPP
#define TINYDBMS_GUI_SCHEMA_BROWSER_HPP

#include <QHash>
#include <QWidget>
#include <QString>

#include "tinydbms/core.hpp"

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace tinydbms::gui {

// 表浏览器：通过只读 SELECT 读取系统表 tdb_sys_tables / tdb_sys_columns，
// 不直接访问 storage，也不缓存 schema 作为权威数据。
class SchemaBrowser : public QWidget {
    Q_OBJECT

public:
    explicit SchemaBrowser(QWidget* parent = nullptr);

    void set_tables(const tinydbms::core::QueryResult& tables);
    void set_columns(const tinydbms::core::QueryResult& columns);
    void set_status_text(const QString& text);
    void set_refresh_enabled(bool enabled);
    void clear();

    int table_count() const;
    int column_count() const;
    QString status_text() const;
    bool refresh_enabled() const;

signals:
    void refresh_requested();

private:
    QPushButton* refresh_button_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* status_ = nullptr;
    // 表行按 table_id 文本索引到顶层节点：列行按 table_id 挂到父节点，
    // 避免每个列行都线性扫描整棵树。
    QHash<QString, QTreeWidgetItem*> table_items_;
    int column_count_ = 0;
};

}  // namespace tinydbms::gui

#endif  // TINYDBMS_GUI_SCHEMA_BROWSER_HPP
