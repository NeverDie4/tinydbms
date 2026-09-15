#include <QApplication>

#include <memory>

#include "backend.hpp"
#include "execution_payload.hpp"
#include "main_window.hpp"

int main(int argc, char* argv[]) {
    QApplication application{argc, argv};
    QApplication::setApplicationName(QStringLiteral("tinydbms"));
    QApplication::setApplicationVersion(QStringLiteral(TINYDBMS_VERSION));

    // 跨线程信号参数必须注册；Qt 类型之外只有 GUI 私有的两个 payload。
    qRegisterMetaType<tinydbms::gui::ExecutionPayload>();
    qRegisterMetaType<tinydbms::gui::LifecyclePayload>();
    qRegisterMetaType<tinydbms::gui::LifecycleAction>();
    qRegisterMetaType<tinydbms::core::CancelToken>();

    const std::unique_ptr<tinydbms::gui::Backend> backend = tinydbms::gui::make_backend();
    tinydbms::gui::MainWindow window{*backend};
    window.show();
    return QApplication::exec();
}
