#include "tinydbms/storage.hpp"

#include "file_manager.h"
#include "cursor_state.h"
#include "storage_test_access.h"

#include <algorithm>
#include <filesystem>
#include <charconv>
#include <fstream>
#include <map>
#include <new>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace tinydbms::storage {
namespace {

enum class Lifecycle { kClosed, kOpen, kClosing };
struct StorageState {
    Lifecycle lifecycle = Lifecycle::kClosed;
    std::filesystem::path data_dir;
    std::vector<TableMeta> tables;
    std::unique_ptr<internal::FileManager> file_manager;
    std::unique_ptr<internal::BufferPool> buffer_pool; // Destroyed before FileManager.
    std::unique_ptr<internal::CursorRegistry> cursors; // Destroyed before borrowed resources.
};

StorageState state;
constexpr TableId kSystemTablesId = 0;
constexpr TableId kSystemColumnsId = 1;
constexpr std::string_view kSystemTablesName = "tdb_sys_tables";
constexpr std::string_view kSystemColumnsName = "tdb_sys_columns";
struct BufferConfig {
    std::size_t capacity = 64;
    internal::ReplacementPolicy policy = internal::ReplacementPolicy::kFifo;
    internal::BufferPool::WritePage write;
    internal::BufferPool::ReadPage read;
} buffer_config;

void reset_state() {
    state.cursors.reset(); // Does not reset the process-lifetime CursorId allocator.
    state.buffer_pool.reset();
    state.file_manager.reset();
    state = StorageState{};
}

StorageError error(StorageErrorKind kind, std::string message) {
    return StorageError{kind, std::move(message)};
}
StorageError unexpected_exception_error() {
    return error(StorageErrorKind::kIoError, "unexpected exception in storage API");
}
StorageError buffer_error(const internal::BufferPoolError& source) {
    switch (source.kind) {
        case internal::BufferPoolErrorKind::kIo: return error(StorageErrorKind::kIoError, source.message);
        case internal::BufferPoolErrorKind::kCorrupt: return error(StorageErrorKind::kCorrupt, source.message);
        case internal::BufferPoolErrorKind::kInvalidArgument:
        case internal::BufferPoolErrorKind::kNoVictim: return error(StorageErrorKind::kInvalidRequest, source.message);
    }
    return error(StorageErrorKind::kInvalidRequest, source.message);
}

StorageError data_error(const internal::HeapTableError& source) {
    using Kind=internal::HeapTableErrorKind;
    switch(source.kind) {
        case Kind::kInvalidArgument:return error(StorageErrorKind::kInvalidRequest,source.message);
        case Kind::kValueTooLarge:return error(StorageErrorKind::kValueTooLarge,source.message);
        case Kind::kNoVictim:return error(StorageErrorKind::kInvalidRequest,"BufferPool resource unavailable: no unpinned replacement frame; " + source.message);
        case Kind::kIo:return error(StorageErrorKind::kIoError,source.message);
        case Kind::kCorrupt:return error(StorageErrorKind::kCorrupt,source.message);
    }
    return error(StorageErrorKind::kCorrupt,"unknown heap error");
}
StorageError data_error(const internal::CursorError& source) {
    using C=internal::CursorErrorKind;
    using H=internal::HeapTableErrorKind;
    switch(source.kind) {
        case C::kCursorInvalid:return error(StorageErrorKind::kCursorInvalid,source.message);
        case C::kInvalidArgument:return data_error(internal::HeapTableError{H::kInvalidArgument,source.message});
        case C::kValueTooLarge:return data_error(internal::HeapTableError{H::kValueTooLarge,source.message});
        case C::kNoVictim:return data_error(internal::HeapTableError{H::kNoVictim,source.message});
        case C::kIo:return data_error(internal::HeapTableError{H::kIo,source.message});
        case C::kCorrupt:return data_error(internal::HeapTableError{H::kCorrupt,source.message});
    }
    return error(StorageErrorKind::kCorrupt,"unknown cursor error");
}
template<typename E> std::optional<StorageError> data_error(const std::optional<E>& source) {
    return source ? std::optional{data_error(*source)} : std::nullopt;
}
std::optional<StorageError> require_data_open(bool cursor_api=false) {
    if(state.lifecycle==Lifecycle::kOpen)return std::nullopt;
    if(cursor_api && state.lifecycle==Lifecycle::kClosed)
        return error(StorageErrorKind::kCursorInvalid,"storage is closed; cursor is invalid");
    return error(StorageErrorKind::kInvalidRequest,"storage is not open");
}

StorageError file_error(const internal::PageFileError& source,
                        bool invalid_means_corrupt = false) {
    switch (source.kind) {
        case internal::PageFileErrorKind::kIo:
            return error(StorageErrorKind::kIoError, source.message);
        case internal::PageFileErrorKind::kCorrupt:
            return error(StorageErrorKind::kCorrupt, source.message);
        case internal::PageFileErrorKind::kInvalidArgument:
            return error(invalid_means_corrupt ? StorageErrorKind::kCorrupt
                                               : StorageErrorKind::kInvalidRequest,
                         source.message);
    }
    return error(StorageErrorKind::kIoError, "unknown storage file error");
}

std::filesystem::path metadata_path() {
    return state.data_dir / "storage.meta";
}

std::filesystem::path temporary_metadata_path() {
    return state.data_dir / "storage.meta.tmp";
}

TableMeta system_tables_meta() {
    return TableMeta{kSystemTablesId, std::string{kSystemTablesName}, {
        {"table_id", Type::kVarchar},
        {"table_name", Type::kVarchar},
        {"column_count", Type::kInt}}};
}

TableMeta system_columns_meta() {
    return TableMeta{kSystemColumnsId, std::string{kSystemColumnsName}, {
        {"table_id", Type::kVarchar},
        {"column_ordinal", Type::kInt},
        {"column_name", Type::kVarchar},
        {"column_type", Type::kVarchar}}};
}

std::vector<TableMeta> system_tables() {
    std::vector<TableMeta> tables;
    tables.reserve(2);
    tables.push_back(system_tables_meta());
    tables.push_back(system_columns_meta());
    return tables;
}

bool is_reserved_system_identity(TableId table_id, std::string_view table_name) {
    return table_id == kSystemTablesId || table_id == kSystemColumnsId ||
        table_name == kSystemTablesName || table_name == kSystemColumnsName;
}

bool is_system_catalog_table(TableId table_id) {
    return table_id == kSystemTablesId || table_id == kSystemColumnsId;
}

std::optional<std::string_view> type_name(Type type) {
    switch (type) {
        case Type::kInt:
            return "INT32";
        case Type::kVarchar:
            return "VARCHAR";
    }
    return std::nullopt;
}

bool is_normalized_identifier(std::string_view name);
bool has_valid_columns(const std::vector<ColumnMeta>& columns);

enum class BootstrapReadResult { kOk, kLegacy, kCorrupt };

BootstrapReadResult read_bootstrap(std::ifstream& input) {
    std::string line;
    if (!std::getline(input, line)) {
        return BootstrapReadResult::kCorrupt;
    }
    if (line == "TINYDBMS_STORAGE_V1") {
        return BootstrapReadResult::kLegacy;
    }
    if (line != "TINYDBMS_STORAGE_BOOTSTRAP_V2") {
        return BootstrapReadResult::kCorrupt;
    }
    bool saw_tables = false;
    bool saw_columns = false;
    while (std::getline(input, line)) {
        if (line == "END") {
            while (std::getline(input, line)) {
                if (line.find_first_not_of(" \t\r") != std::string::npos) {
                    return BootstrapReadResult::kCorrupt;
                }
            }
            return !input.bad() && saw_tables && saw_columns
                ? BootstrapReadResult::kOk
                : BootstrapReadResult::kCorrupt;
        }
        if (line == "SYS_TABLES 0" && !saw_tables) {
            saw_tables = true;
            continue;
        }
        if (line == "SYS_COLUMNS 1" && !saw_columns) {
            saw_columns = true;
            continue;
        }
        return BootstrapReadResult::kCorrupt;
    }
    return BootstrapReadResult::kCorrupt;
}

std::optional<StorageError> write_bootstrap() {
    const auto temporary_path = temporary_metadata_path();
    std::error_code filesystem_error;
    std::filesystem::remove(temporary_path, filesystem_error);
    if (filesystem_error) {
        return error(StorageErrorKind::kIoError, "cannot remove temporary storage metadata");
    }

    {
        std::ofstream output(temporary_path, std::ios::trunc);
        if (!output) {
            return error(StorageErrorKind::kIoError, "cannot write temporary storage metadata");
        }
        output << "TINYDBMS_STORAGE_BOOTSTRAP_V2\n"
               << "SYS_TABLES 0\n"
               << "SYS_COLUMNS 1\n"
               << "END\n";
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary_path, filesystem_error);
            return error(StorageErrorKind::kIoError, "cannot flush temporary storage metadata");
        }
        output.close();
        if (output.fail()) {
            std::filesystem::remove(temporary_path, filesystem_error);
            return error(StorageErrorKind::kIoError, "cannot close temporary storage metadata");
        }
    }

