#include "session.hpp"

#include <optional>
#include <utility>

namespace tinydbms::app {
namespace {

tinydbms::core::Error not_open_error() {
    return tinydbms::core::Error{
        tinydbms::core::ErrorKind::kExecute,
        std::nullopt,
        "session is not open"};
}

tinydbms::core::ExecuteScriptResult not_open_execute_result() {
    tinydbms::core::ExecuteScriptResult result;
    result.outcomes.push_back(tinydbms::core::ExecuteResult{not_open_error()});
    return result;
}

}  // namespace

tinydbms::core::OpenDatabaseResult CoreSession::open(
    const tinydbms::core::OpenDatabaseRequest& request) {
    if (database_ != nullptr) {
        return tinydbms::core::OpenDatabaseResult{
            std::optional<tinydbms::core::Error>{tinydbms::core::Error{
                tinydbms::core::ErrorKind::kExecute,
                std::nullopt,
                "session is already open"}}};
    }

    auto candidate = std::make_unique<tinydbms::core::Database>();
    const tinydbms::core::OpenDatabaseResult result = candidate->open(request);
    if (result.error.has_value()) {
        return result;
    }
    database_ = std::move(candidate);
    return result;
}

tinydbms::core::ExecuteScriptResult CoreSession::execute_script(
    const tinydbms::core::ExecuteScriptRequest& request) {
    if (database_ == nullptr) {
        return not_open_execute_result();
    }
    return database_->execute_script(request);
}

tinydbms::core::CloseDatabaseResult CoreSession::close() {
    if (database_ == nullptr) {
        return tinydbms::core::CloseDatabaseResult{
            std::optional<tinydbms::core::Error>{not_open_error()}};
    }

    tinydbms::core::CloseDatabaseResult result = database_->close();
    if (!result.error.has_value()) {
        database_.reset();
    }
    return result;
}

}  // namespace tinydbms::app
