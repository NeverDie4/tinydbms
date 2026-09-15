#ifndef TINYDBMS_TESTS_GUI_BACKEND_FAKE_HPP
#define TINYDBMS_TESTS_GUI_BACKEND_FAKE_HPP

#include <deque>
#include <string>
#include <vector>

#include "backend.hpp"

namespace tinydbms::testing::gui_fake {

// GUI 测试用的假后端：预置每次 open / execute_script / close 的返回值，
// 并记录调用顺序，使 UI 测试不依赖真实 compiler、storage 或数据目录。
class FakeBackend final : public tinydbms::gui::Backend {
public:
    void queue_open(tinydbms::core::OpenDatabaseResult result);
    void queue_close(tinydbms::core::CloseDatabaseResult result);
    void queue_script(tinydbms::core::ExecuteScriptResult result);

    int open_calls() const noexcept;
    int close_calls() const noexcept;
    int execute_calls() const noexcept;
    const std::vector<std::string>& opened_dirs() const noexcept;
    const std::string& last_script_text() const noexcept;
    bool last_analyze_mode() const noexcept;

    tinydbms::core::OpenDatabaseResult open(
        const tinydbms::core::OpenDatabaseRequest& request) override;
    tinydbms::core::ExecuteScriptResult execute_script(
        const tinydbms::core::ExecuteScriptRequest& request) override;
    tinydbms::core::CloseDatabaseResult close() override;

private:
    std::deque<tinydbms::core::OpenDatabaseResult> opens_;
    std::deque<tinydbms::core::CloseDatabaseResult> closes_;
    std::deque<tinydbms::core::ExecuteScriptResult> scripts_;
    std::vector<std::string> opened_dirs_;
    std::string last_script_text_;
    bool last_analyze_mode_ = false;
    int open_calls_ = 0;
    int close_calls_ = 0;
    int execute_calls_ = 0;
};

}  // namespace tinydbms::testing::gui_fake

#endif  // TINYDBMS_TESTS_GUI_BACKEND_FAKE_HPP