#ifdef _WIN32
    if (MoveFileExW(temporary_path.c_str(), metadata_path().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        std::filesystem::remove(temporary_path, filesystem_error);
        return error(StorageErrorKind::kIoError, "cannot replace storage bootstrap metadata");
    }
#else
    std::filesystem::rename(temporary_path, metadata_path(), filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(temporary_path, filesystem_error);
        return error(StorageErrorKind::kIoError, "cannot replace storage bootstrap metadata");
    }
#endif
    return std::nullopt;
}

bool has_table_name(std::string_view name) {
    for (const TableMeta& table : state.tables) {
        if (table.table_name == name) {
            return true;
        }
    }
    return false;
}

bool is_normalized_identifier(std::string_view name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    const auto is_lower_alpha = [](char value) { return value >= 'a' && value <= 'z'; };
    const auto is_digit = [](char value) { return value >= '0' && value <= '9'; };
    if (!is_lower_alpha(name.front())) {
        return false;
    }
    for (const char value : name.substr(1)) {
        if (!is_lower_alpha(value) && !is_digit(value) && value != '_') {
            return false;
        }
    }
    return true;
}

bool has_valid_columns(const std::vector<ColumnMeta>& columns) {
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (!is_normalized_identifier(columns[index].name) || !type_name(columns[index].type).has_value()) {
            return false;
        }
        for (std::size_t other = 0; other < index; ++other) {
            if (columns[index].name == columns[other].name) {
                return false;
            }
        }
    }
    return true;
}

