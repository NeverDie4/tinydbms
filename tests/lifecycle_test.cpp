#include "tinydbms/storage.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>

int main() {
    using namespace tinydbms;
    using namespace tinydbms::storage;
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path data_dir =
        std::filesystem::temp_directory_path() / ("tinydbms-storage-lifecycle-" + std::to_string(suffix));

    assert(!open_storage({data_dir.string()}).error);
    const CreateTableRequest create{2, "student", {{"id", Type::kInt}, {"name", Type::kVarchar}}};
    assert(!create_table(create).error);
    assert(!close_storage({}).error);

    assert(!open_storage({data_dir.string()}).error);
    const auto tables = list_tables({});
    assert(!tables.error && tables.tables.size() == 3 && tables.tables[2].table_id == 2);
    assert(!close_storage({}).error);

    std::filesystem::remove_all(data_dir);
    return 0;
}
