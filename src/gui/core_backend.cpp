#include "backend.hpp"

#include <memory>

#include "close_policy.hpp"

namespace tinydbms::gui {
namespace {

// 唯一直接调用 core 的 GUI 文件：Database 的构造发生在工作线程的首次 open()。
class CoreBackend final : public Backend {
public:
    tinydbms::core::OpenDatabaseResult open(
        const tinydbms::core::OpenDatabaseRequest& request) override {
        if (database_ == nullptr) {
            database_ = std::make_unique<tinydbms::core::Database>();
        }
        tinydbms::core::OpenDatabaseResult result = database_->open(request);
        open_reported_ = !result.error.has_value();
        return result;
    }

    tinydbms::core::ExecuteScriptResult execute_script(
        const tinydbms::core::ExecuteScriptRequest& request) override {
        if (database_ == nullptr) {
            tinydbms::core::ExecuteScriptResult result;
            result.script_error = tinydbms::core::Error{
                tinydbms::core::ErrorKind::kExecute,
                std::nullopt,
                std::nullopt,
                "database has not been opened",
                std::nullopt,
                std::nullopt};
            return result;
        }
        return database_->execute_script(request);
    }

    tinydbms::core::CloseDatabaseResult close() override {
        if (database_ == nullptr) {
            return tinydbms::core::CloseDatabaseResult{std::nullopt};
        }
        const tinydbms::core::CloseDatabaseResult result = database_->close();
        const bool open_reported = open_reported_;
        open_reported_ = false;
        if (close_succeeded(open_reported, result)) {
            return tinydbms::core::CloseDatabaseResult{std::nullopt};
        }
        return result;
    }

private:
    std::unique_ptr<tinydbms::core::Database> database_;
    bool open_reported_ = false;  // 上一次 open 是否成功报告给调用方
};

}  // namespace

std::unique_ptr<Backend> make_backend() {
    return std::make_unique<CoreBackend>();
}

}  // namespace tinydbms::gui
