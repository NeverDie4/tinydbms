#include "session.hpp"

#include <optional>

namespace tinydbms::app {
namespace {

tinydbms::core::Error unavailable_error() {
    return tinydbms::core::Error{
        tinydbms::core::ErrorKind::kInternal,
        std::nullopt,
        "compiler/storage implementations are not available in this build"};
}

tinydbms::core::ExecuteScriptResult unavailable_execute_result() {
    tinydbms::core::ExecuteScriptResult result;
    result.outcomes.push_back(tinydbms::core::ExecuteResult{unavailable_error()});
    return result;
}

}  // namespace

tinydbms::core::OpenDatabaseResult UnavailableSession::open(
    const tinydbms::core::OpenDatabaseRequest& /*request*/) {
    return tinydbms::core::OpenDatabaseResult{
        std::optional<tinydbms::core::Error>{unavailable_error()}};
}

tinydbms::core::ExecuteScriptResult UnavailableSession::execute_script(
    const tinydbms::core::ExecuteScriptRequest& /*request*/) {
    return unavailable_execute_result();
}

tinydbms::core::CloseDatabaseResult UnavailableSession::close() {
    return tinydbms::core::CloseDatabaseResult{std::nullopt};
}

}  // namespace tinydbms::app