const TableMeta* find_table(TableId table_id) {
    for (const TableMeta& table : state.tables) {
        if (table.table_id == table_id) {
            return &table;
        }
    }
    return nullptr;
}

std::optional<TableId> parse_catalog_table_id(const Value& value) {
    const auto* text = std::get_if<std::string>(&value.data);
    if (text == nullptr || text->empty() || (text->size() > 1 && text->front() == '0')) {
        return std::nullopt;
    }
    TableId table_id = 0;
    const auto parsed = std::from_chars(text->data(), text->data() + text->size(), table_id);
    if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size()) {
        return std::nullopt;
    }
    return table_id;
}

struct PendingCatalogTable {
    std::string name;
    std::int32_t column_count = 0;
    std::map<std::int32_t, ColumnMeta> columns;
};

std::optional<StorageError> scan_catalog_records(
    const TableMeta& metadata,
    internal::FileManager& file_manager,
    internal::BufferPool& buffer_pool,
    std::vector<Record>& records) {
    internal::HeapTable table(metadata, file_manager, buffer_pool);
    auto position = table.begin_scan();
    if (!position.value.has_value()) {
        return data_error(*position.error);
    }
    while (true) {
        auto next = table.next_record(*position.value);
        if (next.error.has_value()) {
            return data_error(*next.error);
        }
        if (!next.value.has_value()) {
            return std::nullopt;
        }
        records.push_back(std::move(*next.value));
    }
}

