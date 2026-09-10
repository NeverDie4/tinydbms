#pragma once
#include "heap_table.h"

namespace tinydbms::storage::internal {
enum class CursorStatus { kActive, kEof, kFailed };
enum class CursorErrorKind { kCursorInvalid, kInvalidArgument, kValueTooLarge, kNoVictim, kIo, kCorrupt };
struct CursorError { CursorErrorKind kind; std::string message; };
template <typename T> struct CursorResult {
    std::optional<T> value;
    std::optional<CursorError> error;
};
struct CursorState {
    TableId table_id;
    HeapScanPosition position;
    CursorStatus status=CursorStatus::kActive;
    std::optional<CursorError> saved_error;
};

// Owned by StorageState. Neither guards nor file/table pointers live in entries.
// Single-threaded; owners must outlive this registry. clear() invalidates all IDs.
class CursorRegistry {
public:
    CursorRegistry(FileManager& files, BufferPool& pool):files_(files),pool_(pool){}
    CursorRegistry(const CursorRegistry&)=delete;
    CursorRegistry& operator=(const CursorRegistry&)=delete;
    CursorRegistry(CursorRegistry&&)=delete;
    CursorRegistry& operator=(CursorRegistry&&)=delete;
    CursorResult<CursorId> create(const TableMeta& meta);
    CursorResult<CursorState> lookup(CursorId id) const; // Owned diagnostic snapshot.
    CursorResult<Record> next_record(CursorId id);
    std::optional<CursorError> close(CursorId id);
    void clear() noexcept;
    bool has_cursor(TableId id) const noexcept; // Includes Eof/Failed until close.
private:
    friend struct CursorRegistryTestAccess;
    static std::optional<CursorId>& next_id(); // Process lifetime, never reset by registry.
    struct Entry { TableMeta meta; CursorState state; };
    FileManager& files_;
    BufferPool& pool_;
    std::unordered_map<CursorId,Entry> entries_;
};
}
