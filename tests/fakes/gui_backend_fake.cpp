#include "gui_backend_fake.hpp"

#include <utility>

namespace tinydbms::testing::gui_fake {

void FakeBackend::queue_open(tinydbms::core::OpenDatabaseResult result) {
    opens_.push_back(std::move(result));
}

void FakeBackend::queue_close(tinydbms::core::CloseDatabaseResult result) {
    closes_.push_back(std::move(result));
}

void FakeBackend::queue_script(tinydbms::core::ExecuteScriptResult result) {
    scripts_.push_back(std::move(result));
}

int FakeBackend::open_calls() const noexcept {
    return open_calls_;
}

int FakeBackend::close_calls() const noexcept {
    return close_calls_;
}

int FakeBackend::execute_calls() const noexcept {
    return execute_calls_;
}

const std::vector<std::string>& FakeBackend::opened_dirs() const noexcept {
    return opened_dirs_;
}

const std::string& FakeBackend::last_script_text() const noexcept {
    return last_script_text_;
}

bool FakeBackend::last_analyze_mode() const noexcept {
    return last_analyze_mode_;
}

tinydbms::core::OpenDatabaseResult FakeBackend::open(
    const tinydbms::core::OpenDatabaseRequest& request) {
    ++open_calls_;
    opened_dirs_.push_back(request.data_dir);
    if (opens_.empty()) {
        return tinydbms::core::OpenDatabaseResult{std::nullopt};
    }
    tinydbms::core::OpenDatabaseResult result = std::move(opens_.front());
    opens_.pop_front();
    return result;
}

tinydbms::core::ExecuteScriptResult FakeBackend::execute_script(
    const tinydbms::core::ExecuteScriptRequest& request) {
    ++execute_calls_;
    last_script_text_ = request.text;
    last_analyze_mode_ = request.error_policy == tinydbms::core::ScriptErrorPolicy::kAnalyzeRemaining;
    if (scripts_.empty()) {
        return tinydbms::core::ExecuteScriptResult{};
    }
    tinydbms::core::ExecuteScriptResult result = std::move(scripts_.front());
    scripts_.pop_front();
    return result;
}

tinydbms::core::CloseDatabaseResult FakeBackend::close() {
    ++close_calls_;
    if (closes_.empty()) {
        return tinydbms::core::CloseDatabaseResult{std::nullopt};
    }
    tinydbms::core::CloseDatabaseResult result = std::move(closes_.front());
    closes_.pop_front();
    return result;
}

}  // namespace tinydbms::testing::gui_fake
