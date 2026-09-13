#include "tinydbms/storage.hpp"
#include "storage_test_access.h"
#include "record_page.h"
#include "cursor_state.h"
#include <chrono>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <limits>
#include <fstream>
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
    Temp(){
        check(std::filesystem::create_directory(path));
        std::ofstream metadata(path / "storage.meta");
        metadata << "TINYDBMS_STORAGE_V1\nEND\n";
        check(static_cast<bool>(metadata));
    }
    ~Temp(){std::error_code e;std::filesystem::remove_all(path,e);}
};
void create(TableId id=0){check(!create_table({id,id==0?"records":"other",{{"s",Type::kVarchar}}}).error);}
std::vector<Value> row(std::size_t n=10,char c='x'){return {{std::string(n,c)}};}
std::vector<Record> scan(TableId id=0){auto opened=open_table({id});check(opened.cursor && !opened.error);
    std::vector<Record> rows;
    while(true){auto r=scan_next({*opened.cursor});
        if(r.error)throw std::runtime_error("scan failed: "+r.error->message);
        check(BufferPoolTestAccess::no_pins(*StorageTestAccess::buffer_pool()));
        if(!r.record) {
            break;
        }
        rows.push_back(std::move(*r.record));}
    auto again=scan_next({*opened.cursor});check(!again.error && !again.record);check(!close_cursor({*opened.cursor}).error);return rows;}
