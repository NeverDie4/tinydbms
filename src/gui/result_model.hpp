#ifndef TINYDBMS_GUI_RESULT_MODEL_HPP
#define TINYDBMS_GUI_RESULT_MODEL_HPP

#include <QAbstractTableModel>

#include "tinydbms/core.hpp"

namespace tinydbms::gui {

// 只读结果表格模型：Value 到显示文本的转换发生在 data() 中，
// 因此大结果集只转换当前可见行，不预生成整表字符串。
class ResultModel : public QAbstractTableModel {
    Q_OBJECT

public:
    explicit ResultModel(QObject* parent = nullptr);

    // 非拥有指针；调用方（ResultPanel）保证结果在模型使用期间存活。
    void set_query(const tinydbms::core::QueryResult* query);
    const tinydbms::core::QueryResult* query() const noexcept;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(
        int section,
        Qt::Orientation orientation,
        int role = Qt::DisplayRole) const override;

private:
    const tinydbms::core::QueryResult* query_ = nullptr;
};

// 单元格文本：按变体分支渲染，不按字符串内容猜测类型。
QString value_text(const tinydbms::Value& value);

}  // namespace tinydbms::gui

#endif  // TINYDBMS_GUI_RESULT_MODEL_HPP
