#include "record_page.h"
#include <iostream>
#include <stdexcept>
#include <source_location>
using namespace tinydbms;
using namespace tinydbms::storage;
using namespace tinydbms::storage::internal;
void check(bool b, std::source_location at = std::source_location::current()) {
    if (!b) throw std::runtime_error("RecordPage contract failed at line " + std::to_string(at.line()));
}
int main() { try {
    for (auto parts : {RecordIdParts{1,0,1}, RecordIdParts{UINT32_MAX,UINT16_MAX,UINT16_MAX}}) {
        auto id = RecordIdCodec::encode(parts); check(id.value.has_value());
        auto back = RecordIdCodec::decode(*id.value); check(back.value.has_value());
        check(back.value->page_id == parts.page_id && back.value->slot_id == parts.slot_id && back.value->generation == parts.generation);
    }
    check(!RecordIdCodec::encode({0,0,1}).value);
    check(!RecordIdCodec::encode({1,0,0}).value);
    check(!RecordIdCodec::decode({0}).value);
    check(!RecordIdCodec::decode({std::uint64_t{1} << 32}).value);
    check(!RecordIdCodec::decode({std::uint64_t{1} << 16}).value);
    check(RecordIdCodec::encode({0x12345678,0x9abc,0xdef0}).value->value == 0x12345678def09abcULL);
    RawPage page; check(!RecordPage::initialize(page,1));
    TableMeta meta{0,"test",{{"i",Type::kInt},{"j",Type::kInt},{"s",Type::kVarchar}}};
    std::vector<Value> values{{std::int32_t{-2}},{std::int32_t{9}},{std::string{"\xE4\xB8\xAD\xF0\x9F\x99\x82"}}};
    auto a = RecordPage::insert_record(page,1,meta,values); check(a.value.has_value());
    auto b = RecordPage::insert_record(page,1,meta,values); check(b.value.has_value());
    auto get = RecordPage::get_record(page,1,meta,*a.value); check(get.value.has_value());
    check(get.value->rid.value == a.value->value);
    for (std::size_t i=0;i<values.size();++i) check(values[i].data == get.value->values[i].data);
    auto snapshot=page.bytes;
    check(!RecordPage::get_record(page,2,meta,*a.value).value); check(page.bytes==snapshot);
    check(RecordPage::erase_record(page,2,*a.value).has_value()); check(page.bytes==snapshot);
    auto wrong=values; wrong[0]=Value{std::string{"wrong"}}; check(!RecordPage::insert_record(page,1,meta,wrong).value); check(page.bytes==snapshot);
    wrong=values; wrong.back()=Value{std::string(1025,'x')};
    auto oversized=RecordPage::insert_record(page,1,meta,wrong); check(oversized.error && oversized.error->kind==RecordPageErrorKind::kValueTooLarge); check(page.bytes==snapshot);
    check(!RecordPage::erase_record(page,1,*a.value));
    check(!RecordPage::get_record(page,1,meta,*a.value).value);
    auto replacement=RecordPage::insert_record(page,1,meta,values); check(replacement.value.has_value());
    check(replacement.value->value!=a.value->value);
    check(RecordIdCodec::decode(*replacement.value).value->generation==2);
    snapshot=page.bytes; check(!RecordPage::get_record(page,1,meta,*a.value).value);
    check(RecordPage::erase_record(page,1,*a.value).has_value()); check(page.bytes==snapshot);
    check(RecordPage::get_record(page,1,meta,*b.value).value.has_value());
    check(RecordPage::get_record(page,1,meta,*replacement.value).value.has_value());
    RawPage full; check(!RecordPage::initialize(full,3));
    check(SlottedPage::insert(full,3,std::vector<std::byte>(kMaxRecordPayloadBytes)).value.has_value());
    snapshot=full.bytes; auto no_space=RecordPage::insert_record(full,3,meta,values);
    check(no_space.error && no_space.error->kind==RecordPageErrorKind::kNoSpace); check(full.bytes==snapshot);
    RawPage corrupt; check(!RecordPage::initialize(corrupt,4));
    auto slot=SlottedPage::insert(corrupt,4,std::vector<std::byte>{std::byte{2}}); check(slot.value.has_value());
    auto rid=RecordIdCodec::encode({4,slot.value->slot_id,slot.value->generation});
    TableMeta integer{0,"int",{{"i",Type::kInt}}};
    auto bad=RecordPage::get_record(corrupt,4,integer,*rid.value); check(bad.error && bad.error->kind==RecordPageErrorKind::kCorrupt);
    corrupt.bytes[0]=std::byte{0}; bad=RecordPage::get_record(corrupt,4,integer,*rid.value); check(bad.error && bad.error->kind==RecordPageErrorKind::kCorrupt);
    snapshot=corrupt.bytes;
    check(RecordPage::insert_record(corrupt,4,integer,{Value{std::int32_t{1}}}).error->kind==RecordPageErrorKind::kCorrupt);
    check(RecordPage::erase_record(corrupt,4,*rid.value)->kind==RecordPageErrorKind::kCorrupt);
    check(corrupt.bytes==snapshot);
    RawPage retired; check(!RecordPage::initialize(retired,5));
    auto live=RecordPage::insert_record(retired,5,integer,{Value{std::int32_t{1}}}); check(live.value.has_value());
    retired.bytes[kSlottedPageHeaderSize+4]=std::byte{0xff};
    retired.bytes[kSlottedPageHeaderSize+5]=std::byte{0xff};
    auto last=RecordIdCodec::encode({5,0,UINT16_MAX});
    check(!RecordPage::erase_record(retired,5,*last.value));
    check(!RecordPage::get_record(retired,5,integer,*last.value).value);
    auto next=RecordPage::insert_record(retired,5,integer,{Value{std::int32_t{0}}});
    check(RecordIdCodec::decode(*next.value).value->slot_id==1);
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } }
