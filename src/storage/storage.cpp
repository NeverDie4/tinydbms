#include "tinydbms/storage.hpp"

#include "file_manager.h"
#include "cursor_state.h"
#include "storage_test_access.h"

#include <algorithm>
#include <filesystem>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <new>
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
enum class MetadataFormat { kV1, kV2 };
enum class CatalogMode { kLegacy, kSystem };
struct StoredTable {
    TableMeta metadata;
    internal::RowFormat row_format;
};
struct StorageState {
    Lifecycle lifecycle = Lifecycle::kClosed;
    CatalogMode catalog_mode = CatalogMode::kSystem;
    MetadataFormat metadata_format = MetadataFormat::kV2;
    std::filesystem::path data_dir;
    std::vector<StoredTable> tables;
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

bool buffer_event_log_enabled() noexcept {
    const char* value = std::getenv("TINYDBMS_BUFFER_EVENT_LOG");
    return value != nullptr && std::string_view(value) == "1";
}

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
        {"column_count", Type::kInt},
        {"row_format", Type::kVarchar}}};
}

TableMeta system_columns_meta() {
    return TableMeta{kSystemColumnsId, std::string{kSystemColumnsName}, {
        {"table_id", Type::kVarchar},
        {"column_ordinal", Type::kInt},
        {"column_name", Type::kVarchar},
        {"column_type", Type::kVarchar},
        {"nullable", Type::kBoolean}}};
}

std::vector<StoredTable> system_tables() {
    return {
        {system_tables_meta(), internal::RowFormat::kV2},
        {system_columns_meta(), internal::RowFormat::kV2}};
}

bool is_reserved_system_identity(TableId table_id, std::string_view table_name) {
    return table_id == kSystemTablesId || table_id == kSystemColumnsId ||
        table_name == kSystemTablesName || table_name == kSystemColumnsName;
}

bool is_system_catalog_table(TableId table_id) {
    return table_id == kSystemTablesId || table_id == kSystemColumnsId;
}

