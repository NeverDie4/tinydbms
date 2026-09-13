#include "record_page.h"
#include <limits>
#include <utility>

namespace tinydbms::storage::internal {
namespace {
constexpr unsigned kSlotBits = std::numeric_limits<SlotId>::digits;
constexpr unsigned kGenerationBits = std::numeric_limits<std::uint16_t>::digits;
constexpr unsigned kPageShift = kSlotBits + kGenerationBits;
static_assert(kPageShift + std::numeric_limits<PageId>::digits == 64);
RecordPageError invalid() { return {RecordPageErrorKind::kInvalidArgument, "invalid record handle or page"}; }
RecordPageError map(const RecordCodecError& e) {
    switch(e.kind) {
        case RecordCodecErrorKind::kInvalidArgument: return {RecordPageErrorKind::kInvalidArgument,e.message};
        case RecordCodecErrorKind::kValueTooLarge: return {RecordPageErrorKind::kValueTooLarge,e.message};
        case RecordCodecErrorKind::kCorrupt: return {RecordPageErrorKind::kCorrupt,e.message};
    }
    return invalid();
}
RecordPageError map(const SlottedPageError& e) {
    switch(e.kind) {
        case SlottedPageErrorKind::kInvalidArgument: return {RecordPageErrorKind::kInvalidArgument,e.message};
        case SlottedPageErrorKind::kNoSpace: return {RecordPageErrorKind::kNoSpace,e.message};
        case SlottedPageErrorKind::kCorrupt: return {RecordPageErrorKind::kCorrupt,e.message};
    }
    return invalid();
}
}
NextRecordResult RecordPage::next_record(
    const RawPage& page, PageId id, RowFormat format, const TableMeta& meta,
    std::size_t start) {
    auto next=SlottedPage::next_live_slot(page,id,start);
    if (next.error) return {std::nullopt,start,map(*next.error)};
    if (!next.slot) return {std::nullopt,next.next_position,std::nullopt};
    auto rid=RecordIdCodec::encode({id,next.slot->slot_id,next.slot->generation});
    if (rid.error) return {std::nullopt,start,std::move(rid.error)};
    auto record=get_record(page,id,format,meta,*rid.value);
    if (record.error) return {std::nullopt,start,std::move(record.error)};
    return {std::move(record.value),next.next_position,std::nullopt};
}
NextRecordResult RecordPage::next_record(
    const RawPage& page, PageId id, const TableMeta& meta, std::size_t start) {
    return next_record(page,id,RowFormat::kV1,meta,start);
}
RecordPageResult<RecordId> RecordIdCodec::encode(RecordIdParts p) {
    if (!p.page_id || !p.generation) return {std::nullopt,invalid()};
    return {RecordId{(std::uint64_t{p.page_id} << kPageShift) |
                     (std::uint64_t{p.generation} << kSlotBits) | p.slot_id},std::nullopt};
}
RecordPageResult<RecordIdParts> RecordIdCodec::decode(RecordId id) {
    RecordIdParts p{static_cast<PageId>(id.value >> kPageShift),
                    static_cast<SlotId>(id.value),
                    static_cast<std::uint16_t>(id.value >> kSlotBits)};
    if (!p.page_id || !p.generation) return {std::nullopt,invalid()};
    return {p,std::nullopt};
}
std::optional<RecordPageError> RecordPage::initialize(RawPage& page, PageId id) {
    if (auto e=SlottedPage::initialize(page,id)) return map(*e);
    return std::nullopt;
}
RecordPageResult<RecordId> RecordPage::insert_record(RawPage& page, PageId id,
    RowFormat format, const TableMeta& meta, const std::vector<Value>& values) {
    auto encoded=RecordCodec::encode(format,meta,values);
    if (encoded.error) return {std::nullopt,map(*encoded.error)};
    RawPage candidate=page;
    auto slot=SlottedPage::insert(candidate,id,*encoded.value);
    if (slot.error) return {std::nullopt,map(*slot.error)};
    auto rid=RecordIdCodec::encode({id,slot.value->slot_id,slot.value->generation});
    if (rid.error) return rid;
    page=std::move(candidate);
    return rid;
}
RecordPageResult<RecordId> RecordPage::insert_record(RawPage& page, PageId id,
    const TableMeta& meta, const std::vector<Value>& values) {
    return insert_record(page,id,RowFormat::kV1,meta,values);
}
RecordPageResult<Record> RecordPage::get_record(const RawPage& page, PageId id,
    RowFormat format, const TableMeta& meta, RecordId rid) {
    auto parts=RecordIdCodec::decode(rid);
    if (parts.error) return {std::nullopt,parts.error};
    if (parts.value->page_id!=id) return {std::nullopt,invalid()};
    auto bytes=SlottedPage::get(page,id,{parts.value->slot_id,parts.value->generation});
    if (bytes.error) return {std::nullopt,map(*bytes.error)};
    auto values=RecordCodec::decode(format,meta,*bytes.value);
    if (values.error) return {std::nullopt,map(*values.error)};
    return {Record{rid,std::move(*values.value)},std::nullopt};
}
RecordPageResult<Record> RecordPage::get_record(const RawPage& page, PageId id,
    const TableMeta& meta, RecordId rid) {
    return get_record(page,id,RowFormat::kV1,meta,rid);
}
std::optional<RecordPageError> RecordPage::replace_record(
    RawPage& page, PageId id, RowFormat format, const TableMeta& meta, RecordId rid,
    const std::vector<Value>& values) {
    auto parts=RecordIdCodec::decode(rid);
    if (parts.error) return parts.error;
    if (parts.value->page_id!=id) return invalid();
    auto encoded=RecordCodec::encode(format,meta,values);
    if (encoded.error) return map(*encoded.error);
    RawPage candidate=page;
    if (auto e=SlottedPage::replace(
            candidate,id,{parts.value->slot_id,parts.value->generation},*encoded.value)) {
        return map(*e);
    }
    page=std::move(candidate);
    return std::nullopt;
}
std::optional<RecordPageError> RecordPage::replace_record(
    RawPage& page, PageId id, const TableMeta& meta, RecordId rid,
    const std::vector<Value>& values) {
    return replace_record(page,id,RowFormat::kV1,meta,rid,values);
}
std::optional<RecordPageError> RecordPage::erase_record(RawPage& page, PageId id, RecordId rid) {
    auto parts=RecordIdCodec::decode(rid);
    if (parts.error) return parts.error;
    if (parts.value->page_id!=id) return invalid();
    if (auto e=SlottedPage::erase(page,id,{parts.value->slot_id,parts.value->generation})) return map(*e);
    return std::nullopt;
}
}
