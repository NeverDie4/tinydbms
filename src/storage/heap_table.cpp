#include "heap_table.h"
#include <utility>
#include <limits>

namespace tinydbms::storage::internal {
namespace {
HeapTableError translate(const PageFileError& e) {
    switch(e.kind) {
        case PageFileErrorKind::kIo: return {HeapTableErrorKind::kIo,e.message};
        case PageFileErrorKind::kCorrupt: return {HeapTableErrorKind::kCorrupt,e.message};
        case PageFileErrorKind::kInvalidArgument: return {HeapTableErrorKind::kInvalidArgument,e.message};
    }
    return {HeapTableErrorKind::kCorrupt,"unknown PageFile error"};
}
HeapTableError translate(const BufferPoolError& e) {
    switch(e.kind) {
        case BufferPoolErrorKind::kIo: return {HeapTableErrorKind::kIo,e.message};
        case BufferPoolErrorKind::kCorrupt: return {HeapTableErrorKind::kCorrupt,e.message};
        case BufferPoolErrorKind::kInvalidArgument: return {HeapTableErrorKind::kInvalidArgument,e.message};
        case BufferPoolErrorKind::kNoVictim: return {HeapTableErrorKind::kNoVictim,e.message};
    }
    return {HeapTableErrorKind::kCorrupt,"unknown BufferPool error"};
}
HeapTableError translate(const RecordCodecError& e) {
    switch(e.kind) {
        case RecordCodecErrorKind::kInvalidArgument: return {HeapTableErrorKind::kInvalidArgument,e.message};
        case RecordCodecErrorKind::kValueTooLarge: return {HeapTableErrorKind::kValueTooLarge,e.message};
        case RecordCodecErrorKind::kCorrupt: return {HeapTableErrorKind::kCorrupt,e.message};
    }
    return {HeapTableErrorKind::kCorrupt,"unknown RecordCodec error"};
}
HeapTableError translate(const RecordPageError& e) {
    switch(e.kind) {
        case RecordPageErrorKind::kInvalidArgument: return {HeapTableErrorKind::kInvalidArgument,e.message};
        case RecordPageErrorKind::kValueTooLarge: return {HeapTableErrorKind::kValueTooLarge,e.message};
        case RecordPageErrorKind::kCorrupt: return {HeapTableErrorKind::kCorrupt,e.message};
        // Existing-page NoSpace is handled by First-Fit. An initialized empty page
        // must fit every prevalidated row; NoSpace there violates that invariant.
        case RecordPageErrorKind::kNoSpace: return {HeapTableErrorKind::kCorrupt,"new RecordPage unexpectedly has no space"};
    }
    return {HeapTableErrorKind::kCorrupt,"unknown RecordPage error"};
}
}

HeapTable::HeapTable(TableMeta meta, FileManager& files, BufferPool& pool, NewRecordPageIo io)
    : meta_(std::move(meta)), files_(files), pool_(pool), io_(std::move(io)) {}

HeapTableResult<HeapScanPosition> HeapTable::begin_scan() const {
    if (auto error=check_file()) return {std::nullopt,std::move(error)};
    return {HeapScanPosition{1,0,files_.find_table_file(meta_.table_id)->page_count()},std::nullopt};
}
HeapTableResult<Record> HeapTable::next_record(HeapScanPosition& position) {
    if (auto error=check_file()) return {std::nullopt,std::move(error)};
    auto& file=*files_.find_table_file(meta_.table_id);
    constexpr auto max_end=std::uint64_t{std::numeric_limits<PageId>::max()}+1;
    if (position.next_page<1 || position.page_end_exclusive<1 ||
        position.next_page>position.page_end_exclusive || position.page_end_exclusive>max_end ||
        position.page_end_exclusive>file.page_count() || position.next_slot>kMaxSlotsPerPage ||
        (position.next_page==position.page_end_exclusive && position.next_slot!=0))
        return {std::nullopt,HeapTableError{HeapTableErrorKind::kInvalidArgument,"invalid heap scan position"}};
    auto candidate=position;
    while (candidate.next_page<candidate.page_end_exclusive) {
        // The exclusive limit above proves this narrowing is safe.
        const auto id=static_cast<PageId>(candidate.next_page);
        auto state=file.page_allocation_state(id);
        if (state.error) return {std::nullopt,translate(*state.error)};
        if (*state.value==PageAllocationState::kAllocated) {
            auto guard=pool_.fetch_page({meta_.table_id,id});
            if (guard.error) return {std::nullopt,translate(*guard.error)};
            auto next=RecordPage::next_record(guard.value->page(),id,meta_,candidate.next_slot);
            if (next.error) return {std::nullopt,translate(*next.error)};
            if (next.record) {
                candidate.next_slot=next.next_position;
                position=candidate;
                return {std::move(next.record),std::nullopt};
            }
        } // Guard released before moving to another page.
        ++candidate.next_page;
        candidate.next_slot=0;
    }
    position=candidate;
    return {std::nullopt,std::nullopt};
}

std::optional<HeapTableError> HeapTable::check_file() const {
    auto* file = files_.find_table_file(meta_.table_id);
    if (!file) return HeapTableError{HeapTableErrorKind::kInvalidArgument,"HeapTable requires an opened table file"};
    if (auto error = file->require_open()) return translate(*error);
    return std::nullopt;
}

HeapTableResult<RecordId> HeapTable::insert_record(const std::vector<Value>& values) {
    auto size = RecordCodec::encoded_size(meta_,values);
    if (size.error) return {std::nullopt,translate(*size.error)};
    if (*size.value == 0) return {std::nullopt,HeapTableError{HeapTableErrorKind::kInvalidArgument,"empty record schema"}};
    if (auto error = check_file()) return {std::nullopt,std::move(error)};
    auto& file = *files_.find_table_file(meta_.table_id); // Borrow only during this synchronous call.
    // uint64 loop also handles page_count == UINT32_MAX + 1 without wraparound.
    for (std::uint64_t next = 1; next < file.page_count(); ++next) {
        const auto id = static_cast<PageId>(next);
        auto state = file.page_allocation_state(id);
        if (state.error) return {std::nullopt,translate(*state.error)};
        if (*state.value == PageAllocationState::kFree) continue;
        auto guard = pool_.fetch_page({meta_.table_id,id});
        if (guard.error) return {std::nullopt,translate(*guard.error)};
        auto inserted = RecordPage::insert_record(guard.value->mutable_page(),id,meta_,values);
        if (inserted.error) {
            if (inserted.error->kind == RecordPageErrorKind::kNoSpace) continue;
            return {std::nullopt,translate(*inserted.error)};
        }
        guard.value->mark_dirty();
        return {inserted.value,std::nullopt};
        // Guard destruction releases each page, including every continue/error.
    }
    return create_record_page(file,values);
}

HeapTableResult<RecordId> HeapTable::create_record_page(PageFile& file, const std::vector<Value>& values) {
    auto allocated = io_.allocate ? io_.allocate(file) : file.allocate_page();
    if (allocated.error) return {std::nullopt,translate(*allocated.error)};
    const PageId id = *allocated.value;
    auto compensate = [&](HeapTableError original) -> HeapTableResult<RecordId> {
        auto error = io_.compensate_free ? io_.compensate_free(file,id) : file.free_page(id);
        if (error) {
            // All lightweight HeapTable instances see the same file health latch.
            // No on-disk recovery format, and no attempt to hide a damaged allocation.
            file.poisoned_ = true;
            return {std::nullopt,HeapTableError{HeapTableErrorKind::kIo,
                "bootstrap failed: " + original.message + "; compensation failed: " + error->message}};
        }
        return {std::nullopt,std::move(original)};
    };
    RawPage scratch;
    if (auto error = RecordPage::initialize(scratch,id)) return compensate(translate(*error));
    // Sole cache bypass: fresh allocation, never resident, no RID yet published.
    auto error = io_.bootstrap_write ? io_.bootstrap_write(file,id,scratch) : file.write_page(id,scratch);
    if (error) return compensate(translate(*error));

    // From here the valid empty page is retained on every failure, never freed.
    auto guard = pool_.fetch_page({meta_.table_id,id});
    if (guard.error) return {std::nullopt,translate(*guard.error)};
    auto inserted = RecordPage::insert_record(guard.value->mutable_page(),id,meta_,values);
    if (inserted.error) return {std::nullopt,translate(*inserted.error)};
    guard.value->mark_dirty();
    return {inserted.value,std::nullopt};
}

HeapTableBatchResult HeapTable::insert_batch(const std::vector<std::vector<Value>>& rows) {
    if (auto error = check_file()) return {{},std::move(error)};
    HeapTableBatchResult result;
    result.record_ids.reserve(rows.size());
    for (const auto& values : rows) {
        auto inserted = insert_record(values);
        if (inserted.error) { result.error = std::move(inserted.error); break; }
        result.record_ids.push_back(*inserted.value);
    }
    return result;
}
std::optional<HeapTableError> HeapTable::delete_record(RecordId rid) {
    auto parts=RecordIdCodec::decode(rid);
    if(parts.error)return translate(*parts.error);
    if(auto error=check_file())return error;
    auto& file=*files_.find_table_file(meta_.table_id);
    const auto id=parts.value->page_id;
    auto state=file.page_allocation_state(id); // Also checks logical range.
    if(state.error)return translate(*state.error);
    if(*state.value==PageAllocationState::kFree)
        return HeapTableError{HeapTableErrorKind::kInvalidArgument,"RID refers to a free page"};
    auto guard=pool_.fetch_page({meta_.table_id,id});
    if(guard.error)return translate(*guard.error);
    // erase_record validates slots/layout, not encoded values. Validate the target
    // payload through the existing read path before deleting it; no parsing here.
    auto record=RecordPage::get_record(guard.value->page(),id,meta_,rid);
    if(record.error)return translate(*record.error);
    if(auto error=RecordPage::erase_record(guard.value->mutable_page(),id,rid))return translate(*error);
    guard.value->mark_dirty();
    return std::nullopt;
}
HeapDeleteResult HeapTable::delete_batch(const std::vector<RecordId>& record_ids) {
    if(auto error=check_file())return {0,std::move(error)};
    HeapDeleteResult result;
    for(auto rid:record_ids) {
        if(auto error=delete_record(rid)){result.error=std::move(error);break;}
        ++result.deleted_count;
    }
    return result;
}
}
