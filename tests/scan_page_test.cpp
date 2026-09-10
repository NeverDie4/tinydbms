#include "record_page.h"
#include <iostream>
#include <source_location>
#include <stdexcept>
using namespace tinydbms;
using namespace tinydbms::storage::internal;
void check(bool ok, std::source_location at=std::source_location::current()) {
    if(!ok) throw std::runtime_error("scan-page line " + std::to_string(at.line()));
}
int main() try {
    RawPage p; check(!SlottedPage::initialize(p,1));
    auto empty=SlottedPage::next_live_slot(p,1,0);
    check(!empty.slot && !empty.error && empty.next_position==0);
    auto a=SlottedPage::insert(p,1,std::vector<std::byte>{std::byte{1}}); check(a.value.has_value());
    auto live=SlottedPage::next_live_slot(p,1,0);
    check(live.slot==a.value && !live.error && live.next_position==1);
    auto end=SlottedPage::next_live_slot(p,1,1); check(!end.slot && !end.error);
    auto b=SlottedPage::insert(p,1,std::vector<std::byte>{std::byte{0}}); check(b.value.has_value());
    auto c=SlottedPage::insert(p,1,std::vector<std::byte>{std::byte{1}}); check(c.value.has_value());
    check(!SlottedPage::erase(p,1,*b.value));
    auto third=SlottedPage::next_live_slot(p,1,1); check(third.slot==c.value && third.next_position==3);
    auto before=p.bytes;
    TableMeta meta{0,"bools",{{"b",Type::kBool}}};
    auto record=RecordPage::next_record(p,1,meta,0);
    check(record.record.has_value() && !record.error && record.next_position==1);
    check(record.record->values[0].data==Value{true}.data);
    check(record.record->record_id.value==RecordIdCodec::encode({1,0,1}).value->value);
    auto record3=RecordPage::next_record(p,1,meta,1);
    check(record3.record.has_value() && record3.next_position==3);
    check(!RecordPage::next_record(p,1,meta,3).record);
    check(p.bytes==before);
    // Retire the deleted middle slot without changing frozen layout.
    p.bytes[44]=std::byte{0xff};p.bytes[45]=std::byte{0xff};p.bytes[46]=std::byte{2};
    check(!SlottedPage::validate(p,1));
    check(SlottedPage::next_live_slot(p,1,1).slot==c.value);
    check(RecordPage::next_record(p,1,meta,1).record->record_id.value==record3.record->record_id.value);
    auto good=p;
    // Entire page validated even when the requested start is already EOF.
    for(auto [offset,value]:std::vector<std::pair<std::size_t,std::byte>>{
        {38,std::byte{4}}, {38,std::byte{3}}, {36,std::byte{0}},
        {40,std::byte{1}}, {42,std::byte{1}}, {44,std::byte{1}},
        {16,std::byte{0}}, {32,std::byte{0}}}) {
        auto bad=good;bad.bytes[offset]=value;
        auto damaged=SlottedPage::next_live_slot(bad,1,3);
        check(damaged.error && damaged.error->kind==SlottedPageErrorKind::kCorrupt);
    }
    check(SlottedPage::next_live_slot(good,1,4).error->kind==SlottedPageErrorKind::kInvalidArgument);
    auto broken=good;
    auto offset=std::to_integer<unsigned>(broken.bytes[32])+(std::to_integer<unsigned>(broken.bytes[33])<<8);
    broken.bytes[offset]=std::byte{2}; // Structurally valid slot, corrupt BOOL payload.
    check(!SlottedPage::validate(broken,1));
    check(RecordPage::next_record(broken,1,meta,0).error->kind==RecordPageErrorKind::kCorrupt);
    // The returned Record owns its values independently of subsequent RawPage changes.
    p.bytes.fill(std::byte{0});check(std::get<bool>(record.record->values[0].data));
    std::cout << "scan-page tests passed\n";
} catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
