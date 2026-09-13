#ifndef TINYDBMS_GUI_WORKER_HPP
#define TINYDBMS_GUI_WORKER_HPP

#include <QObject>
#include <QString>

#include <cstdint>

#include "execution_payload.hpp"

namespace tinydbms::gui {

class Backend;

// 工作线程对象：Backend 的调用全部发生在这里，UI 线程只投递请求与接收结果。
class Worker : public QObject {
    Q_OBJECT

public:
    explicit Worker(Backend* backend, QObject* parent = nullptr);

public slots:
    void open_database(const QString& data_dir);
    void execute_script(const QString& text, bool analyze_mode, quint64 snapshot_id);
    void close_database();

signals:
    void lifecycle_finished(const tinydbms::gui::LifecyclePayload& payload);
    void script_finished(const tinydbms::gui::ExecutionPayload& payload);

private:
    Backend* backend_ = nullptr;
};

}  // namespace tinydbms::gui

#endif  // TINYDBMS_GUI_WORKER_HPP
