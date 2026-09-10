#pragma once

#include "buffer_pool.h"
#include "record_page.h"

namespace tinydbms::storage::internal {

enum class HeapTableErrorKind { kInvalidArgument, kValueTooLarge, kNoVictim, kIo, kCorrupt };
struct HeapTableError { HeapTableErrorKind kind; std::string message; };
template <typename T> struct HeapTableResult {
    std::optional<T> value;
    std::optional<HeapTableError> error;
};
struct HeapTableBatchResult {
    std::vector<RecordId> record_ids;
    std::optional<HeapTableError> error;
};
struct HeapDeleteResult {
    std::uint64_t deleted_count = 0;
    std::optional<HeapTableError> error;
};

struct HeapScanPosition {
    std::uint64_t next_page = 1;
    std::size_t next_slot = 0;
    std::uint64_t page_end_exclusive = 1;
    bool operator==(const HeapScanPosition&) const = default;
};

// Narrow synchronous new-page I/O adapters, empty = real PageFile operations.
// Must obey PageFile semantics; no reentry, caching, or mutation of existing pages.
struct NewRecordPageIo {
    std::function<PageFileResult<PageId>(PageFile&)> allocate;
    std::function<std::optional<PageFileError>(PageFile&, PageId, const RawPage&)> bootstrap_write;
    std::function<std::optional<PageFileError>(PageFile&, PageId)> compensate_free;
};

// Resolved immutable schema copy. Owners outlive this object and all its calls.
// Single-threaded; callers must not free/reopen files behind the BufferPool.
class HeapTable {
public:
    HeapTable(TableMeta meta, FileManager& files, BufferPool& pool, NewRecordPageIo io = {});
    HeapTableResult<RecordId> insert_record(const std::vector<Value>& values);
    HeapTableBatchResult insert_batch(const std::vector<std::vector<Value>>& rows);
    std::optional<HeapTableError> delete_record(RecordId rid);
    HeapDeleteResult delete_batch(const std::vector<RecordId>& record_ids);
    HeapTableResult<HeapScanPosition> begin_scan() const;
    // Owned record / empty EOF / error. Position commits only on success or EOF.
    HeapTableResult<Record> next_record(HeapScanPosition& position);

private:
    friend struct HeapTableTestAccess; // Exercise the post-prevalidation failure boundary.
    HeapTableResult<RecordId> create_record_page(PageFile& file, const std::vector<Value>& values);
    std::optional<HeapTableError> check_file() const;
    TableMeta meta_;
    FileManager& files_;
    BufferPool& pool_;
    NewRecordPageIo io_;
};
} // namespace tinydbms::storage::internal