void e2e(){
    Temp temp;check(StorageTestAccess::configure(1));check(!open_storage({temp.path.string()}).error);create();
    kind(insert({99,{}}).error,StorageErrorKind::kTableNotFound);
    kind(delete_records({99,{}}).error,StorageErrorKind::kTableNotFound);
    kind(update_rows({99,{}}).error,StorageErrorKind::kTableNotFound);
    kind(open_table({99}).error,StorageErrorKind::kTableNotFound);
    auto first=open_table({0});check(first.cursor && *first.cursor==0);check(!close_cursor({*first.cursor}).error);
    kind(insert({0,{}}).error,StorageErrorKind::kInvalidRequest);check(!delete_records({0,{}}).error);check(scan().empty());
    std::vector<std::vector<Value>> values;for(int i=0;i<10;++i)values.push_back(row(1024,static_cast<char>('a'+i)));
    auto added=insert({0,values});check(!added.error && added.rids.size()==10);
    auto changed=row(300,'u');
    auto updated=update_rows({0,{{added.rids[0],changed}}});
    check(!updated.error && updated.updated_count==1);values[0]=changed;
    auto duplicate=update_rows({0,{
        {added.rids[0],row(20,'d')},{added.rids[0],row(30,'e')}}});
    kind(duplicate.error,StorageErrorKind::kInvalidRequest);check(duplicate.updated_count==0);
    auto invalid=update_rows({0,{
        {added.rids[0],row(20,'v')},{RecordId{},row(30,'i')}}});
    kind(invalid.error,StorageErrorKind::kInvalidRequest);check(invalid.updated_count==0);
    auto wrong=update_rows({0,{{added.rids[0],{Value{std::int32_t{7}}}}}});
    kind(wrong.error,StorageErrorKind::kInvalidRequest);check(wrong.updated_count==0);
    auto too_large=update_rows({0,{{added.rids[0],row(1025)}}});
    kind(too_large.error,StorageErrorKind::kValueTooLarge);check(too_large.updated_count==0);
    auto rows=scan();check(rows.size()==10);
    for(std::size_t i=0;i<rows.size();++i){check(rows[i].rid.value==added.rids[i].value);check(rows[i].values[0].data==values[i][0].data);}
    auto partial=insert({0,{row(),{{std::int32_t{7}}},row()}});kind(partial.error,StorageErrorKind::kInvalidRequest);check(partial.rids.size()==1);
    kind(insert({0,{row(1025)}}).error,StorageErrorKind::kValueTooLarge);
    auto deleted=delete_records({0,{added.rids[1],added.rids[5]}});check(!deleted.error && deleted.deleted_count==2);
    auto prefix=delete_records({0,{added.rids[2],added.rids[2],added.rids[3]}});
    kind(prefix.error,StorageErrorKind::kInvalidRequest);check(prefix.deleted_count==1);
    auto survivors=scan();check(survivors.size()==8);
    auto held=open_table({0});check(held.cursor.has_value());check(!close_storage({}).error);
    kind(scan_next({*held.cursor}).error,StorageErrorKind::kCursorInvalid);
    check(!open_storage({temp.path.string()}).error);
    kind(scan_next({*held.cursor}).error,StorageErrorKind::kCursorInvalid);
    auto newer=open_table({0});check(newer.cursor && *newer.cursor>*held.cursor);check(!close_cursor({*newer.cursor}).error);
    auto restored=scan();check(restored.size()==survivors.size());
    for(std::size_t i=0;i<restored.size();++i){check(restored[i].rid.value==survivors[i].rid.value);check(restored[i].values[0].data==survivors[i].values[0].data);}
    auto reused=insert({0,{row(1024)}});check(!reused.error && reused.rids.size()==1);
    kind(delete_records({0,{added.rids[1]}}).error,StorageErrorKind::kInvalidRequest);
    check(!close_storage({}).error);check(!open_storage({temp.path.string()}).error);
    kind(delete_records({0,{added.rids[1]}}).error,StorageErrorKind::kInvalidRequest);
    check(!delete_records({0,reused.rids}).error);check(!close_storage({}).error);
}
void blocked(TableId table,RecordId rid){
    auto i=insert({table,{row()}});kind(i.error,StorageErrorKind::kInvalidRequest);check(i.rids.empty());
    auto d=delete_records({table,{rid}});kind(d.error,StorageErrorKind::kInvalidRequest);check(d.deleted_count==0);
    auto u=update_rows({table,{{rid,row()}}});kind(u.error,StorageErrorKind::kInvalidRequest);check(u.updated_count==0);
    kind(insert({table,{}}).error,StorageErrorKind::kInvalidRequest);check(!delete_records({table,{}}).error);
    check(!update_rows({table,{}}).error);
}
void restrictions(){
    Temp temp;check(StorageTestAccess::configure(1));check(!open_storage({temp.path.string()}).error);create();create(1);
    auto i=insert({0,{row(),row(20)}});check(!i.error);auto id=i.rids[0];
    auto* pool=StorageTestAccess::buffer_pool();auto fetches=pool->stats().fetch_count;
    auto a=open_table({0}),b=open_table({0});check(a.cursor && b.cursor && *b.cursor>*a.cursor);
    check(pool->stats().fetch_count==fetches);blocked(0,id);
    auto other=insert({1,{row()}});check(!other.error);check(!delete_records({1,other.rids}).error);
    auto r1=scan_next({*a.cursor}),r2=scan_next({*a.cursor}),s1=scan_next({*b.cursor});
    check(r1.record && r2.record && s1.record && r1.record->rid.value==s1.record->rid.value);
    check(!scan_next({*a.cursor}).record);blocked(0,id); // EOF still blocks.
    check(!close_cursor({*a.cursor}).error);blocked(0,id); // Other cursor remains.
    check(!close_cursor({*b.cursor}).error);kind(close_cursor({*b.cursor}).error,StorageErrorKind::kCursorInvalid);
    check(!insert({0,{row()}}).error);check(!delete_records({0,{id}}).error);
    kind(scan_next({std::numeric_limits<CursorId>::max()}).error,StorageErrorKind::kCursorInvalid);
    check(!close_storage({}).error);
}
void nonopen(bool closing=false){
    kind(insert({0,{}}).error,StorageErrorKind::kInvalidRequest);
    kind(delete_records({0,{}}).error,StorageErrorKind::kInvalidRequest);
    kind(update_rows({0,{}}).error,StorageErrorKind::kInvalidRequest);
    kind(open_table({0}).error,StorageErrorKind::kInvalidRequest);
    kind(scan_next({0}).error,closing?StorageErrorKind::kInvalidRequest:StorageErrorKind::kCursorInvalid);
    kind(close_cursor({0}).error,closing?StorageErrorKind::kInvalidRequest:StorageErrorKind::kCursorInvalid);
}
void lifecycle(){
    nonopen();Temp temp;bool fail=false;bool observed=false;FileManager* files=nullptr;
    check(StorageTestAccess::configure(1,ReplacementPolicy::kFifo,
        [&](PageKey key,const RawPage& p)->std::optional<PageFileError>{
            observed=true;nonopen(true); // Read-only lifecycle probes during Closing, no pool reentry.
            if(fail)return PageFileError{PageFileErrorKind::kIo,"close write injected"};
            return files->find_table_file(key.table_id)->write_page(key.page_id,p);}));
    check(!open_storage({temp.path.string()}).error);create();files=StorageTestAccess::file_manager();
    check(!insert({0,{row()}}).error);auto opened=open_table({0});check(opened.cursor.has_value());
    auto pin=StorageTestAccess::buffer_pool()->fetch_page({0,1});check(pin.value.has_value());
    kind(close_storage({}).error,StorageErrorKind::kInvalidRequest);check(!observed);pin.value->release();
    check(scan_next({*opened.cursor}).record.has_value()); // Position survives failed pin-close.
    fail=true;kind(close_storage({}).error,StorageErrorKind::kIoError);check(observed);
    check(!list_tables({}).error);auto eof=scan_next({*opened.cursor});check(!eof.record && !eof.error);
    blocked(0,RecordId{});fail=false;check(!close_storage({}).error);nonopen();
    check(StorageTestAccess::configure(1));check(!open_storage({temp.path.string()}).error);
    kind(scan_next({*opened.cursor}).error,StorageErrorKind::kCursorInvalid);
    auto c=open_table({0});check(c.cursor.has_value());StorageTestAccess::fail_next_file_close();
    kind(close_storage({}).error,StorageErrorKind::kIoError);nonopen(true);
    check(StorageTestAccess::file_manager()->find_table_file(0)!=nullptr);
    check(!close_storage({}).error);check(!open_storage({temp.path.string()}).error);
    kind(scan_next({*c.cursor}).error,StorageErrorKind::kCursorInvalid);check(!close_storage({}).error);
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
        check(!close_storage({}).error);check(!open_storage({temp.path.string()}).error);fail=!corrupt;
        auto opened=open_table({0});check(opened.cursor.has_value());
        auto r=scan_next({*opened.cursor});kind(r.error,corrupt?StorageErrorKind::kCorrupt:StorageErrorKind::kIoError);check(!r.record);
        fail=false;auto again=scan_next({*opened.cursor});check(again.error && again.error->message==r.error->message && again.error->kind==r.error->kind);
        blocked(0,RecordId{});check(BufferPoolTestAccess::no_pins(*StorageTestAccess::buffer_pool()));
        check(BufferPoolTestAccess::clean(*StorageTestAccess::buffer_pool()));check(!close_cursor({*opened.cursor}).error);
        if(!corrupt)check(!insert({0,{row()}}).error);
        check(!close_storage({}).error);
    }
    Temp temp;check(StorageTestAccess::configure(1));check(!open_storage({temp.path.string()}).error);create();
    auto data=insert({0,{row(1024),row(1024),row(1024),row(1024)}});check(!data.error);
    auto guard=StorageTestAccess::buffer_pool()->fetch_page({0,2});check(guard.value.has_value());
    auto failure=delete_records({0,{data.rids[0]}});kind(failure.error,StorageErrorKind::kInvalidRequest);
    check(failure.error->message.find("unpinned")!=std::string::npos);
    guard.value->release();check(!close_storage({}).error);
}
void holes_retired(){
    Temp temp;check(StorageTestAccess::configure(1));check(!open_storage({temp.path.string()}).error);create();
    auto* file=StorageTestAccess::file_manager()->find_table_file(0);
    check(file->allocate_page().value==1);check(file->allocate_page().value==2);check(!file->free_page(1));
    RawPage p;check(!RecordPage::initialize(p,2));TableMeta meta{0,"records",{{"s",Type::kVarchar}}};
    auto a=RecordPage::insert_record(p,2,RowFormat::kV2,meta,row());
    auto b=RecordPage::insert_record(p,2,RowFormat::kV2,meta,row(20));check(a.value && b.value);
    p.bytes[36]=std::byte{255};p.bytes[37]=std::byte{255};
    check(!RecordPage::erase_record(p,2,RecordId{(std::uint64_t{2}<<32)|(std::uint64_t{65535}<<16)}));
    check(!file->write_page(2,p)); // Uncached fixture bootstrap; public scan does not bypass pool.
    check(!close_storage({}).error);check(!open_storage({temp.path.string()}).error);
    auto rows=scan();check(rows.size()==1 && rows[0].rid.value==b.value->value);
    check(!close_storage({}).error);
}
void exhausted(){
    Temp temp;check(StorageTestAccess::configure());check(!open_storage({temp.path.string()}).error);create();
    CursorRegistryTestAccess::exhaust_soon();auto last=open_table({0});
    check(last.cursor && *last.cursor==std::numeric_limits<CursorId>::max());
    kind(open_table({0}).error,StorageErrorKind::kInvalidRequest);check(!close_storage({}).error);
    check(!open_storage({temp.path.string()}).error);kind(open_table({0}).error,StorageErrorKind::kInvalidRequest);
    check(!close_storage({}).error);
}
void mixed_v1_v2_scalars(){
    Temp temp;check(StorageTestAccess::configure(2));
    check(!open_storage({temp.path.string()}).error);
    check(!create_table({0,"legacy",{{"id",Type::kInt},{"name",Type::kVarchar}}}).error);
    check(!close_storage({}).error);
    {
        std::ofstream out(temp.path/"storage.meta",std::ios::trunc);
        out<<"TINYDBMS_STORAGE_V1\n"
              "TABLE 0 legacy\n"
              "COLUMN INT32 id\n"
              "COLUMN VARCHAR name\n"
              "ENDTABLE\nEND\n";
        out.close();check(!out.fail());
    }
    check(!open_storage({temp.path.string()}).error);
    check(!insert({0,{{Value{std::int32_t{42}},Value{std::string{"alice"}}}}}).error);
    check(!create_table({1,"events",{{"event_id",Type::kBigInt},{"label",Type::kVarchar}}}).error);
    const std::int64_t large=0x0102030405060708LL;
    check(!insert({1,{{Value{large},Value{std::string{"created"}}}}}).error);
    check(!create_table({2,"measurements",{{"value",Type::kDouble}}}).error);
    check(!insert({2,{{Value{1.0}},{Value{-1.5}}}}).error);
    check(!create_table({3,"flags",{{"active",Type::kBoolean}}}).error);
    check(!insert({3,{{Value{true}},{Value{false}}}}).error);
    check(!create_table({4,"nullable_values",{
        {"id",Type::kInt,false},{"note",Type::kVarchar,true},
        {"active",Type::kBoolean,true}}}).error);
    check(!insert({4,{
        {Value{std::int32_t{1}},Value{std::monostate{}},Value{std::monostate{}}},
        {Value{std::int32_t{0}},Value{std::string{}},Value{false}}}}).error);
    kind(insert({4,{{Value{std::monostate{}},Value{std::string{"bad"}},Value{true}}}}).error,
         StorageErrorKind::kInvalidRequest);
    check(!close_storage({}).error);

    {
        std::ifstream input(temp.path/"storage.meta");
        const std::string metadata(
            (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        check(metadata.find("TABLE 2 measurements V2")!=std::string::npos);
        check(metadata.find("COLUMN DOUBLE NOT_NULL value")!=std::string::npos);
        check(metadata.find("TABLE 3 flags V2")!=std::string::npos);
        check(metadata.find("COLUMN BOOLEAN NOT_NULL active")!=std::string::npos);
        check(metadata.find("TABLE 4 nullable_values V2")!=std::string::npos);
        check(metadata.find("COLUMN VARCHAR NULLABLE note")!=std::string::npos);
        check(metadata.find("COLUMN BOOLEAN NULLABLE active")!=std::string::npos);
    }

    check(!open_storage({temp.path.string()}).error);
    const auto legacy=scan(0);check(legacy.size()==1);
    check(std::get<std::int32_t>(legacy[0].values[0].data)==42);
    check(std::get<std::string>(legacy[0].values[1].data)=="alice");
    const auto events=scan(1);check(events.size()==1);
    check(std::get<std::int64_t>(events[0].values[0].data)==large);
    check(std::get<std::string>(events[0].values[1].data)=="created");
    const auto measurements=scan(2);check(measurements.size()==2);
    check(std::get<double>(measurements[0].values[0].data)==1.0);
    check(std::get<double>(measurements[1].values[0].data)==-1.5);
    const auto flags=scan(3);check(flags.size()==2);
    check(std::get<bool>(flags[0].values[0].data));
    check(!std::get<bool>(flags[1].values[0].data));
    const auto nullable=scan(4);check(nullable.size()==2);
    check(std::holds_alternative<std::monostate>(nullable[0].values[1].data));
    check(std::holds_alternative<std::monostate>(nullable[0].values[2].data));
    check(std::get<std::int32_t>(nullable[1].values[0].data)==0);
    check(std::get<std::string>(nullable[1].values[1].data).empty());
    check(!std::get<bool>(nullable[1].values[2].data));
    auto null_violation=update_rows({4,{{nullable[0].rid,{
        Value{std::monostate{}},Value{std::string{"bad"}},Value{true}}}}});
    kind(null_violation.error,StorageErrorKind::kInvalidRequest);
    check(null_violation.updated_count==0);
    auto nullable_update=update_rows({4,{
        {nullable[0].rid,{Value{std::int32_t{1}},Value{std::string{"updated"}},Value{true}}},
        {nullable[1].rid,{Value{std::int32_t{0}},Value{std::monostate{}},Value{std::monostate{}}}}}});
    check(!nullable_update.error && nullable_update.updated_count==2);
    check(!close_storage({}).error);
    check(!open_storage({temp.path.string()}).error);
    const auto nullable_second_reopen=scan(4);check(nullable_second_reopen.size()==2);
    check(std::get<std::string>(nullable_second_reopen[0].values[1].data)=="updated");
    check(std::get<bool>(nullable_second_reopen[0].values[2].data));
    check(std::holds_alternative<std::monostate>(nullable_second_reopen[1].values[1].data));
    check(std::holds_alternative<std::monostate>(nullable_second_reopen[1].values[2].data));
    check(!close_storage({}).error);
}
}
int main()try{e2e();restrictions();lifecycle();failures();holes_retired();mixed_v1_v2_scalars();exhausted();std::cout<<"storage CRUD tests passed\n";}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
