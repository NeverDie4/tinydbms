#include "worker.hpp"

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "backend.hpp"

namespace tinydbms::gui {
namespace {

tinydbms::core::Error internal_error(const std::string& message) {
    return tinydbms::core::Error{
        tinydbms::core::ErrorKind::kInternal,
        std::nullopt,
        std::nullopt,
        message,
        std::nullopt,
        std::nullopt};
}

std::string utf8(const QString& text) {
    return text.toUtf8().toStdString();
}

}  // namespace

Worker::Worker(Backend* backend, QObject* parent) : QObject{parent}, backend_{backend} {}

void Worker::open_database(const QString& data_dir) {
    LifecyclePayload payload;
    payload.data_dir = utf8(data_dir);
    try {
        const tinydbms::core::OpenDatabaseResult result =
            backend_->open(tinydbms::core::OpenDatabaseRequest{payload.data_dir});
        if (result.error.has_value()) {
            payload.action = LifecycleAction::kOpenFailed;
            payload.error = std::make_shared<const tinydbms::core::Error>(*result.error);
        } else {
            payload.action = LifecycleAction::kOpened;
        }
    } catch (const std::exception& exception) {
        payload.action = LifecycleAction::kOpenFailed;
        payload.error = std::make_shared<const tinydbms::core::Error>(
            internal_error(exception.what()));
    } catch (...) {
        payload.action = LifecycleAction::kOpenFailed;
        payload.error = std::make_shared<const tinydbms::core::Error>(
            internal_error("unknown exception while opening the database"));
    }
    emit lifecycle_finished(payload);
}

void Worker::execute_script(
    const QString& text,
    bool analyze_mode,
    quint64 snapshot_id,
    tinydbms::core::CancelToken cancel) {
    ExecutionPayload payload;
    payload.snapshot_id = snapshot_id;
    try {
        auto result = std::make_shared<tinydbms::core::ExecuteScriptResult>();
        tinydbms::core::ExecuteScriptRequest request;
        request.text = utf8(text);
        request.error_policy = analyze_mode
            ? tinydbms::core::ScriptErrorPolicy::kAnalyzeRemaining
            : tinydbms::core::ScriptErrorPolicy::kStopOnFirstError;
        request.cancel = std::move(cancel);
        *result = backend_->execute_script(request);
        payload.result = result;
    } catch (const std::exception& exception) {
        auto result = std::make_shared<tinydbms::core::ExecuteScriptResult>();
        result->script_error = internal_error(exception.what());
        payload.result = result;
    } catch (...) {
        auto result = std::make_shared<tinydbms::core::ExecuteScriptResult>();
        result->script_error = internal_error("unknown exception while executing the script");
        payload.result = result;
    }
    emit script_finished(payload);
}

void Worker::close_database() {
    LifecyclePayload payload;
    try {
        const tinydbms::core::CloseDatabaseResult result = backend_->close();
        if (result.error.has_value()) {
            payload.action = LifecycleAction::kCloseFailed;
            payload.error = std::make_shared<const tinydbms::core::Error>(*result.error);
        } else {
            payload.action = LifecycleAction::kClosed;
        }
    } catch (const std::exception& exception) {
        payload.action = LifecycleAction::kCloseFailed;
        payload.error = std::make_shared<const tinydbms::core::Error>(
            internal_error(exception.what()));
    } catch (...) {
        payload.action = LifecycleAction::kCloseFailed;
        payload.error = std::make_shared<const tinydbms::core::Error>(
            internal_error("unknown exception while closing the database"));
    }
    emit lifecycle_finished(payload);
}

}  // namespace tinydbms::gui
