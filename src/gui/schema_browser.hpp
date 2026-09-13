#ifndef TINYDBMS_GUI_SCHEMA_BROWSER_HPP
#define TINYDBMS_GUI_SCHEMA_BROWSER_HPP

#include <QWidget>
#include <QString>

#include "tinydbms/core.hpp"

class QLabel;
class QTreeWidget;

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
    void clear();

    int table_count() const;
    int column_count() const;
    QString status_text() const;

signals:
    void refresh_requested();

private:
    QTreeWidget* tree_ = nullptr;
    QLabel* status_ = nullptr;
    int column_count_ = 0;
};

}  // namespace tinydbms::gui

#endif  // TINYDBMS_GUI_SCHEMA_BROWSER_HPP