std::optional<StorageError> load_user_catalog(
    internal::FileManager& file_manager,
    internal::BufferPool& buffer_pool,
    std::vector<TableMeta>& tables) {
    std::vector<Record> table_records;
    std::vector<Record> column_records;
    const TableMeta tables_meta = system_tables_meta();
    const TableMeta columns_meta = system_columns_meta();
    if (auto scan_error = scan_catalog_records(tables_meta, file_manager, buffer_pool, table_records)) {
        return scan_error;
    }
    if (auto scan_error = scan_catalog_records(columns_meta, file_manager, buffer_pool, column_records)) {
        return scan_error;
    }

    std::map<TableId, PendingCatalogTable> pending;
    for (const Record& record : table_records) {
        if (record.values.size() != 3) {
            return error(StorageErrorKind::kCorrupt, "invalid tdb_sys_tables record");
        }
        const auto table_id = parse_catalog_table_id(record.values[0]);
        const auto* name = std::get_if<std::string>(&record.values[1].data);
        const auto* column_count = std::get_if<std::int32_t>(&record.values[2].data);
        if (!table_id.has_value() || name == nullptr || column_count == nullptr || *column_count <= 0 ||
            is_reserved_system_identity(*table_id, *name) || !is_normalized_identifier(*name) ||
            pending.contains(*table_id)) {
            return error(StorageErrorKind::kCorrupt, "invalid tdb_sys_tables record");
        }
        for (const auto& [existing_id, existing] : pending) {
            if (existing.name == *name) {
                return error(StorageErrorKind::kCorrupt, "duplicate tdb_sys_tables name");
            }
        }
        pending.emplace(*table_id, PendingCatalogTable{*name, *column_count, {}});
    }

    for (const Record& record : column_records) {
        if (record.values.size() != 4) {
            return error(StorageErrorKind::kCorrupt, "invalid tdb_sys_columns record");
        }
        const auto table_id = parse_catalog_table_id(record.values[0]);
        const auto* ordinal = std::get_if<std::int32_t>(&record.values[1].data);
        const auto* name = std::get_if<std::string>(&record.values[2].data);
        const auto* type_name_value = std::get_if<std::string>(&record.values[3].data);
        if (!table_id.has_value() || ordinal == nullptr || name == nullptr || type_name_value == nullptr ||
            *ordinal < 0 || is_reserved_system_identity(*table_id, "") ||
            !is_normalized_identifier(*name)) {
            return error(StorageErrorKind::kCorrupt, "invalid tdb_sys_columns record");
        }
        const auto type = *type_name_value == "INT32" ? std::optional<Type>{Type::kInt}
            : *type_name_value == "VARCHAR" ? std::optional<Type>{Type::kVarchar}
            : std::nullopt;
        auto table = pending.find(*table_id);
        if (!type.has_value() || table == pending.end() ||
            !table->second.columns.emplace(*ordinal, ColumnMeta{*name, *type}).second) {
            return error(StorageErrorKind::kCorrupt, "invalid tdb_sys_columns record");
        }
    }

    tables = system_tables();
    for (auto& [table_id, pending_table] : pending) {
        if (pending_table.columns.size() != static_cast<std::size_t>(pending_table.column_count)) {
            return error(StorageErrorKind::kCorrupt, "tdb_sys_columns count does not match tdb_sys_tables");
        }
        TableMeta table{table_id, std::move(pending_table.name), {}};
        table.columns.reserve(pending_table.columns.size());
        for (std::int32_t ordinal = 0; ordinal < pending_table.column_count; ++ordinal) {
            const auto column = pending_table.columns.find(ordinal);
            if (column == pending_table.columns.end()) {
                return error(StorageErrorKind::kCorrupt, "tdb_sys_columns ordinal is not contiguous");
            }
            table.columns.push_back(std::move(column->second));
        }
        if (!has_valid_columns(table.columns)) {
            return error(StorageErrorKind::kCorrupt, "invalid user table schema in system catalog");
        }
        tables.push_back(std::move(table));
    }
    return std::nullopt;
}

std::optional<StorageError> rollback_catalog_create(
    internal::HeapTable& columns_table,
    const std::vector<RecordId>& column_records,
    internal::FileManager& file_manager,
    TableId table_id) {
    bool rollback_failed = false;
    if (!column_records.empty()) {
        const auto deleted = columns_table.delete_batch(column_records);
        rollback_failed = deleted.error.has_value() || deleted.deleted_count != column_records.size();
    }
    if (const auto removed = file_manager.remove_table_file(table_id); removed.has_value()) {
        rollback_failed = true;
    }
    return rollback_failed
        ? std::optional{error(StorageErrorKind::kIoError, "system catalog rollback failed")}
        : std::nullopt;
}

std::optional<TableId> parse_canonical_table_file_name(std::string_view filename) {
    constexpr std::string_view prefix = "table_";
    constexpr std::string_view suffix = ".dat";
    if (!filename.starts_with(prefix) || !filename.ends_with(suffix) ||
        filename.size() == prefix.size() + suffix.size()) {
        return std::nullopt;
    }
    const std::string_view id_text = filename.substr(
        prefix.size(), filename.size() - prefix.size() - suffix.size());
    TableId table_id{};
    const auto parsed = std::from_chars(id_text.data(), id_text.data() + id_text.size(), table_id);
    if (parsed.ec != std::errc{} || parsed.ptr != id_text.data() + id_text.size() ||
        filename != "table_" + std::to_string(table_id) + ".dat") {
        return std::nullopt;
    }
    return table_id;
}

std::optional<StorageError> validate_catalog_table_files(const std::vector<TableMeta>& tables) {
    std::error_code filesystem_error;
    const std::filesystem::path tables_dir = state.data_dir / "tables";
    std::filesystem::directory_iterator iterator(tables_dir, filesystem_error);
    if (filesystem_error) {
        return error(StorageErrorKind::kIoError, "cannot inspect tables directory");
    }
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        const auto table_id = parse_canonical_table_file_name(iterator->path().filename().string());
        const bool catalog_has_file = table_id.has_value() && std::any_of(
            tables.begin(), tables.end(), [&](const TableMeta& table) { return table.table_id == *table_id; });
        if (table_id.has_value() && !catalog_has_file) {
            return error(StorageErrorKind::kCorrupt, "orphan table PageFile is not in system catalog");
        }
        iterator.increment(filesystem_error);
        if (filesystem_error) {
            return error(StorageErrorKind::kIoError, "cannot inspect tables directory");
        }
    }
    return std::nullopt;
}

}  // namespace

