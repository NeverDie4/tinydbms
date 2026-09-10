#include "tinydbms/storage.h"
#include "storage_test_access.h"
#include "record_page.h"
#include "cursor_state.h"
#include <chrono>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <limits>
namespace tinydbms::storage::internal {
struct CursorRegistryTestAccess {
    static void exhaust_soon(){CursorRegistry::next_id()=std::numeric_limits<CursorId>::max();}
};
struct BufferPoolTestAccess {
    static bool no_pins(const BufferPool& p){for(const auto& f:p.frames_)if(f.pin_count)return false;return true;}
    static bool clean(const BufferPool& p){for(const auto& f:p.frames_)if(f.dirty)return false;return true;}
};
}
namespace {
using namespace tinydbms;
using namespace tinydbms::storage;
using namespace tinydbms::storage::internal;
void check(bool ok,std::source_location at=std::source_location::current()){
    if(!ok)throw std::runtime_error("storage-crud line "+std::to_string(at.line()));
}
void kind(const std::optional<StorageError>& e,StorageErrorKind expected){check(e && e->kind==expected);}
struct Temp {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("tinydbms-crud-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp(){check(std::filesystem::create_directory(path));}
    ~Temp(){std::error_code e;std::filesystem::remove_all(path,e);}
};
void create(TableId id=0){check(!create_table({id,id==0?"records":"other",{{"s",Type::kVarchar}}}).error);}
std::vector<Value> row(std::size_t n=10,char c='x'){return {{std::string(n,c)}};}
std::vector<Record> scan(TableId id=0){auto opened=open_table({id});check(opened.cursor_id && !opened.error);
    std::vector<Record> rows;
    while(true){auto r=scan_next(*opened.cursor_id);check(!r.error);check(BufferPoolTestAccess::no_pins(*StorageTestAccess::buffer_pool()));
        if(!r.record)break;rows.push_back(std::move(*r.record));}
    auto again=scan_next(*opened.cursor_id);check(!again.error && !again.record);check(!close_cursor(*opened.cursor_id));return rows;}
void e2e(){
    Temp temp;check(StorageTestAccess::configure(1));check(!open_storage({temp.path.string()}).error);create();
    kind(insert({99,{}}).error,StorageErrorKind::kTableNotFound);
    kind(delete_records({99,{}}).error,StorageErrorKind::kTableNotFound);
    kind(open_table({99}).error,StorageErrorKind::kTableNotFound);
    auto first=open_table({0});check(first.cursor_id && *first.cursor_id==0);check(!close_cursor(*first.cursor_id));
    check(!insert({0,{}}).error);check(!delete_records({0,{}}).error);check(scan().empty());
    std::vector<std::vector<Value>> values;for(int i=0;i<10;++i)values.push_back(row(1024,static_cast<char>('a'+i)));
    auto added=insert({0,values});check(!added.error && added.record_ids.size()==10);
    auto rows=scan();check(rows.size()==10);
    for(std::size_t i=0;i<rows.size();++i){check(rows[i].record_id.value==added.record_ids[i].value);check(rows[i].values[0].data==values[i][0].data);}
    auto partial=insert({0,{row(),{{std::int32_t{7}}},row()}});kind(partial.error,StorageErrorKind::kInvalidRequest);check(partial.record_ids.size()==1);
    kind(insert({0,{row(1025)}}).error,StorageErrorKind::kValueTooLarge);
    auto deleted=delete_records({0,{added.record_ids[1],added.record_ids[5]}});check(!deleted.error && deleted.deleted_count==2);
    auto prefix=delete_records({0,{added.record_ids[2],added.record_ids[2],added.record_ids[3]}});
    kind(prefix.error,StorageErrorKind::kInvalidRequest);check(prefix.deleted_count==1);
    auto survivors=scan();check(survivors.size()==8);
    auto held=open_table({0});check(held.cursor_id.has_value());check(!close_storage().error);
    kind(scan_next(*held.cursor_id).error,StorageErrorKind::kCursorInvalid);
    check(!open_storage({temp.path.string()}).error);
    kind(scan_next(*held.cursor_id).error,StorageErrorKind::kCursorInvalid);
    auto newer=open_table({0});check(newer.cursor_id && *newer.cursor_id>*held.cursor_id);check(!close_cursor(*newer.cursor_id));
    auto restored=scan();check(restored.size()==survivors.size());
    for(std::size_t i=0;i<restored.size();++i){check(restored[i].record_id.value==survivors[i].record_id.value);check(restored[i].values[0].data==survivors[i].values[0].data);}
    auto reused=insert({0,{row(1024)}});check(!reused.error && reused.record_ids.size()==1);
    kind(delete_records({0,{added.record_ids[1]}}).error,StorageErrorKind::kInvalidRequest);
    check(!close_storage().error);check(!open_storage({temp.path.string()}).error);
    kind(delete_records({0,{added.record_ids[1]}}).error,StorageErrorKind::kInvalidRequest);
    check(!delete_records({0,reused.record_ids}).error);check(!close_storage().error);
}
void blocked(TableId table,RecordId rid){
    auto i=insert({table,{row()}});kind(i.error,StorageErrorKind::kInvalidRequest);check(i.record_ids.empty());
    auto d=delete_records({table,{rid}});kind(d.error,StorageErrorKind::kInvalidRequest);check(d.deleted_count==0);
    check(!insert({table,{}}).error);check(!delete_records({table,{}}).error);
}
void restrictions(){
    Temp temp;check(StorageTestAccess::configure(1));check(!open_storage({temp.path.string()}).error);create();create(1);
    auto i=insert({0,{row(),row(20)}});check(!i.error);auto id=i.record_ids[0];
    auto* pool=StorageTestAccess::buffer_pool();auto fetches=pool->stats().fetch_count;
    auto a=open_table({0}),b=open_table({0});check(a.cursor_id && b.cursor_id && *b.cursor_id>*a.cursor_id);
    check(pool->stats().fetch_count==fetches);blocked(0,id);
    auto other=insert({1,{row()}});check(!other.error);check(!delete_records({1,other.record_ids}).error);
    auto r1=scan_next(*a.cursor_id),r2=scan_next(*a.cursor_id),s1=scan_next(*b.cursor_id);
    check(r1.record && r2.record && s1.record && r1.record->record_id.value==s1.record->record_id.value);
    check(!scan_next(*a.cursor_id).record);blocked(0,id); // EOF still blocks.
    check(!close_cursor(*a.cursor_id));blocked(0,id); // Other cursor remains.
    check(!close_cursor(*b.cursor_id));kind(close_cursor(*b.cursor_id),StorageErrorKind::kCursorInvalid);
    check(!insert({0,{row()}}).error);check(!delete_records({0,{id}}).error);
    kind(scan_next(std::numeric_limits<CursorId>::max()).error,StorageErrorKind::kCursorInvalid);
    check(!close_storage().error);
}
void nonopen(bool closing=false){
    kind(insert({0,{}}).error,StorageErrorKind::kInvalidRequest);
    kind(delete_records({0,{}}).error,StorageErrorKind::kInvalidRequest);
    kind(open_table({0}).error,StorageErrorKind::kInvalidRequest);
    kind(scan_next(0).error,closing?StorageErrorKind::kInvalidRequest:StorageErrorKind::kCursorInvalid);
    kind(close_cursor(0),closing?StorageErrorKind::kInvalidRequest:StorageErrorKind::kCursorInvalid);
}
void lifecycle(){
    nonopen();Temp temp;bool fail=false;bool observed=false;FileManager* files=nullptr;
    check(StorageTestAccess::configure(1,ReplacementPolicy::kFifo,
        [&](PageKey key,const RawPage& p)->std::optional<PageFileError>{
            observed=true;nonopen(true); // Read-only lifecycle probes during Closing, no pool reentry.
            if(fail)return PageFileError{PageFileErrorKind::kIo,"close write injected"};
            return files->find_table_file(key.table_id)->write_page(key.page_id,p);}));
    check(!open_storage({temp.path.string()}).error);create();files=StorageTestAccess::file_manager();
    check(!insert({0,{row()}}).error);auto opened=open_table({0});check(opened.cursor_id.has_value());
    auto pin=StorageTestAccess::buffer_pool()->fetch_page({0,1});check(pin.value.has_value());
    kind(close_storage().error,StorageErrorKind::kInvalidRequest);check(!observed);pin.value->release();
    check(scan_next(*opened.cursor_id).record.has_value()); // Position survives failed pin-close.
    fail=true;kind(close_storage().error,StorageErrorKind::kIoError);check(observed);
    check(!list_tables().error);auto eof=scan_next(*opened.cursor_id);check(!eof.record && !eof.error);
    blocked(0,RecordId{});fail=false;check(!close_storage().error);nonopen();
    check(StorageTestAccess::configure(1));check(!open_storage({temp.path.string()}).error);
    kind(scan_next(*opened.cursor_id).error,StorageErrorKind::kCursorInvalid);
    auto c=open_table({0});check(c.cursor_id.has_value());StorageTestAccess::fail_next_file_close();
    kind(close_storage().error,StorageErrorKind::kIoError);nonopen(true);
    check(!close_storage().error);check(!open_storage({temp.path.string()}).error);
    kind(scan_next(*c.cursor_id).error,StorageErrorKind::kCursorInvalid);check(!close_storage().error);
}
void failures(){
    for(bool corrupt:{false,true}){
        Temp temp;bool fail=false;
        check(StorageTestAccess::configure(1,ReplacementPolicy::kFifo,{},
            [&](PageKey key)->PageFileResult<RawPage>{
                if(fail)return {std::nullopt,PageFileError{PageFileErrorKind::kIo,"scan read injected"}};
                return StorageTestAccess::file_manager()->find_table_file(key.table_id)->read_page(key.page_id);}));
        check(!open_storage({temp.path.string()}).error);create();
        if(corrupt)check(StorageTestAccess::file_manager()->find_table_file(0)->allocate_page().value==1);
        else check(!insert({0,{row()}}).error);
        check(!close_storage().error);check(!open_storage({temp.path.string()}).error);fail=!corrupt;
        auto opened=open_table({0});check(opened.cursor_id.has_value());
        auto r=scan_next(*opened.cursor_id);kind(r.error,corrupt?StorageErrorKind::kCorrupt:StorageErrorKind::kIoError);check(!r.record);
        fail=false;auto again=scan_next(*opened.cursor_id);check(again.error && again.error->message==r.error->message && again.error->kind==r.error->kind);
        blocked(0,RecordId{});check(BufferPoolTestAccess::no_pins(*StorageTestAccess::buffer_pool()));
        check(BufferPoolTestAccess::clean(*StorageTestAccess::buffer_pool()));check(!close_cursor(*opened.cursor_id));
        if(!corrupt)check(!insert({0,{row()}}).error);
        check(!close_storage().error);
    }
    Temp temp;check(StorageTestAccess::configure(1));check(!open_storage({temp.path.string()}).error);create();
    auto data=insert({0,{row(1024),row(1024),row(1024),row(1024)}});check(!data.error);
    auto guard=StorageTestAccess::buffer_pool()->fetch_page({0,2});check(guard.value.has_value());
    auto failure=delete_records({0,{data.record_ids[0]}});kind(failure.error,StorageErrorKind::kInvalidRequest);
    check(failure.error->message.find("unpinned")!=std::string::npos);
    guard.value->release();check(!close_storage().error);
}
void holes_retired(){
    Temp temp;check(StorageTestAccess::configure(1));check(!open_storage({temp.path.string()}).error);create();
    auto* file=StorageTestAccess::file_manager()->find_table_file(0);
    check(file->allocate_page().value==1);check(file->allocate_page().value==2);check(!file->free_page(1));
    RawPage p;check(!RecordPage::initialize(p,2));TableMeta meta{0,"records",{{"s",Type::kVarchar}}};
    auto a=RecordPage::insert_record(p,2,meta,row()),b=RecordPage::insert_record(p,2,meta,row(20));check(a.value && b.value);
    p.bytes[36]=std::byte{255};p.bytes[37]=std::byte{255};
    check(!RecordPage::erase_record(p,2,RecordId{(std::uint64_t{2}<<32)|(std::uint64_t{65535}<<16)}));
    check(!file->write_page(2,p)); // Uncached fixture bootstrap; public scan does not bypass pool.
    check(!close_storage().error);check(!open_storage({temp.path.string()}).error);
    auto rows=scan();check(rows.size()==1 && rows[0].record_id.value==b.value->value);
    check(!close_storage().error);
}
void exhausted(){
    Temp temp;check(StorageTestAccess::configure());check(!open_storage({temp.path.string()}).error);create();
    CursorRegistryTestAccess::exhaust_soon();auto last=open_table({0});
    check(last.cursor_id && *last.cursor_id==std::numeric_limits<CursorId>::max());
    kind(open_table({0}).error,StorageErrorKind::kInvalidRequest);check(!close_storage().error);
    check(!open_storage({temp.path.string()}).error);kind(open_table({0}).error,StorageErrorKind::kInvalidRequest);
    check(!close_storage().error);
}
}
int main()try{e2e();restrictions();lifecycle();failures();holes_retired();exhausted();std::cout<<"storage CRUD tests passed\n";}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
