#pragma once
#include "record_codec.h"
#include "slotted_page.h"
#include "tinydbms/storage.hpp"

namespace tinydbms::storage::internal {
enum class RecordPageErrorKind { kInvalidArgument, kValueTooLarge, kNoSpace, kCorrupt };
struct RecordPageError { RecordPageErrorKind kind; std::string message; };
template<class T> struct RecordPageResult {
    std::optional<T> value;
    std::optional<RecordPageError> error;
};
struct RecordIdParts { PageId page_id; SlotId slot_id; std::uint16_t generation; };
class RecordIdCodec {
public:
    static RecordPageResult<RecordId> encode(RecordIdParts parts);
    static RecordPageResult<RecordIdParts> decode(RecordId id);
};
struct NextRecordResult {
    std::optional<Record> record;
    std::size_t next_position;
    std::optional<RecordPageError> error;
};
class RecordPage {
public:
    static NextRecordResult next_record(const RawPage&, PageId, const TableMeta&, std::size_t start);
    static std::optional<RecordPageError> initialize(RawPage&, PageId);
    static RecordPageResult<RecordId> insert_record(RawPage&, PageId, const TableMeta&,
                                                     const std::vector<Value>&);
    static RecordPageResult<Record> get_record(const RawPage&, PageId, const TableMeta&, RecordId);
    static std::optional<RecordPageError> erase_record(RawPage&, PageId, RecordId);
};
}
