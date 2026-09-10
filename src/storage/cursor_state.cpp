#include "cursor_state.h"
#include <limits>
#include <utility>
namespace tinydbms::storage::internal {
namespace {
CursorError invalid_cursor(){return {CursorErrorKind::kCursorInvalid,"cursor does not exist"};}
CursorError map(const HeapTableError& e) {
    switch(e.kind) {
        case HeapTableErrorKind::kInvalidArgument:return {CursorErrorKind::kInvalidArgument,e.message};
        case HeapTableErrorKind::kValueTooLarge:return {CursorErrorKind::kValueTooLarge,e.message};
        case HeapTableErrorKind::kNoVictim:return {CursorErrorKind::kNoVictim,e.message};
        case HeapTableErrorKind::kIo:return {CursorErrorKind::kIo,e.message};
        case HeapTableErrorKind::kCorrupt:return {CursorErrorKind::kCorrupt,e.message};
    }
    return {CursorErrorKind::kCorrupt,"unknown HeapTable error"};
}
}
CursorResult<CursorId> CursorRegistry::create(const TableMeta& meta) {
    HeapTable table(meta,files_,pool_);
    auto position=table.begin_scan();
    if(position.error)return {std::nullopt,map(*position.error)};
    auto& next=next_id();
    if(!next)return {std::nullopt,CursorError{CursorErrorKind::kInvalidArgument,"CursorId space exhausted"}};
    const CursorId id=*next;
    // Successful insertion precedes issuance. Optional exhaustion is not an ID sentinel.
    entries_.emplace(id,Entry{meta,CursorState{meta.table_id,*position.value,CursorStatus::kActive,std::nullopt}});
    if(id==std::numeric_limits<CursorId>::max())next.reset();else next=id+1;
    return {id,std::nullopt};
}
CursorResult<CursorState> CursorRegistry::lookup(CursorId id) const {
    auto found=entries_.find(id);
    if(found==entries_.end())return {std::nullopt,invalid_cursor()};
    return {found->second.state,std::nullopt};
}
CursorResult<Record> CursorRegistry::next_record(CursorId id) {
    auto found=entries_.find(id);
    if(found==entries_.end())return {std::nullopt,invalid_cursor()};
    auto& entry=found->second;
    auto& state=entry.state;
    if(state.status==CursorStatus::kEof)return {std::nullopt,std::nullopt};
    if(state.status==CursorStatus::kFailed)return {std::nullopt,state.saved_error};
    HeapTable table(entry.meta,files_,pool_);
    auto record=table.next_record(state.position);
    if(record.error) {
        state.saved_error=map(*record.error);
        state.status=CursorStatus::kFailed;
        return {std::nullopt,state.saved_error};
    }
    if(!record.value)state.status=CursorStatus::kEof;
    return {std::move(record.value),std::nullopt};
}
std::optional<CursorError> CursorRegistry::close(CursorId id) {
    if(entries_.erase(id)==0)return invalid_cursor();
    return std::nullopt;
}
void CursorRegistry::clear() noexcept {entries_.clear();}
bool CursorRegistry::has_cursor(TableId id) const noexcept {
    for(const auto& [key,entry]:entries_)if(entry.state.table_id==id)return true;
    return false;
}
std::optional<CursorId>& CursorRegistry::next_id(){static std::optional<CursorId> id{0};return id;}
}