OpenStorageResult open_storage(const OpenStorageRequest& request) {
    try {
        if (state.lifecycle != Lifecycle::kClosed) {
            return {error(StorageErrorKind::kInvalidRequest, "storage is already open")};
        }
        if (request.data_dir.empty()) {
            return {error(StorageErrorKind::kInvalidRequest, "data directory is empty")};
        }

        std::error_code filesystem_error;
        const std::filesystem::path data_dir = request.data_dir;
        const bool existed = std::filesystem::exists(data_dir, filesystem_error);
        if (filesystem_error) {
            return {error(StorageErrorKind::kIoError, "cannot inspect data directory")};
        }
        if (!existed) {
            std::filesystem::create_directories(data_dir, filesystem_error);
            if (filesystem_error) {
                return {error(StorageErrorKind::kIoError, "cannot create data directory")};
            }
        } else if (!std::filesystem::is_directory(data_dir, filesystem_error) || filesystem_error) {
            return {error(StorageErrorKind::kIoError, "data path is not a directory")};
        }

        state.data_dir = data_dir;
        state.tables.clear();
        const auto meta = metadata_path();
        const bool metadata_exists = std::filesystem::exists(meta, filesystem_error);
        if (filesystem_error) {
            state.data_dir.clear();
            return {error(StorageErrorKind::kIoError, "cannot inspect storage metadata")};
        }
        bool initializing = false;
        if (metadata_exists) {
            std::ifstream input(meta);
            if (!input) {
                reset_state();
                return {error(StorageErrorKind::kIoError,"cannot open storage.meta")};
            }
            const BootstrapReadResult bootstrap = read_bootstrap(input);
            if (bootstrap != BootstrapReadResult::kOk) {
                const bool io_failed = input.bad();
                state.data_dir.clear();
                return {error(io_failed ? StorageErrorKind::kIoError
                                        : bootstrap == BootstrapReadResult::kLegacy
                                            ? StorageErrorKind::kInvalidRequest
                                            : StorageErrorKind::kCorrupt,
                              io_failed ? "cannot read storage.meta"
                                        : bootstrap == BootstrapReadResult::kLegacy
                                            ? "legacy storage metadata format is unsupported"
                                            : "invalid storage bootstrap metadata")};
            }
            state.tables = system_tables();
        } else {
            bool has_unrecognized_entries = false;
            std::filesystem::directory_iterator iterator(data_dir, filesystem_error);
            if (filesystem_error) {
                state.data_dir.clear();
                return {error(StorageErrorKind::kIoError, "cannot inspect data directory")};
            }
            const std::filesystem::directory_iterator end;
            while (iterator != end) {
                const auto entry = *iterator;
                const bool empty_tables_directory = entry.path().filename() == "tables" &&
                    entry.is_directory(filesystem_error) && !filesystem_error &&
                    std::filesystem::directory_iterator(entry.path(), filesystem_error) == end &&
                    !filesystem_error;
                if (!empty_tables_directory) {
                    has_unrecognized_entries = true;
                    break;
                }
                iterator.increment(filesystem_error);
                if (filesystem_error) {
                    state.data_dir.clear();
                    return {error(StorageErrorKind::kIoError, "cannot inspect data directory")};
                }
            }
            if (has_unrecognized_entries) {
                state.data_dir.clear();
                return {error(StorageErrorKind::kCorrupt, "data directory has no storage.meta")};
            }
            initializing = true;
        }
        const auto tables_dir = data_dir / "tables";
        const bool tables_existed = std::filesystem::exists(tables_dir, filesystem_error);
        if (filesystem_error) {
            reset_state();
            return {error(StorageErrorKind::kIoError, "cannot inspect tables directory")};
        }
        // Declared before FileManager: files close first during unwinding. Never
        // recursively delete; remove only the newly created directory if empty.
        struct EmptyDirectoryRollback {
            std::filesystem::path path;
            bool keep;
            ~EmptyDirectoryRollback() noexcept {
                if (!keep) {
                    try { std::error_code ec; std::filesystem::remove(path, ec); }
                    catch (...) { /* Cleanup must not throw during allocation failure. */ }
                }
            }
        } directory_rollback{tables_dir, tables_existed};
        auto file_manager = internal::FileManager::open(data_dir);
        if (!file_manager.value.has_value()) {
            const auto mapped = file_error(*file_manager.error);
            reset_state();
            return {mapped};
        }

        auto rollback_initialization = [&]() {
            auto* manager = state.file_manager ? state.file_manager.get() : file_manager.value->get();
            (void)manager->remove_table_file(kSystemColumnsId);
            (void)manager->remove_table_file(kSystemTablesId);
            std::error_code cleanup_error;
            std::filesystem::remove(metadata_path(), cleanup_error);
        };

        if (initializing) {
            auto created_tables = (*file_manager.value)->create_table_file(kSystemTablesId);
            if (!created_tables.value.has_value()) {
                const auto mapped = file_error(*created_tables.error, true);
                reset_state();
                return {mapped};
            }
            auto created_columns = (*file_manager.value)->create_table_file(kSystemColumnsId);
            if (!created_columns.value.has_value()) {
                const auto mapped = file_error(*created_columns.error, true);
                rollback_initialization();
                reset_state();
                return {mapped};
            }
            if (const auto bootstrap_error = write_bootstrap(); bootstrap_error.has_value()) {
                rollback_initialization();
                reset_state();
                return {*bootstrap_error};
            }
        } else {
            for (TableId table_id : {kSystemTablesId, kSystemColumnsId}) {
                auto opened_table = (*file_manager.value)->open_table_file(table_id);
                if (!opened_table.value.has_value()) {
                    const auto mapped = file_error(*opened_table.error, true);
                    reset_state();
                    return {mapped};
                }
            }
        }
        auto pool = internal::BufferPool::create(**file_manager.value, buffer_config.capacity,
            buffer_config.read, buffer_config.write, buffer_config.policy);
        if (!pool.value) {
            const auto mapped = buffer_error(*pool.error);
            if (initializing) {
                rollback_initialization();
            }
            reset_state();
            return {mapped};
        }
        // V2 recovery scans HeapTables before open_storage returns.  Keep the
        // manager owned by Storage while that scan runs so its narrow test I/O
        // adapter observes the same valid manager it will use after open.
        state.file_manager = std::move(*file_manager.value);
        if (const auto catalog_error = load_user_catalog(
                *state.file_manager, **pool.value, state.tables);
            catalog_error.has_value()) {
            pool.value.reset();
            if (initializing) {
                rollback_initialization();
            }
            reset_state();
            return {*catalog_error};
        }
        for (std::size_t index = 2; index < state.tables.size(); ++index) {
            auto opened_table = state.file_manager->open_table_file(state.tables[index].table_id);
            if (!opened_table.value.has_value()) {
                const auto mapped = file_error(*opened_table.error, true);
                pool.value.reset();
                if (initializing) {
                    rollback_initialization();
                }
                reset_state();
                return {mapped};
            }
        }
        if (const auto consistency_error = validate_catalog_table_files(state.tables);
            consistency_error.has_value()) {
            pool.value.reset();
            reset_state();
            return {*consistency_error};
        }
        state.buffer_pool = std::move(*pool.value);
        state.cursors = std::make_unique<internal::CursorRegistry>(*state.file_manager,*state.buffer_pool);
        state.lifecycle = Lifecycle::kOpen;
        directory_rollback.keep = true;
        return {};
    } catch (const std::filesystem::filesystem_error&) {
        reset_state();
        return {error(StorageErrorKind::kIoError, "filesystem operation failed while opening storage")};
    } catch (const std::bad_alloc&) {
        reset_state();
        return {error(StorageErrorKind::kIoError, "cannot allocate storage resources")};
    } catch (const std::length_error&) {
        reset_state();
        return {error(StorageErrorKind::kInvalidRequest, "BufferPool capacity is too large")};
    } catch (...) {
        reset_state();
        return {unexpected_exception_error()};
    }
}