std::optional<std::string_view> v1_type_name(Type type) {
    switch (type) {
        case Type::kInt:
            return "INT32";
        case Type::kVarchar:
            return "VARCHAR";
        case Type::kBigInt:
        case Type::kDouble:
        case Type::kBoolean:
            return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string_view> v2_type_name(Type type) {
    switch (type) {
        case Type::kInt:
            return "INT32";
        case Type::kBigInt:
            return "INT64";
        case Type::kDouble:
            return "DOUBLE";
        case Type::kBoolean:
            return "BOOLEAN";
        case Type::kVarchar:
            return "VARCHAR";
    }
    return std::nullopt;
}

std::optional<Type> parse_v1_type(std::string_view name) {
    if (name == "INT32" || name == "INT") {
        return Type::kInt;
    }
    if (name == "VARCHAR") {
        return Type::kVarchar;
    }
    return std::nullopt;
}

std::optional<Type> parse_v2_type(std::string_view name) {
    if (name == "INT32") {
        return Type::kInt;
    }
    if (name == "INT64") {
        return Type::kBigInt;
    }
    if (name == "DOUBLE") {
        return Type::kDouble;
    }
    if (name == "BOOLEAN") {
        return Type::kBoolean;
    }
    if (name == "VARCHAR") {
        return Type::kVarchar;
    }
    return std::nullopt;
}

enum class MetadataMarker { kLegacyV1, kLegacyV2, kBootstrap, kInvalid };

MetadataMarker inspect_metadata_marker(std::ifstream& input) {
    std::string line;
    if (!std::getline(input, line)) return MetadataMarker::kInvalid;
    input.clear();
    input.seekg(0);
    if (line == "TINYDBMS_STORAGE_V1") return MetadataMarker::kLegacyV1;
    if (line == "TINYDBMS_STORAGE_V2") return MetadataMarker::kLegacyV2;
    if (line == "TINYDBMS_STORAGE_BOOTSTRAP_V2") return MetadataMarker::kBootstrap;
    return MetadataMarker::kInvalid;
}

bool read_bootstrap(std::ifstream& input) {
    std::string line;
    if (!std::getline(input, line) || line != "TINYDBMS_STORAGE_BOOTSTRAP_V2") return false;
    bool saw_tables = false;
    bool saw_columns = false;
    while (std::getline(input, line)) {
        if (line == "END") {
            while (std::getline(input, line))
                if (line.find_first_not_of(" \t\r") != std::string::npos) return false;
            return !input.bad() && saw_tables && saw_columns;
        }
        if (line == "SYS_TABLES 0" && !saw_tables) saw_tables = true;
        else if (line == "SYS_COLUMNS 1" && !saw_columns) saw_columns = true;
        else return false;
    }
    return false;
}

std::optional<StorageError> replace_metadata_file(
    const std::filesystem::path& temporary_path, std::string_view replace_error) {
    std::error_code filesystem_error;
#ifdef _WIN32
    if (MoveFileExW(temporary_path.c_str(), metadata_path().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        std::filesystem::remove(temporary_path, filesystem_error);
        return error(StorageErrorKind::kIoError, std::string{replace_error});
    }
#else
    std::filesystem::rename(temporary_path, metadata_path(), filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(temporary_path, filesystem_error);
        return error(StorageErrorKind::kIoError, std::string{replace_error});
    }
#endif
    return std::nullopt;
}

std::optional<StorageError> write_bootstrap() {
    const auto temporary_path = temporary_metadata_path();
    std::error_code filesystem_error;
    std::filesystem::remove(temporary_path, filesystem_error);
    if (filesystem_error)
        return error(StorageErrorKind::kIoError, "cannot remove temporary storage metadata");
    {
        std::ofstream output(temporary_path, std::ios::trunc);
        if (!output)
            return error(StorageErrorKind::kIoError, "cannot write temporary storage metadata");
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
    return replace_metadata_file(temporary_path, "cannot replace storage bootstrap metadata");
}

bool is_normalized_identifier(std::string_view name);
bool has_valid_columns(
    const std::vector<ColumnMeta>& columns, internal::RowFormat row_format);

bool read_metadata(
    std::ifstream& input, std::vector<StoredTable>& tables,
    MetadataFormat& metadata_format, bool& unsupported_schema) {
    std::string line;
    if (!std::getline(input, line)) {
        return false;
    }
    const bool is_v1 = line == "TINYDBMS_STORAGE_V1";
    if (!is_v1 && line != "TINYDBMS_STORAGE_V2") return false;
    metadata_format = is_v1 ? MetadataFormat::kV1 : MetadataFormat::kV2;
    while (std::getline(input, line)) {
        if (line == "END") {
            // Only whitespace may follow the final marker; never ignore extra tables/data.
            while (std::getline(input,line)) {
                if (line.find_first_not_of(" \t\r") != std::string::npos) return false;
            }
            return !input.bad();
        }
        std::istringstream table_stream(line);
        std::string marker;
        std::string id_token, extra;
        std::string row_format_token;
        TableMeta table;
        if (!(table_stream >> marker >> id_token >> table.table_name) || marker != "TABLE" ||
            (!is_v1 && !(table_stream >> row_format_token)) || (table_stream >> extra) ||
            !is_normalized_identifier(table.table_name)) {
            return false;
        }
        internal::RowFormat row_format = internal::RowFormat::kV1;
        if (!is_v1) {
            if (row_format_token == "V2") row_format = internal::RowFormat::kV2;
            else if (row_format_token != "V1") return false;
        }
        const auto parsed_id=std::from_chars(id_token.data(),id_token.data()+id_token.size(),table.table_id);
        if (parsed_id.ec!=std::errc{} || parsed_id.ptr!=id_token.data()+id_token.size()) return false;
        for (const auto& existing:tables)
            if (existing.metadata.table_id==table.table_id ||
                existing.metadata.table_name==table.table_name) return false;
        bool ended=false;
        while (std::getline(input, line)) {
            if (line == "ENDTABLE") {
                ended=true;
                break;
            }
            std::istringstream column_stream(line);
            std::string type;
            std::string nullability;
            ColumnMeta column;
            if (!(column_stream >> marker >> type) || marker != "COLUMN" ||
                (!is_v1 && !(column_stream >> nullability)) ||
                !(column_stream >> column.name) || (column_stream >> extra)) {
                return false;
            }
            const auto parsed_type = is_v1 ? parse_v1_type(type) : parse_v2_type(type);
            if (!parsed_type.has_value()) {
                unsupported_schema = is_v1 &&
                    (type == "INT64" || type == "FLOAT" || type == "DOUBLE" ||
                     type == "BOOL" || type == "BOOLEAN");
                return false;
            }
            column.type = *parsed_type;
            if (!is_v1) {
                if (nullability == "NULLABLE") column.nullable = true;
                else if (nullability != "NOT_NULL") return false;
            }
            table.columns.push_back(std::move(column));
        }
        if (!ended || table.columns.empty()) {
            return false;
        }
        bool schema_supported = true;
        for (const ColumnMeta& column : table.columns) {
            const bool type_supported = column.type == Type::kInt ||
                column.type == Type::kVarchar ||
                (row_format == internal::RowFormat::kV2 &&
                 (column.type == Type::kBigInt || column.type == Type::kDouble ||
                  column.type == Type::kBoolean));
            if ((row_format == internal::RowFormat::kV1 && column.nullable) ||
                !type_supported) {
                schema_supported = false;
            }
        }
        if (!schema_supported || !has_valid_columns(table.columns,row_format)) {
            unsupported_schema = !schema_supported;
            return false;
        }
        tables.push_back(StoredTable{std::move(table),row_format});
    }
    return false;
}

std::optional<StorageError> write_metadata() {
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
        output << (state.metadata_format == MetadataFormat::kV1
                       ? "TINYDBMS_STORAGE_V1\n"
                       : "TINYDBMS_STORAGE_V2\n");
        for (const StoredTable& stored : state.tables) {
            const TableMeta& table = stored.metadata;
            output << "TABLE " << table.table_id << ' ' << table.table_name;
            if (state.metadata_format == MetadataFormat::kV2) {
                output << (stored.row_format == internal::RowFormat::kV1 ? " V1\n" : " V2\n");
            } else {
                output << '\n';
            }
            for (const ColumnMeta& column : table.columns) {
                const auto name = state.metadata_format == MetadataFormat::kV1
                    ? v1_type_name(column.type) : v2_type_name(column.type);
                if (!name.has_value()) {
                    output.close();
                    std::filesystem::remove(temporary_path, filesystem_error);
                    return error(StorageErrorKind::kInvalidRequest, "unknown column type");
                }
                output << "COLUMN " << *name;
                if (state.metadata_format == MetadataFormat::kV2) {
                    output << (column.nullable ? " NULLABLE " : " NOT_NULL ");
                } else {
                    output << ' ';
                }
                output << column.name << "\n";
            }
            output << "ENDTABLE\n";
        }
        output << "END\n";
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
        return error(StorageErrorKind::kIoError, "cannot replace storage.meta");
    }
#else
    std::filesystem::rename(temporary_path, metadata_path(), filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(temporary_path, filesystem_error);
        return error(StorageErrorKind::kIoError, "cannot replace storage.meta");
    }
#endif
    return std::nullopt;
}

bool has_table_name(std::string_view name) {
    for (const StoredTable& table : state.tables) {
        if (table.metadata.table_name == name) {
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

bool has_valid_columns(
    const std::vector<ColumnMeta>& columns, internal::RowFormat row_format) {
    for (std::size_t index = 0; index < columns.size(); ++index) {
        const Type type = columns[index].type;
        const bool supported = type == Type::kInt || type == Type::kVarchar ||
            (row_format == internal::RowFormat::kV2 &&
             (type == Type::kBigInt || type == Type::kDouble ||
              type == Type::kBoolean));
        if (!is_normalized_identifier(columns[index].name) ||
            (row_format == internal::RowFormat::kV1 && columns[index].nullable) ||
            !supported) {
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

const StoredTable* find_table(TableId table_id) {
    for (const StoredTable& table : state.tables) {
        if (table.metadata.table_id == table_id) {
            return &table;
        }
    }
    return nullptr;
}

std::optional<TableId> parse_catalog_table_id(const Value& value) {
    const auto* text = std::get_if<std::string>(&value.data);
    if (text == nullptr || text->empty() || (text->size() > 1 && text->front() == '0'))
        return std::nullopt;
    TableId table_id = 0;
    const auto parsed = std::from_chars(text->data(), text->data() + text->size(), table_id);
    if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size())
        return std::nullopt;
    return table_id;
}

struct PendingCatalogTable {
    std::string name;
    std::int32_t column_count = 0;
    internal::RowFormat row_format = internal::RowFormat::kV2;
    std::map<std::int32_t, ColumnMeta> columns;
};

std::optional<StorageError> scan_catalog_records(
    const TableMeta& metadata, internal::FileManager& file_manager,
    internal::BufferPool& buffer_pool, std::vector<Record>& records) {
    internal::HeapTable table(
        metadata, internal::RowFormat::kV2, file_manager, buffer_pool);
    auto position = table.begin_scan();
    if (!position.value.has_value()) return data_error(*position.error);
    while (true) {
        auto next = table.next_record(*position.value);
        if (next.error.has_value()) return data_error(*next.error);
        if (!next.value.has_value()) return std::nullopt;
        records.push_back(std::move(*next.value));
    }
}

std::optional<StorageError> load_user_catalog(
    internal::FileManager& file_manager, internal::BufferPool& buffer_pool,
    std::vector<StoredTable>& tables) {
    std::vector<Record> table_records;
    std::vector<Record> column_records;
    if (auto scan_error = scan_catalog_records(
            system_tables_meta(), file_manager, buffer_pool, table_records)) return scan_error;
    if (auto scan_error = scan_catalog_records(
            system_columns_meta(), file_manager, buffer_pool, column_records)) return scan_error;

    std::map<TableId, PendingCatalogTable> pending;
    for (const Record& record : table_records) {
        if (record.values.size() != 4)
            return error(StorageErrorKind::kCorrupt, "invalid tdb_sys_tables record");
        const auto table_id = parse_catalog_table_id(record.values[0]);
        const auto* name = std::get_if<std::string>(&record.values[1].data);
        const auto* column_count = std::get_if<std::int32_t>(&record.values[2].data);
        const auto* row_format_name = std::get_if<std::string>(&record.values[3].data);
        const auto row_format = row_format_name != nullptr && *row_format_name == "V1"
            ? std::optional{internal::RowFormat::kV1}
            : row_format_name != nullptr && *row_format_name == "V2"
                ? std::optional{internal::RowFormat::kV2} : std::nullopt;
        if (!table_id.has_value() || name == nullptr || column_count == nullptr ||
            *column_count <= 0 || !row_format.has_value() ||
            is_reserved_system_identity(*table_id, *name) ||
            !is_normalized_identifier(*name) || pending.contains(*table_id))
            return error(StorageErrorKind::kCorrupt, "invalid tdb_sys_tables record");
        for (const auto& [existing_id, existing] : pending) {
            static_cast<void>(existing_id);
            if (existing.name == *name)
                return error(StorageErrorKind::kCorrupt, "duplicate tdb_sys_tables name");
        }
        pending.emplace(*table_id, PendingCatalogTable{*name, *column_count, *row_format, {}});
    }

    for (const Record& record : column_records) {
        if (record.values.size() != 5)
            return error(StorageErrorKind::kCorrupt, "invalid tdb_sys_columns record");
        const auto table_id = parse_catalog_table_id(record.values[0]);
        const auto* ordinal = std::get_if<std::int32_t>(&record.values[1].data);
        const auto* name = std::get_if<std::string>(&record.values[2].data);
        const auto* type_text = std::get_if<std::string>(&record.values[3].data);
        const auto* nullable = std::get_if<bool>(&record.values[4].data);
        const auto type = type_text == nullptr ? std::nullopt : parse_v2_type(*type_text);
        auto table = table_id.has_value() ? pending.find(*table_id) : pending.end();
        if (!table_id.has_value() || ordinal == nullptr || name == nullptr ||
            nullable == nullptr || *ordinal < 0 ||
            is_reserved_system_identity(*table_id, "") ||
            !is_normalized_identifier(*name) || !type.has_value() || table == pending.end() ||
            !table->second.columns.emplace(
                *ordinal, ColumnMeta{*name, *type, *nullable}).second)
            return error(StorageErrorKind::kCorrupt, "invalid tdb_sys_columns record");
    }

    tables = system_tables();
    for (auto& [table_id, pending_table] : pending) {
        if (pending_table.columns.size() !=
            static_cast<std::size_t>(pending_table.column_count))
            return error(StorageErrorKind::kCorrupt,
                         "tdb_sys_columns count does not match tdb_sys_tables");
        TableMeta metadata{table_id, std::move(pending_table.name), {}};
        metadata.columns.reserve(pending_table.columns.size());
        for (std::int32_t ordinal = 0; ordinal < pending_table.column_count; ++ordinal) {
            const auto column = pending_table.columns.find(ordinal);
            if (column == pending_table.columns.end())
                return error(StorageErrorKind::kCorrupt,
                             "tdb_sys_columns ordinal is not contiguous");
            metadata.columns.push_back(std::move(column->second));
        }
        if (!has_valid_columns(metadata.columns, pending_table.row_format))
            return error(StorageErrorKind::kCorrupt,
                         "invalid user table schema in system catalog");
        tables.push_back(StoredTable{std::move(metadata), pending_table.row_format});
    }
    return std::nullopt;
}

std::optional<StorageError> rollback_catalog_create(
    internal::HeapTable& columns_table, const std::vector<RecordId>& column_records,
    internal::FileManager& file_manager, TableId table_id) {
    bool rollback_failed = false;
    if (!column_records.empty()) {
        const auto deleted = columns_table.delete_batch(column_records);
        rollback_failed = deleted.error.has_value() ||
            deleted.deleted_count != column_records.size();
    }
    if (const auto removed = file_manager.remove_table_file(table_id); removed.has_value())
        rollback_failed = true;
    return rollback_failed
        ? std::optional{error(StorageErrorKind::kIoError, "system catalog rollback failed")}
        : std::nullopt;
}

std::optional<TableId> parse_canonical_table_file_name(std::string_view filename) {
    constexpr std::string_view prefix = "table_";
    constexpr std::string_view suffix = ".dat";
    if (!filename.starts_with(prefix) || !filename.ends_with(suffix) ||
        filename.size() == prefix.size() + suffix.size()) return std::nullopt;
    const std::string_view id_text = filename.substr(
        prefix.size(), filename.size() - prefix.size() - suffix.size());
    TableId table_id{};
    const auto parsed = std::from_chars(id_text.data(), id_text.data() + id_text.size(), table_id);
    if (parsed.ec != std::errc{} || parsed.ptr != id_text.data() + id_text.size() ||
        filename != "table_" + std::to_string(table_id) + ".dat") return std::nullopt;
    return table_id;
}

std::optional<StorageError> validate_catalog_table_files(
    const std::vector<StoredTable>& tables) {
    std::error_code filesystem_error;
    std::filesystem::directory_iterator iterator(state.data_dir / "tables", filesystem_error);
    if (filesystem_error)
        return error(StorageErrorKind::kIoError, "cannot inspect tables directory");
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        const auto table_id = parse_canonical_table_file_name(iterator->path().filename().string());
        const bool catalog_has_file = table_id.has_value() && std::any_of(
            tables.begin(), tables.end(), [&](const StoredTable& table) {
                return table.metadata.table_id == *table_id;
            });
        if (table_id.has_value() && !catalog_has_file)
            return error(StorageErrorKind::kCorrupt,
                         "orphan table PageFile is not in system catalog");
        iterator.increment(filesystem_error);
        if (filesystem_error)
            return error(StorageErrorKind::kIoError, "cannot inspect tables directory");
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
        bool initializing_system_catalog = false;
        if (metadata_exists) {
            std::ifstream input(meta);
            if (!input) {
                reset_state();
                return {error(StorageErrorKind::kIoError,"cannot open storage.meta")};
            }
            const MetadataMarker marker = inspect_metadata_marker(input);
            if (marker == MetadataMarker::kBootstrap) {
                state.catalog_mode = CatalogMode::kSystem;
                if (!read_bootstrap(input)) {
                    const bool io_failed = input.bad();
                    reset_state();
                    return {error(io_failed ? StorageErrorKind::kIoError
                                            : StorageErrorKind::kCorrupt,
                                  io_failed ? "cannot read storage.meta"
                                            : "invalid storage bootstrap metadata")};
                }
                state.tables = system_tables();
            } else if (marker == MetadataMarker::kLegacyV1 ||
                       marker == MetadataMarker::kLegacyV2) {
                state.catalog_mode = CatalogMode::kLegacy;
                bool unsupported_schema = false;
                if (!read_metadata(
                        input, state.tables, state.metadata_format, unsupported_schema)) {
                    const bool io_failed = input.bad();
                    reset_state();
                    return {error(io_failed ? StorageErrorKind::kIoError
                                            : unsupported_schema
                                                ? StorageErrorKind::kInvalidRequest
                                                : StorageErrorKind::kCorrupt,
                                  io_failed ? "cannot read storage.meta"
                                            : unsupported_schema
                                                ? "legacy storage schema uses unsupported type"
                                                : "invalid storage.meta")};
                }
            } else {
                reset_state();
                return {error(StorageErrorKind::kCorrupt, "invalid storage.meta")};
            }
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
            state.catalog_mode = CatalogMode::kSystem;
            initializing_system_catalog = true;
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

        if (state.catalog_mode == CatalogMode::kSystem) {
            if (initializing_system_catalog) {
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
        } else {
            for (const StoredTable& table : state.tables) {
                auto opened_table =
                    (*file_manager.value)->open_table_file(table.metadata.table_id);
                if (!opened_table.value.has_value()) {
                    const auto mapped = file_error(*opened_table.error, true);
                    reset_state();
                    return {mapped};
                }
            }
        }
        internal::BufferPool::LogSink log;
        if (buffer_event_log_enabled()) {
            log = [](std::string_view event) { std::clog << event << '\n'; };
        }
        auto pool = internal::BufferPool::create(**file_manager.value, buffer_config.capacity,
            buffer_config.read, buffer_config.write, buffer_config.policy, std::move(log));
        if (!pool.value) {
            const auto mapped = buffer_error(*pool.error);
            if (initializing_system_catalog) rollback_initialization();
            reset_state();
            return {mapped}; // Local FileManager closes validated files.
        }
        state.file_manager = std::move(*file_manager.value);
        if (state.catalog_mode == CatalogMode::kSystem) {
            if (const auto catalog_error = load_user_catalog(
                    *state.file_manager, **pool.value, state.tables);
                catalog_error.has_value()) {
                pool.value.reset();
                if (initializing_system_catalog) rollback_initialization();
                reset_state();
                return {*catalog_error};
            }
            for (std::size_t index = 2; index < state.tables.size(); ++index) {
                auto opened_table = state.file_manager->open_table_file(
                    state.tables[index].metadata.table_id);
                if (!opened_table.value.has_value()) {
                    const auto mapped = file_error(*opened_table.error, true);
                    pool.value.reset();
                    if (initializing_system_catalog) rollback_initialization();
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
        const auto metadata_error = state.catalog_mode == CatalogMode::kSystem
            ? write_bootstrap() : write_metadata();
        if (metadata_error.has_value()) return {metadata_error};
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
        std::vector<TableMeta> tables;
        tables.reserve(state.tables.size());
        for (const StoredTable& table : state.tables) tables.push_back(table.metadata);
        return {std::move(tables), std::nullopt};
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
            !has_valid_columns(request.columns,internal::RowFormat::kV2)) {
            return {error(StorageErrorKind::kInvalidRequest, "invalid table definition")};
        }
        if (state.catalog_mode == CatalogMode::kSystem &&
            is_reserved_system_identity(request.table_id, request.table_name)) {
            return {error(StorageErrorKind::kInvalidRequest, "system table identity is reserved")};
        }
        if (find_table(request.table_id) != nullptr || has_table_name(request.table_name)) {
            return {error(StorageErrorKind::kInvalidRequest, "table already exists")};
        }

        if (state.catalog_mode == CatalogMode::kLegacy) {
            auto created_file = state.file_manager->create_table_file(request.table_id);
            if (!created_file.value.has_value()) return {file_error(*created_file.error, true)};
            const MetadataFormat previous_format = state.metadata_format;
            state.metadata_format = MetadataFormat::kV2;
            state.tables.push_back(StoredTable{
                TableMeta{request.table_id, request.table_name, request.columns},
                internal::RowFormat::kV2});
            if (const auto write_error = write_metadata(); write_error.has_value()) {
                state.tables.pop_back();
                state.metadata_format = previous_format;
                if (const auto rollback_error = state.file_manager->remove_table_file(request.table_id);
                    rollback_error.has_value()) {
                    return {error(StorageErrorKind::kIoError,
                                  "metadata commit and table-file rollback both failed")};
                }
                return {write_error};
            }
            return {};
        }

        if (request.columns.size() >
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
            return {error(StorageErrorKind::kInvalidRequest, "table has too many columns")};
        if (state.tables.size() == state.tables.max_size())
            return {error(StorageErrorKind::kInvalidRequest, "too many tables")};
        state.tables.reserve(state.tables.size() + 1);

        auto created_file = state.file_manager->create_table_file(request.table_id);
        if (!created_file.value.has_value()) {
            return {file_error(*created_file.error, true)};
        }

        if (state.cursors->has_cursor(kSystemTablesId) ||
            state.cursors->has_cursor(kSystemColumnsId)) {
            if (const auto rollback_error = state.file_manager->remove_table_file(request.table_id);
                rollback_error.has_value()) {
                return {error(StorageErrorKind::kIoError,
                              "system catalog cursor and table-file rollback failed")};
            }
            return {error(StorageErrorKind::kInvalidRequest,
                          "system catalog has an unclosed cursor")};
        }

        internal::HeapTable columns_table(
            system_columns_meta(), internal::RowFormat::kV2,
            *state.file_manager, *state.buffer_pool);
        std::vector<std::vector<Value>> column_rows;
        column_rows.reserve(request.columns.size());
        for (std::size_t ordinal = 0; ordinal < request.columns.size(); ++ordinal) {
            const auto type = v2_type_name(request.columns[ordinal].type);
            if (!type.has_value()) {
                const auto rollback_error = state.file_manager->remove_table_file(request.table_id);
                return {rollback_error.has_value()
                    ? error(StorageErrorKind::kIoError,
                            "unknown column type and table-file rollback failed")
                    : error(StorageErrorKind::kInvalidRequest, "unknown column type")};
            }
            column_rows.push_back({
                Value{std::to_string(request.table_id)},
                Value{static_cast<std::int32_t>(ordinal)},
                Value{request.columns[ordinal].name},
                Value{std::string{*type}},
                Value{request.columns[ordinal].nullable}});
        }
        const auto columns_written = columns_table.insert_batch(column_rows);
        if (columns_written.error.has_value() ||
            columns_written.record_ids.size() != column_rows.size()) {
            const auto rollback_error = rollback_catalog_create(
                columns_table, columns_written.record_ids, *state.file_manager,
                request.table_id);
            if (rollback_error.has_value()) return {*rollback_error};
            return {columns_written.error.has_value()
                ? data_error(*columns_written.error)
                : error(StorageErrorKind::kCorrupt,
                        "system catalog column write returned an incomplete result")};
        }

        internal::HeapTable tables_table(
            system_tables_meta(), internal::RowFormat::kV2,
            *state.file_manager, *state.buffer_pool);
        const auto table_written = tables_table.insert_record({
            Value{std::to_string(request.table_id)},
            Value{request.table_name},
            Value{static_cast<std::int32_t>(request.columns.size())},
            Value{std::string{"V2"}}});
        if (table_written.error.has_value() || !table_written.value.has_value()) {
            const auto rollback_error = rollback_catalog_create(
                columns_table, columns_written.record_ids, *state.file_manager,
                request.table_id);
            if (rollback_error.has_value()) return {*rollback_error};
            return {table_written.error.has_value()
                ? data_error(*table_written.error)
                : error(StorageErrorKind::kCorrupt,
                        "system catalog table write returned no record id")};
        }

        const auto marker = internal::RecordIdCodec::decode(*table_written.value);
        if (marker.error.has_value())
            return {error(StorageErrorKind::kCorrupt,
                          "system catalog returned an invalid commit marker")};
        if (const auto marker_flush = state.buffer_pool->flush_page(
                {kSystemTablesId, marker.value->page_id}); marker_flush.has_value()) {
            const auto marker_rollback = tables_table.delete_record(*table_written.value);
            const auto columns_rollback = rollback_catalog_create(
                columns_table, columns_written.record_ids, *state.file_manager,
                request.table_id);
            if (marker_rollback.has_value() || columns_rollback.has_value())
                return {error(StorageErrorKind::kIoError,
                              "system catalog commit rollback failed")};
            return {buffer_error(*marker_flush)};
        }

        state.tables.push_back(StoredTable{
            TableMeta{request.table_id, request.table_name, request.columns},
            internal::RowFormat::kV2});
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
        auto result=state.cursors->create(table->metadata,table->row_format);
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
        const auto* table=find_table(request.table_id);
        if(!table)return {{},error(StorageErrorKind::kTableNotFound,"table does not exist")};
        if(state.catalog_mode == CatalogMode::kSystem && is_system_catalog_table(request.table_id))
            return {{},error(StorageErrorKind::kInvalidRequest,"system table is read-only")};
        if(request.rows.empty())
            return {{}, error(StorageErrorKind::kInvalidRequest, "insert batch is empty")};
        if(state.cursors->has_cursor(request.table_id))
            return {{},error(StorageErrorKind::kInvalidRequest,"table has an unclosed cursor")};
        internal::HeapTable heap(
            table->metadata,table->row_format,*state.file_manager,*state.buffer_pool);
        auto result=heap.insert_batch(request.rows);
        return {std::move(result.record_ids),data_error(result.error)};
    } catch (...) {
        return {{}, unexpected_exception_error()};
    }
}

DeleteResult delete_records(const DeleteRequest& request) {
    try {
        if(auto e=require_data_open())return {0,std::move(e)};
        const auto* table=find_table(request.table_id);
        if(!table)return {0,error(StorageErrorKind::kTableNotFound,"table does not exist")};
        if(state.catalog_mode == CatalogMode::kSystem && is_system_catalog_table(request.table_id))
            return {0,error(StorageErrorKind::kInvalidRequest,"system table is read-only")};
        if(request.rids.empty())return {};
        if(state.cursors->has_cursor(request.table_id))
            return {0,error(StorageErrorKind::kInvalidRequest,"table has an unclosed cursor")};
        internal::HeapTable heap(
            table->metadata,table->row_format,*state.file_manager,*state.buffer_pool);
        auto result=heap.delete_batch(request.rids);
        return {result.deleted_count,data_error(result.error)};
    } catch (...) {
        return {0, unexpected_exception_error()};
    }
}

UpdateResult update_rows(const UpdateRequest& request) {
    try {
        if(auto e=require_data_open())return {0,std::move(e)};
        const auto* table=find_table(request.table_id);
        if(!table)return {0,error(StorageErrorKind::kTableNotFound,"table does not exist")};
        if(state.catalog_mode == CatalogMode::kSystem && is_system_catalog_table(request.table_id))
            return {0,error(StorageErrorKind::kInvalidRequest,"system table is read-only")};
        if(request.rows.empty())return {};
        if(state.cursors->has_cursor(request.table_id))
            return {0,error(StorageErrorKind::kInvalidRequest,"table has an unclosed cursor")};
        internal::HeapTable heap(
            table->metadata,table->row_format,*state.file_manager,*state.buffer_pool);
        auto result=heap.update_batch(request.rows);
        return {result.updated_count,data_error(result.error)};
    } catch (...) {
        return {0,unexpected_exception_error()};
    }
}

double StorageStats::hit_rate() const noexcept {
    if (fetch_count == 0) return 0.0;
    return static_cast<double>(hit_count) / static_cast<double>(fetch_count);
}

StorageStatsResult storage_stats(const StorageStatsRequest&) {
    try {
        // Closing 期间不返回快照：pool 可能已被收尾，生命周期语义只承认 Open。
        if (state.lifecycle != Lifecycle::kOpen || state.buffer_pool == nullptr) {
            return {std::nullopt, error(StorageErrorKind::kInvalidRequest, "storage is not open")};
        }
        const internal::BufferPoolStats snapshot = state.buffer_pool->stats();
        return {
            StorageStats{
                .fetch_count = snapshot.fetch_count,
                .hit_count = snapshot.hit_count,
                .miss_count = snapshot.miss_count,
                .eviction_count = snapshot.eviction_count,
                .dirty_flush_count = snapshot.dirty_flush_count},
            std::nullopt};
    } catch (...) {
        return {std::nullopt, unexpected_exception_error()};
    }
}

internal::BufferPool* internal::StorageTestAccess::buffer_pool() noexcept {
    return state.lifecycle == Lifecycle::kOpen ? state.buffer_pool.get() : nullptr;
}
internal::FileManager* internal::StorageTestAccess::file_manager() noexcept {
    return state.lifecycle == Lifecycle::kClosed ? nullptr : state.file_manager.get();
}
bool internal::StorageTestAccess::configure(std::size_t capacity, ReplacementPolicy policy,
                                           BufferPool::WritePage write, BufferPool::ReadPage read) {
    if (state.lifecycle != Lifecycle::kClosed) return false;
    buffer_config = BufferConfig{capacity, policy, std::move(write), std::move(read)};
    return true;
}
void internal::StorageTestAccess::fail_next_file_close() { PageFile::fail_next_close_for_testing_ = true; }

}  // namespace tinydbms::storage
