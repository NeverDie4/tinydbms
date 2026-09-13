#ifndef TINYDBMS_GUI_BACKEND_HPP
#define TINYDBMS_GUI_BACKEND_HPP

#include <memory>

#include "tinydbms/core.hpp"

namespace tinydbms::gui {

// GUI 私有后端接口：三个方法与 core 的公开入口一一对应，不引入额外概念。
// 真实实现见 core_backend.cpp，未接入 compiler/storage 时见 unavailable_backend.cpp。
class Backend {
public:
    virtual ~Backend() = default;

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    Backend(Backend&&) = delete;
    Backend& operator=(Backend&&) = delete;

    virtual tinydbms::core::OpenDatabaseResult open(
        const tinydbms::core::OpenDatabaseRequest& request) = 0;

    virtual tinydbms::core::ExecuteScriptResult execute_script(
        const tinydbms::core::ExecuteScriptRequest& request) = 0;

    virtual tinydbms::core::CloseDatabaseResult close() = 0;

protected:
    Backend() = default;
};

// 由 core_backend.cpp 或 unavailable_backend.cpp 提供，main 只依赖这个工厂。
std::unique_ptr<Backend> make_backend();

}  // namespace tinydbms::gui

#endif  // TINYDBMS_GUI_BACKEND_HPP