CloseStorageResult close_storage(const CloseStorageRequest&) {
    if (state.lifecycle == Lifecycle::kClosed) return {};
    if (state.lifecycle == Lifecycle::kOpen) {
        state.lifecycle = Lifecycle::kClosing;
        try {
            if (auto close_error = state.buffer_pool->close()) {
                state.lifecycle = Lifecycle::kOpen;
                return {buffer_error(*close_error)};
            }
        } catch (const std::filesystem::filesystem_error&) {
            state.lifecycle = Lifecycle::kOpen;
            return {error(StorageErrorKind::kIoError, "filesystem operation failed while closing BufferPool")};
        } catch (...) {
            return {unexpected_exception_error()};
        }
    }
    // Closing retry skips the already closed pool. FileManager retains failed
    // PageFile owners, so each retry continues outstanding file cleanup.
    try {
        auto close_error = state.file_manager->close_all();
        if (close_error) return {file_error(*close_error)};
        if (auto write_error = write_bootstrap()) return {write_error};
        reset_state();
        return {};
    } catch (const std::filesystem::filesystem_error&) {
        return {error(StorageErrorKind::kIoError, "filesystem operation failed during storage cleanup")};
    } catch (...) {
        return {unexpected_exception_error()};
    }
}

ListTablesResult list_tables(const ListTablesRequest&) {
    try {
        if (state.lifecycle != Lifecycle::kOpen) {
            return {{}, error(StorageErrorKind::kInvalidRequest, "storage is not open")};
        }
        return {state.tables, std::nullopt};
    } catch (...) {
        return {{}, unexpected_exception_error()};
    }
}

