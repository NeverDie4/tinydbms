#pragma once

#include "page_file.h"
#include "tinydbms/common.hpp"

#include <filesystem>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <unordered_map>

namespace tinydbms::storage::internal {

class FileManager;
class PageFileLease {
public:
    PageFileLease() noexcept = default;
    ~PageFileLease() noexcept;
    PageFileLease(const PageFileLease&) = delete;
    PageFileLease& operator=(const PageFileLease&) = delete;
    PageFileLease(PageFileLease&& other) noexcept;
    PageFileLease& operator=(PageFileLease&& other) noexcept;
    PageFile* get() const noexcept { return file_; }
    PageFile& operator*() const noexcept { return *file_; }
    PageFile* operator->() const noexcept { return file_; }
    explicit operator bool() const noexcept { return file_ != nullptr; }
    void release() noexcept;
private:
    friend class FileManager;
    PageFileLease(FileManager& manager, TableId table_id, PageFile& file) noexcept;
    FileManager* manager_ = nullptr;
    PageFile* file_ = nullptr;
    TableId table_id_ = 0;
};

class FileManager {
public:
    static PageFileResult<std::unique_ptr<FileManager>> open(
        const std::filesystem::path& data_dir);

    ~FileManager();

    FileManager(const FileManager&) = delete;
    FileManager& operator=(const FileManager&) = delete;
    FileManager(FileManager&&) = delete;
    FileManager& operator=(FileManager&&) = delete;

    PageFileResult<PageFile*> create_table_file(TableId table_id);
    PageFileResult<PageFile*> open_table_file(TableId table_id);
    PageFileResult<PageFileLease> acquire_file(TableId table_id);
    // Legacy diagnostic/test lookup only; callers must not perform a PageFile operation through it.
    PageFile* find_table_file(TableId table_id) const noexcept;
    std::optional<PageFileError> close_table_file(TableId table_id);
    std::optional<PageFileError> close_all();

    // Used by Storage only to roll back a table file created in the current operation.
    std::optional<PageFileError> remove_table_file(TableId table_id);

    [[nodiscard]] std::filesystem::path table_file_path(TableId table_id) const;

private:
    friend class PageFileLease;
    friend struct FileManagerTestAccess;
    explicit FileManager(std::filesystem::path data_dir);

    void release_lease(TableId table_id) noexcept;
    std::filesystem::path data_dir_;
    std::filesystem::path tables_dir_;
    mutable std::mutex mutex_;
    std::condition_variable state_changed_;
    std::unordered_map<TableId, std::unique_ptr<PageFile>> open_files_;
    std::unordered_map<TableId, std::size_t> in_flight_;
    std::unordered_set<TableId> closing_tables_;
    bool close_all_in_progress_ = false;
};

}  // namespace tinydbms::storage::internal
