#ifndef TINYDBMS_APP_SESSION_HPP
#define TINYDBMS_APP_SESSION_HPP

#include <memory>

#include "tinydbms/core.hpp"

namespace tinydbms::app {

class Session {
public:
    virtual ~Session() = default;

    virtual tinydbms::core::OpenDatabaseResult open(
        const tinydbms::core::OpenDatabaseRequest& request) = 0;

    virtual tinydbms::core::ExecuteScriptResult execute_script(
        const tinydbms::core::ExecuteScriptRequest& request) = 0;

    virtual tinydbms::core::CloseDatabaseResult close() = 0;
};

class CoreSession final : public Session {
public:
    tinydbms::core::OpenDatabaseResult open(
        const tinydbms::core::OpenDatabaseRequest& request) override;

    tinydbms::core::ExecuteScriptResult execute_script(
        const tinydbms::core::ExecuteScriptRequest& request) override;

    tinydbms::core::CloseDatabaseResult close() override;

private:
    // Delay construction until runner has accepted the command-line action.
    std::unique_ptr<tinydbms::core::Database> database_;
};

class UnavailableSession final : public Session {
public:
    tinydbms::core::OpenDatabaseResult open(
        const tinydbms::core::OpenDatabaseRequest& request) override;

    tinydbms::core::ExecuteScriptResult execute_script(
        const tinydbms::core::ExecuteScriptRequest& request) override;

    tinydbms::core::CloseDatabaseResult close() override;
};

}  // namespace tinydbms::app

#endif  // TINYDBMS_APP_SESSION_HPP