CreateTableResult create_table(const CreateTableRequest& request) {
    try {
        if (state.lifecycle != Lifecycle::kOpen) {
            return {error(StorageErrorKind::kInvalidRequest, "storage is not open")};
        }
        if (!is_normalized_identifier(request.table_name) || request.columns.empty() ||
            !has_valid_columns(request.columns)) {
            return {error(StorageErrorKind::kInvalidRequest, "invalid table definition")};
        }
        if (is_reserved_system_identity(request.table_id, request.table_name)) {
            return {error(StorageErrorKind::kInvalidRequest, "system table identity is reserved")};
        }
        if (request.columns.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            return {error(StorageErrorKind::kInvalidRequest, "table has too many columns")};
        }
        if (find_table(request.table_id) != nullptr || has_table_name(request.table_name)) {
            return {error(StorageErrorKind::kInvalidRequest, "table already exists")};
        }
        if (state.tables.size() == state.tables.max_size()) {
            return {error(StorageErrorKind::kInvalidRequest, "too many tables")};
        }
        // Reserve before the system-table commit marker is written.  Publishing
        // the TableMeta after a successful marker write must not allocate or throw.
        state.tables.reserve(state.tables.size() + 1);

        auto created_file = state.file_manager->create_table_file(request.table_id);
        if (!created_file.value.has_value()) {
            return {file_error(*created_file.error, true)};
        }

        if (state.cursors->has_cursor(kSystemTablesId) || state.cursors->has_cursor(kSystemColumnsId)) {
            if (const auto rollback_error = state.file_manager->remove_table_file(request.table_id);
                rollback_error.has_value()) {
                return {error(StorageErrorKind::kIoError, "system catalog cursor and table-file rollback failed")};
            }
            return {error(StorageErrorKind::kInvalidRequest, "system catalog has an unclosed cursor")};
        }

        internal::HeapTable columns_table(
            system_columns_meta(), *state.file_manager, *state.buffer_pool);
        std::vector<std::vector<Value>> column_rows;
        column_rows.reserve(request.columns.size());
        for (std::size_t ordinal = 0; ordinal < request.columns.size(); ++ordinal) {
            const auto type = type_name(request.columns[ordinal].type);
            if (!type.has_value()) {
                const auto rollback_error = state.file_manager->remove_table_file(request.table_id);
                return {rollback_error.has_value()
                    ? error(StorageErrorKind::kIoError, "unknown column type and table-file rollback failed")
                    : error(StorageErrorKind::kInvalidRequest, "unknown column type")};
            }
            column_rows.push_back({
                Value{std::to_string(request.table_id)},
                Value{static_cast<std::int32_t>(ordinal)},
                Value{request.columns[ordinal].name},
                Value{std::string{*type}}});
        }
        const auto columns_written = columns_table.insert_batch(column_rows);
        if (columns_written.error.has_value() || columns_written.record_ids.size() != column_rows.size()) {
            const auto rollback_error = rollback_catalog_create(
                columns_table, columns_written.record_ids, *state.file_manager, request.table_id);
            if (rollback_error.has_value()) {
                return {*rollback_error};
            }
            return {columns_written.error.has_value()
                ? data_error(*columns_written.error)
                : error(StorageErrorKind::kCorrupt, "system catalog column write returned an incomplete result")};
        }

        internal::HeapTable tables_table(
            system_tables_meta(), *state.file_manager, *state.buffer_pool);
        const auto table_written = tables_table.insert_record({
            Value{std::to_string(request.table_id)},
            Value{request.table_name},
            Value{static_cast<std::int32_t>(request.columns.size())}});
        if (table_written.error.has_value() || !table_written.value.has_value()) {
            const auto rollback_error = rollback_catalog_create(
                columns_table, columns_written.record_ids, *state.file_manager, request.table_id);
            if (rollback_error.has_value()) {
                return {*rollback_error};
            }
            return {table_written.error.has_value()
                ? data_error(*table_written.error)
                : error(StorageErrorKind::kCorrupt, "system catalog table write returned no record id")};
        }

        const auto marker = internal::RecordIdCodec::decode(*table_written.value);
        if (marker.error.has_value()) {
            return {error(StorageErrorKind::kCorrupt, "system catalog returned an invalid commit marker")};
        }
        if (const auto marker_flush = state.buffer_pool->flush_page(
                {kSystemTablesId, marker.value->page_id}); marker_flush.has_value()) {
            const auto marker_rollback = tables_table.delete_record(*table_written.value);
            const auto columns_rollback = rollback_catalog_create(
                columns_table, columns_written.record_ids, *state.file_manager, request.table_id);
            if (marker_rollback.has_value() || columns_rollback.has_value()) {
                return {error(StorageErrorKind::kIoError, "system catalog commit rollback failed")};
            }
            return {buffer_error(*marker_flush)};
        }

        state.tables.push_back(TableMeta{request.table_id, request.table_name, request.columns});
        return {};
    } catch (const std::filesystem::filesystem_error&) {
        return {error(StorageErrorKind::kIoError,
                      "filesystem operation failed while creating table")};
    } catch (...) {
        return {unexpected_exception_error()};
    }
}

