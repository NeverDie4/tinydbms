#include "backend.hpp"

#include <memory>
#include <optional>

namespace tinydbms::gui {
namespace {

// 与 app 的 UnavailableSession 保持同一语义：窗口可用，但不伪造任何数据。
tinydbms::core::Error unavailable_error() {
    return tinydbms::core::Error{
        tinydbms::core::ErrorKind::kInternal,
        std::nullopt,
        std::nullopt,
        "compiler/storage implementations are not available in this build; "
        "rebuild with TINYDBMS_ENABLE_REAL_MODULES=ON",
        std::nullopt,
        std::nullopt};
}

class UnavailableBackend final : public Backend {
public:
    tinydbms::core::OpenDatabaseResult open(
        const tinydbms::core::OpenDatabaseRequest& /*request*/) override {
        return tinydbms::core::OpenDatabaseResult{
            std::optional<tinydbms::core::Error>{unavailable_error()}};
    }

    tinydbms::core::ExecuteScriptResult execute_script(
        const tinydbms::core::ExecuteScriptRequest& /*request*/) override {
        tinydbms::core::ExecuteScriptResult result;
        result.script_error = unavailable_error();
        return result;
    }

    tinydbms::core::CloseDatabaseResult close() override {
        return tinydbms::core::CloseDatabaseResult{std::nullopt};
    }
};

}  // namespace

std::unique_ptr<Backend> make_backend() {
    return std::make_unique<UnavailableBackend>();
}

}  // namespace tinydbms::gui
