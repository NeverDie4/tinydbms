#ifndef TINYDBMS_GUI_EXECUTION_PAYLOAD_HPP
#define TINYDBMS_GUI_EXECUTION_PAYLOAD_HPP

#include <QMetaType>

#include <cstdint>
#include <memory>
#include <string>

#include "tinydbms/core.hpp"

namespace tinydbms::gui {

// 队列连接会复制信号参数，因此不直接传 core 的大对象：
// 用共享指针持有结果，并携带编辑器快照版本号，保证结果只作用于匹配的文本。
struct ExecutionPayload {
    std::uint64_t snapshot_id = 0;
    std::shared_ptr<const tinydbms::core::ExecuteScriptResult> result;
};

// open / close 的结果同样走队列信号，避免跨线程直接触碰界面状态。
enum class LifecycleAction {
    kOpened,
    kOpenFailed,
    kClosed,
    kCloseFailed
};

struct LifecyclePayload {
    LifecycleAction action = LifecycleAction::kClosed;
    std::string data_dir;                                // kOpened / kOpenFailed 时有效
    std::shared_ptr<const tinydbms::core::Error> error;  // 失败时非空
};

}  // namespace tinydbms::gui

Q_DECLARE_METATYPE(tinydbms::gui::ExecutionPayload)
Q_DECLARE_METATYPE(tinydbms::gui::LifecycleAction)
Q_DECLARE_METATYPE(tinydbms::gui::LifecyclePayload)

#endif  // TINYDBMS_GUI_EXECUTION_PAYLOAD_HPP