OpenTableResult open_table(const OpenTableRequest& request) {
    try {
        if(auto e=require_data_open())return {std::nullopt,std::move(e)};
        const auto* table=find_table(request.table_id);
        if (table == nullptr) {
            return {std::nullopt, error(StorageErrorKind::kTableNotFound, "table does not exist")};
        }
        auto result=state.cursors->create(*table);
        return {result.value,data_error(result.error)};
    } catch (...) {
        return {std::nullopt, unexpected_exception_error()};
    }
}

ScanNextResult scan_next(const ScanNextRequest& request) {
    try {
        if(auto e=require_data_open(true))return {std::nullopt,std::move(e)};
        // Registry owns the immutable schema copy and constructs its temporary HeapTable.
        auto result=state.cursors->next_record(request.cursor);
        return {std::move(result.value),data_error(result.error)};
    } catch (...) {
        return {std::nullopt, unexpected_exception_error()};
    }
}

CloseCursorResult close_cursor(const CloseCursorRequest& request) {
    try {
        if(auto e=require_data_open(true))return {std::move(e)};
        return {data_error(state.cursors->close(request.cursor))};
    } catch (...) {
        return {unexpected_exception_error()};
    }
}

InsertResult insert(const InsertRequest& request) {
    try {
        if(auto e=require_data_open())return {{},std::move(e)};
        if (is_system_catalog_table(request.table_id)) {
            return {{}, error(StorageErrorKind::kInvalidRequest, "system catalog table is read-only")};
        }
        const auto* table=find_table(request.table_id);
        if(!table)return {{},error(StorageErrorKind::kTableNotFound,"table does not exist")};
        if(request.rows.empty())
            return {{}, error(StorageErrorKind::kInvalidRequest, "insert batch is empty")};
        if(state.cursors->has_cursor(request.table_id))
            return {{},error(StorageErrorKind::kInvalidRequest,"table has an unclosed cursor")};
        internal::HeapTable heap(*table,*state.file_manager,*state.buffer_pool);
        auto result=heap.insert_batch(request.rows);
        return {std::move(result.record_ids),data_error(result.error)};
    } catch (...) {
        return {{}, unexpected_exception_error()};
    }
}

DeleteResult delete_records(const DeleteRequest& request) {
    try {
        if(auto e=require_data_open())return {0,std::move(e)};
        if (is_system_catalog_table(request.table_id)) {
            return {0, error(StorageErrorKind::kInvalidRequest, "system catalog table is read-only")};
        }
        const auto* table=find_table(request.table_id);
        if(!table)return {0,error(StorageErrorKind::kTableNotFound,"table does not exist")};
        if(request.rids.empty())return {};
        if(state.cursors->has_cursor(request.table_id))
            return {0,error(StorageErrorKind::kInvalidRequest,"table has an unclosed cursor")};
        internal::HeapTable heap(*table,*state.file_manager,*state.buffer_pool);
        auto result=heap.delete_batch(request.rids);
        return {result.deleted_count,data_error(result.error)};
    } catch (...) {
        return {0, unexpected_exception_error()};
    }
}

internal::BufferPool* internal::StorageTestAccess::buffer_pool() noexcept {
    return state.lifecycle == Lifecycle::kOpen ? state.buffer_pool.get() : nullptr;
}
internal::FileManager* internal::StorageTestAccess::file_manager() noexcept {
    return state.file_manager.get();
}
bool internal::StorageTestAccess::configure(std::size_t capacity, ReplacementPolicy policy,
                                           BufferPool::WritePage write, BufferPool::ReadPage read) {
    if (state.lifecycle != Lifecycle::kClosed) return false;
    buffer_config = BufferConfig{capacity, policy, std::move(write), std::move(read)};
    return true;
}
void internal::StorageTestAccess::fail_next_file_close() { PageFile::fail_next_close_for_testing_ = true; }

}  // namespace tinydbms::storage
