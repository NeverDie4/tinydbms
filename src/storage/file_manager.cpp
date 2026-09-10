#include "file_manager.h"

#include <string>
#include <utility>

namespace tinydbms::storage::internal {
namespace {

PageFileError make_error(PageFileErrorKind kind, std::string message) {
    return PageFileError{kind, std::move(message)};
}

template <typename T>
PageFileResult<T> failure(PageFileErrorKind kind, std::string message) {
    return PageFileResult<T>{std::nullopt, make_error(kind, std::move(message))};
}

template <typename T>
PageFileResult<T> success(T value) {
    return PageFileResult<T>{std::move(value), std::nullopt};
}

}  // namespace

FileManager::FileManager(std::filesystem::path data_dir)
    : data_dir_(std::move(data_dir)), tables_dir_(data_dir_ / "tables") {}

FileManager::~FileManager() {
    (void)close_all();
}

PageFileResult<std::unique_ptr<FileManager>> FileManager::open(
    const std::filesystem::path& data_dir) {
    try {
        std::error_code filesystem_error;
        const bool data_dir_exists = std::filesystem::exists(data_dir, filesystem_error);
        if (filesystem_error) {
            return failure<std::unique_ptr<FileManager>>(PageFileErrorKind::kIo,
                                                         "cannot inspect storage data directory");
        }
        if (data_dir_exists &&
            (!std::filesystem::is_directory(data_dir, filesystem_error) || filesystem_error)) {
            return failure<std::unique_ptr<FileManager>>(PageFileErrorKind::kIo,
                                                         "storage data path is not a directory");
        }

        auto manager = std::unique_ptr<FileManager>(new FileManager(data_dir));
        std::filesystem::create_directories(manager->tables_dir_, filesystem_error);
        if (filesystem_error) {
            return failure<std::unique_ptr<FileManager>>(PageFileErrorKind::kIo,
                                                         "cannot create tables directory");
        }
        if (!std::filesystem::is_directory(manager->tables_dir_, filesystem_error) ||
            filesystem_error) {
            return failure<std::unique_ptr<FileManager>>(PageFileErrorKind::kIo,
                                                         "tables path is not a directory");
        }
        return success(std::move(manager));
    } catch (const std::filesystem::filesystem_error&) {
        return failure<std::unique_ptr<FileManager>>(PageFileErrorKind::kIo,
                                                     "filesystem operation failed in FileManager");
    }
}

std::filesystem::path FileManager::table_file_path(TableId table_id) const {
    return tables_dir_ / ("table_" + std::to_string(table_id) + ".dat");
}

PageFileResult<PageFile*> FileManager::create_table_file(TableId table_id) {
    if (find_table_file(table_id) != nullptr) {
        return failure<PageFile*>(PageFileErrorKind::kInvalidArgument,
                                  "table PageFile is already open");
    }
    auto created = PageFile::create(table_file_path(table_id));
    if (!created.value.has_value()) {
        return failure<PageFile*>(created.error->kind, created.error->message);
    }
    PageFile* page_file = created.value->get();
    open_files_.emplace(table_id, std::move(*created.value));
    return success(page_file);
}

PageFileResult<PageFile*> FileManager::open_table_file(TableId table_id) {
    if (PageFile* existing = find_table_file(table_id); existing != nullptr) {
        return success(existing);
    }

    std::error_code filesystem_error;
    const auto path = table_file_path(table_id);
    const bool exists = std::filesystem::exists(path, filesystem_error);
    if (filesystem_error) {
        return failure<PageFile*>(PageFileErrorKind::kIo, "cannot inspect table PageFile");
    }
    if (!exists) {
        return failure<PageFile*>(PageFileErrorKind::kInvalidArgument,
                                  "table PageFile does not exist");
    }

    auto opened = PageFile::open(path);
    if (!opened.value.has_value()) {
        return failure<PageFile*>(opened.error->kind, opened.error->message);
    }
    PageFile* page_file = opened.value->get();
    open_files_.emplace(table_id, std::move(*opened.value));
    return success(page_file);
}

PageFile* FileManager::find_table_file(TableId table_id) const noexcept {
    const auto found = open_files_.find(table_id);
    return found == open_files_.end() ? nullptr : found->second.get();
}

std::optional<PageFileError> FileManager::close_table_file(TableId table_id) {
    const auto found = open_files_.find(table_id);
    if (found == open_files_.end()) {
        return std::nullopt;
    }
    auto close_error = found->second->close();
    if (close_error.has_value()) {
        return close_error;
    }
    open_files_.erase(found);
    return std::nullopt;
}

std::optional<PageFileError> FileManager::close_all() {
    if (open_files_.empty() && PageFile::take_close_failure_for_testing()) {
        return make_error(PageFileErrorKind::kIo, "injected PageFile close failure");
    }
    std::optional<PageFileError> first_error;
    for (auto file = open_files_.begin(); file != open_files_.end();) {
        if (auto close_error = file->second->close(); close_error.has_value()) {
            if (!first_error.has_value()) {
                first_error = std::move(close_error);
            }
            ++file;
        } else {
            file = open_files_.erase(file);
        }
    }
    return first_error;
}

std::optional<PageFileError> FileManager::remove_table_file(TableId table_id) {
    if (auto close_error = close_table_file(table_id); close_error.has_value()) {
        return close_error;
    }
    std::error_code filesystem_error;
    const bool removed = std::filesystem::remove(table_file_path(table_id), filesystem_error);
    if (filesystem_error || !removed) {
        return make_error(PageFileErrorKind::kIo, "cannot remove table PageFile");
    }
    return std::nullopt;
}

}  // namespace tinydbms::storage::internal
