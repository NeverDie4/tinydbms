#include "tinydbms/storage.hpp"

#include "file_manager.h"
#include "cursor_state.h"
#include "storage_test_access.h"

#include <filesystem>
#include <charconv>
#include <fstream>
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
struct StorageState {
    Lifecycle lifecycle = Lifecycle::kClosed;
    std::filesystem::path data_dir;
    std::vector<TableMeta> tables;
    std::unique_ptr<internal::FileManager> file_manager;
    std::unique_ptr<internal::BufferPool> buffer_pool; // Destroyed before FileManager.
    std::unique_ptr<internal::CursorRegistry> cursors; // Destroyed before borrowed resources.
};

StorageState state;
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

std::optional<std::string_view> type_name(Type type) {
    switch (type) {
        case Type::kInt:
            return "INT32";
        case Type::kVarchar:
            return "VARCHAR";
    }
    return std::nullopt;
}

std::optional<Type> parse_type(std::string_view name) {
    if (name == "INT32" || name == "INT") {
        return Type::kInt;
    }
    if (name == "VARCHAR") {
        return Type::kVarchar;
    }
    return std::nullopt;
}

bool is_normalized_identifier(std::string_view name);
bool has_valid_columns(const std::vector<ColumnMeta>& columns);

bool read_metadata(std::ifstream& input, std::vector<TableMeta>& tables, bool& unsupported_schema) {
    std::string line;
    if (!std::getline(input, line) || line != "TINYDBMS_STORAGE_V1") {
        return false;
    }
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
        TableMeta table;
        if (!(table_stream >> marker >> id_token >> table.table_name) || marker != "TABLE" ||
            (table_stream >> extra) || !is_normalized_identifier(table.table_name)) {
            return false;
        }
        const auto parsed_id=std::from_chars(id_token.data(),id_token.data()+id_token.size(),table.table_id);
        if (parsed_id.ec!=std::errc{} || parsed_id.ptr!=id_token.data()+id_token.size()) return false;
        for (const auto& existing:tables)
            if (existing.table_id==table.table_id || existing.table_name==table.table_name) return false;
        bool ended=false;
        while (std::getline(input, line)) {
            if (line == "ENDTABLE") {
                ended=true;
                break;
            }
            std::istringstream column_stream(line);
            std::string type;
            ColumnMeta column;
            if (!(column_stream >> marker >> type >> column.name) || marker != "COLUMN" || (column_stream >> extra)) {
                return false;
            }
            const auto parsed_type = parse_type(type);
            if (!parsed_type.has_value()) {
                unsupported_schema = type == "INT64" || type == "FLOAT" || type == "DOUBLE" || type == "BOOL";
                return false;
            }
            column.type = *parsed_type;
            table.columns.push_back(std::move(column));
        }
        if (!ended || table.columns.empty() || !has_valid_columns(table.columns)) {
            return false;
        }
        tables.push_back(std::move(table));
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
        output << "TINYDBMS_STORAGE_V1\n";
        for (const TableMeta& table : state.tables) {
            output << "TABLE " << table.table_id << ' ' << table.table_name << "\n";
            for (const ColumnMeta& column : table.columns) {
                const auto name = type_name(column.type);
                if (!name.has_value()) {
                    output.close();
                    std::filesystem::remove(temporary_path, filesystem_error);
                    return error(StorageErrorKind::kInvalidRequest, "unknown column type");
                }
                output << "COLUMN " << *name << ' ' << column.name << "\n";
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
        if (metadata_exists) {
            std::ifstream input(meta);
            if (!input) {
                reset_state();
                return {error(StorageErrorKind::kIoError,"cannot open storage.meta")};
            }
            bool unsupported_schema = false;
            if (!read_metadata(input, state.tables, unsupported_schema)) {
                const bool io_failed=input.bad();
                state.data_dir.clear();
                return {error(io_failed ? StorageErrorKind::kIoError
                                        : unsupported_schema ? StorageErrorKind::kInvalidRequest
                                                             : StorageErrorKind::kCorrupt,
                              io_failed ? "cannot read storage.meta"
                                        : unsupported_schema ? "legacy storage schema uses unsupported type"
                                                             : "invalid storage.meta")};
            }
        } else {
            bool has_entries = false;
            std::filesystem::directory_iterator iterator(data_dir, filesystem_error);
            if (filesystem_error) {
                state.data_dir.clear();
                return {error(StorageErrorKind::kIoError, "cannot inspect data directory")};
            }
            const std::filesystem::directory_iterator end;
            while (iterator != end) {
                has_entries = true;
                iterator.increment(filesystem_error);
                if (filesystem_error) {
                    state.data_dir.clear();
                    return {error(StorageErrorKind::kIoError, "cannot inspect data directory")};
                }
                break;
            }
            if (has_entries) {
                state.data_dir.clear();
                return {error(StorageErrorKind::kCorrupt, "data directory has no storage.meta")};
            }
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
        for (const TableMeta& table : state.tables) {
            auto opened_table = (*file_manager.value)->open_table_file(table.table_id);
            if (!opened_table.value.has_value()) {
                const auto mapped = file_error(*opened_table.error, true);
                reset_state();
                return {mapped};
            }
        }
        auto pool = internal::BufferPool::create(**file_manager.value, buffer_config.capacity,
            buffer_config.read, buffer_config.write, buffer_config.policy);
        if (!pool.value) {
            const auto mapped = buffer_error(*pool.error);
            reset_state();
            return {mapped}; // Local FileManager closes validated files.
        }
        state.file_manager = std::move(*file_manager.value);
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
        if (auto write_error = write_metadata()) return {write_error};
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
        if (find_table(request.table_id) != nullptr || has_table_name(request.table_name)) {
            return {error(StorageErrorKind::kInvalidRequest, "table already exists")};
        }

        auto created_file = state.file_manager->create_table_file(request.table_id);
        if (!created_file.value.has_value()) {
            return {file_error(*created_file.error, true)};
        }

        state.tables.push_back(TableMeta{request.table_id, request.table_name, request.columns});
        if (const auto write_error = write_metadata(); write_error.has_value()) {
            state.tables.pop_back();
            if (const auto rollback_error = state.file_manager->remove_table_file(request.table_id);
                rollback_error.has_value()) {
                return {error(StorageErrorKind::kIoError,
                              "metadata commit and table-file rollback both failed")};
            }
            return {write_error};
        }
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
