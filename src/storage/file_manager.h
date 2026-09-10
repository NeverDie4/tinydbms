#pragma once

#include "page_file.h"
#include "tinydbms/common.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_map>

namespace tinydbms::storage::internal {

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
    PageFile* find_table_file(TableId table_id) const noexcept;
    std::optional<PageFileError> close_table_file(TableId table_id);
    std::optional<PageFileError> close_all();

    // Used by Storage only to roll back a table file created in the current operation.
    std::optional<PageFileError> remove_table_file(TableId table_id);

    [[nodiscard]] std::filesystem::path table_file_path(TableId table_id) const;

private:
    explicit FileManager(std::filesystem::path data_dir);

    std::filesystem::path data_dir_;
    std::filesystem::path tables_dir_;
    std::unordered_map<TableId, std::unique_ptr<PageFile>> open_files_;
};

}  // namespace tinydbms::storage::internal
