#ifndef TINYDBMS_GUI_CLOSE_POLICY_HPP
#define TINYDBMS_GUI_CLOSE_POLICY_HPP

#include "tinydbms/core.hpp"

namespace tinydbms::gui {

// CoreBackend 的关闭语义（纯判定，便于在无 core 的环境下测试）：
//
// - 上一次 open 成功过：close 的结果原样上抛，失败（kStorage / kInternal）就是真实关闭失败；
// - 上一次 open 从未成功：close 只是 cleanup 重试。core 在"没有需要清理的生命周期"时返回
//   kExecute（database is not open），这表示无事可做，不算失败；真正的清理失败仍然是
//   kStorage / kInternal，必须上抛。
//
// 这样 GUI 在 open 失败后可以无条件先 close 再 open，而不需要靠错误文本判断。
inline bool close_succeeded(
    bool open_reported,
    const tinydbms::core::CloseDatabaseResult& result) {
    if (!result.error.has_value()) {
        return true;
    }
    if (open_reported) {
        return false;
    }
    return result.error->kind == tinydbms::core::ErrorKind::kExecute;
}

}  // namespace tinydbms::gui

#endif  // TINYDBMS_GUI_CLOSE_POLICY_HPP
