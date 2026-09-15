#include "gui_backend_fake.hpp"

#include <chrono>
#include <thread>
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

void FakeBackend::set_script_gate(std::shared_ptr<ScriptGate> gate) {
    script_gate_ = std::move(gate);
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

bool FakeBackend::last_plan_only() const noexcept {
    return last_plan_only_;
}

std::size_t FakeBackend::last_max_rows() const noexcept {
    return last_max_rows_;
}

bool FakeBackend::last_cancel_requested() const noexcept {
    return last_cancel_requested_.load();
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
    last_plan_only_ = request.mode == tinydbms::core::ExecutionMode::kPlanOnly;
    last_max_rows_ = request.max_query_rows;
    last_cancel_requested_.store(request.cancel.cancel_requested());
    if (script_gate_ != nullptr) {
        std::shared_ptr<ScriptGate> gate = std::move(script_gate_);
        script_gate_ = nullptr;
        gate->entered.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!request.cancel.cancel_requested() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool cancelled = request.cancel.cancel_requested();
        last_cancel_requested_.store(cancelled);
        gate->observed_cancel.store(cancelled);
        if (cancelled) {
            // 模拟 core 在无副作用检查点结束：只返回 kCancelled，不携带 outcome。
            tinydbms::core::ExecuteScriptResult result;
            result.statements.push_back(tinydbms::core::StatementResult::cancelled(
                0,
                tinydbms::SourceRange{
                    tinydbms::SourceLocation{1, 1, 0},
                    tinydbms::SourceLocation{1, 1, 0}}));
            return result;
        }
    }
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
