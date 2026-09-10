#include "heap_table.h"
#include <chrono>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <type_traits>
namespace tinydbms::storage::internal {
struct BufferPoolTestAccess {
    static bool no_pins(const BufferPool& p){for(const auto& f:p.frames_)if(f.pin_count)return false;return true;}
    static bool dirty(const BufferPool& p){for(const auto& f:p.frames_)if(f.dirty)return true;return false;}
};
}
namespace {
using namespace tinydbms;
using namespace tinydbms::storage;
using namespace tinydbms::storage::internal;
void check(bool ok,std::source_location at=std::source_location::current()){
    if(!ok)throw std::runtime_error("heap-delete line "+std::to_string(at.line()));
}
struct Temp {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("tinydbms-delete-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp(){check(std::filesystem::create_directory(path));}
    ~Temp(){std::error_code e;std::filesystem::remove_all(path,e);}
};
TableMeta meta(TableId id=0){return {id,"records",{{"s",Type::kVarchar}}};}
RecordId rid(PageId p,SlotId s=0,std::uint16_t g=1){return RecordId{(std::uint64_t{p}<<32)|(std::uint64_t{g}<<16)|s};}
struct Fixture {
    Temp temp;
    std::unique_ptr<FileManager> files;
    std::unique_ptr<BufferPool> pool;
    Fixture(){auto f=FileManager::open(temp.path);check(f.value.has_value());files=std::move(*f.value);
        check(files->create_table_file(0).value.has_value());reset_pool();}
    void reset_pool(BufferPool::ReadPage read={}){if(pool)check(!pool->close());
        auto p=BufferPool::create(*files,1,std::move(read));check(p.value.has_value());pool=std::move(*p.value);}
    PageFile& file(TableId id=0){return *files->find_table_file(id);}
    RecordId insert(std::string value="row",TableId id=0){HeapTable t(meta(id),*files,*pool);
        auto r=t.insert_record({{std::move(value)}});check(r.value.has_value());return *r.value;}
    void present(RecordId id,bool exists,TableId table=0){auto p=RecordIdCodec::decode(id);check(p.value.has_value());
        auto guard=pool->fetch_page({table,p.value->page_id});check(guard.value.has_value());
        auto r=RecordPage::get_record(guard.value->page(),p.value->page_id,meta(table),id);
        if(exists)check(r.value.has_value());else check(r.error && r.error->kind==RecordPageErrorKind::kInvalidArgument);}
};
void lifecycle(){
    Fixture f;HeapTable t(meta(),*f.files,*f.pool);auto a=f.insert("a"),b=f.insert("b"),c=f.insert("c");
    check(!f.pool->flush_all());check(!BufferPoolTestAccess::dirty(*f.pool));
    check(!t.delete_record(b));check(BufferPoolTestAccess::dirty(*f.pool));check(BufferPoolTestAccess::no_pins(*f.pool));
    f.present(a,true);f.present(b,false);f.present(c,true);
    check(t.delete_record(b)->kind==HeapTableErrorKind::kInvalidArgument);
    check(BufferPoolTestAccess::dirty(*f.pool)); // Failed operation preserves pre-dirty.
    check(!f.pool->flush_all());check(t.delete_record(b)->kind==HeapTableErrorKind::kInvalidArgument);
    check(!BufferPoolTestAccess::dirty(*f.pool));
    auto replacement=f.insert("replacement");auto parts=RecordIdCodec::decode(replacement);
    check(parts.value->slot_id==1 && parts.value->generation==2);
    check(t.delete_record(b)->kind==HeapTableErrorKind::kInvalidArgument);f.present(replacement,true);
    check(!t.delete_record(a));check(!t.delete_record(c));check(!t.delete_record(replacement));
    check(f.file().page_allocation_state(1).value==PageAllocationState::kAllocated);
    {auto g=f.pool->fetch_page({0,1});check(g.value.has_value());const auto& p=g.value->page();
        check(!SlottedPage::validate(p,1));check(p.bytes[12]==std::byte{3} && p.bytes[14]==std::byte{0});}
    check(BufferPoolTestAccess::no_pins(*f.pool));check(!f.pool->close());check(!f.files->close_all());
    check(f.files->open_table_file(0).value.has_value());f.reset_pool();
    f.present(a,false);f.present(b,false);f.present(c,false);f.present(replacement,false);
    auto new_id=f.insert("after reopen");check(RecordIdCodec::decode(new_id).value->generation==2);
    check(!f.pool->close());check(!f.files->close_all());
    check(f.files->open_table_file(0).value.has_value());f.reset_pool();
    HeapTable reopened(meta(),*f.files,*f.pool);check(reopened.delete_record(a)->kind==HeapTableErrorKind::kInvalidArgument);
    f.present(new_id,true);check(!reopened.delete_record(new_id));check(!f.pool->close());
}
void batches(){
    static_assert(std::is_same_v<decltype(HeapDeleteResult::deleted_count),std::uint64_t>);
    Fixture f;HeapTable t(meta(),*f.files,*f.pool);
    auto a=f.insert(),b=f.insert(),c=f.insert(),d=f.insert();
    auto empty=t.delete_batch({});check(!empty.error && empty.deleted_count==0);
    auto all=t.delete_batch({a,b});check(!all.error && all.deleted_count==2);
    auto prefix=t.delete_batch({c,c,d});check(prefix.error && prefix.deleted_count==1);
    f.present(a,false);f.present(b,false);f.present(c,false);f.present(d,true);
    auto first=t.delete_batch({rid(999),d});check(first.error && first.deleted_count==0);f.present(d,true);
    check(BufferPoolTestAccess::no_pins(*f.pool));check(!f.pool->close());check(!f.files->close_all());
    check(f.files->open_table_file(0).value.has_value());f.reset_pool();
    f.present(a,false);f.present(b,false);f.present(c,false);f.present(d,true);
    HeapTable missing(meta(99),*f.files,*f.pool);check(missing.delete_batch({}).error.has_value());
}
void invalid_and_collision(){
    Fixture f;HeapTable a(meta(),*f.files,*f.pool);auto id=f.insert();check(!f.pool->flush_all());
    for(auto bad:std::vector<RecordId>{rid(0),rid(2),rid(1,0,0),rid(1,7),rid(1,0,2)}){
        auto e=a.delete_record(bad);check(e && e->kind==HeapTableErrorKind::kInvalidArgument);
        check(!BufferPoolTestAccess::dirty(*f.pool));check(BufferPoolTestAccess::no_pins(*f.pool));}
    check(f.file().allocate_page().value==2);check(!f.file().free_page(2));
    check(a.delete_record(rid(2))->kind==HeapTableErrorKind::kInvalidArgument);
    check(f.files->create_table_file(1).value.has_value());HeapTable b(meta(1),*f.files,*f.pool);
    check(b.delete_record(id)->kind==HeapTableErrorKind::kInvalidArgument); // Detectable wrong-table range.
    auto collision=f.insert("other",1);check(collision.value==id.value);
    check(!b.delete_record(id)); // Identical bits are valid in either scope; provenance is not encoded.
    f.present(id,true,0);f.present(collision,false,1);
    check(!f.pool->close());
}
void corrupt_and_retired(){
    for(int mode=0;mode<4;++mode){
        Fixture f;check(f.file().allocate_page().value==1);RawPage p;RecordId id=rid(1);
        if(mode!=0){check(!RecordPage::initialize(p,1));auto r=RecordPage::insert_record(p,1,meta(),{{std::string("x")}});
            check(r.value.has_value());id=*r.value;
            if(mode==1)p.bytes[38]=std::byte{4}; // Unknown slot flags.
            if(mode==2){auto off=std::to_integer<unsigned>(p.bytes[32])+(std::to_integer<unsigned>(p.bytes[33])<<8);
                p.bytes[off]=std::byte{255};check(!SlottedPage::validate(p,1));} // Invalid VARCHAR length.
            if(mode==3){p.bytes[36]=std::byte{255};p.bytes[37]=std::byte{255};id=rid(1,0,65535);}
        }
        check(!f.file().write_page(1,p)); // Bootstrap fixture; no resident existing-page bypass.
        HeapTable t(meta(),*f.files,*f.pool);
        if(mode==3){check(!t.delete_record(id));check(!f.pool->flush_all());
            auto g=f.pool->fetch_page({0,1});check(g.value.has_value());check(g.value->page().bytes[38]==std::byte{2});g.value->release();
            check(t.delete_record(id)->kind==HeapTableErrorKind::kInvalidArgument);
            check(!BufferPoolTestAccess::dirty(*f.pool));
            auto next=f.insert();check(RecordIdCodec::decode(next).value->slot_id==1);
            check(!f.pool->close());
        }else{auto e=t.delete_record(id);check(e && e->kind==HeapTableErrorKind::kCorrupt);
            check(!BufferPoolTestAccess::dirty(*f.pool));
            auto g=f.pool->fetch_page({0,1});check(g.value && g.value->page().bytes==p.bytes);g.value->release();}
        check(BufferPoolTestAccess::no_pins(*f.pool));
    }
}
void io_and_capacity(){
    Fixture f;std::vector<RecordId> ids;for(int i=0;i<7;++i)ids.push_back(f.insert(std::string(1024,'x')));
    check(!f.pool->flush_all());HeapTable t(meta(),*f.files,*f.pool);
    {auto guard=f.pool->fetch_page({0,3});check(guard.value.has_value());
        auto e=t.delete_record(ids[0]);check(e && e->kind==HeapTableErrorKind::kNoVictim);check(guard.value->pin_count()==1);}
    check(BufferPoolTestAccess::no_pins(*f.pool));
    bool fail=true;
    f.reset_pool([&](PageKey key)->PageFileResult<RawPage>{if(fail)return {std::nullopt,PageFileError{PageFileErrorKind::kIo,"delete read failure"}};
        return f.file(key.table_id).read_page(key.page_id);});
    HeapTable reader(meta(),*f.files,*f.pool);auto e=reader.delete_record(ids[0]);check(e && e->kind==HeapTableErrorKind::kIo);
    check(BufferPoolTestAccess::no_pins(*f.pool) && !BufferPoolTestAccess::dirty(*f.pool));
    fail=false;auto result=reader.delete_batch(ids);check(!result.error && result.deleted_count==ids.size());
    check(BufferPoolTestAccess::no_pins(*f.pool));check(!f.pool->close());check(!f.files->close_all());
    check(f.files->open_table_file(0).value.has_value());f.reset_pool();
    HeapTable reopened(meta(),*f.files,*f.pool);auto pos=reopened.begin_scan();check(pos.value.has_value());
    auto eof=reopened.next_record(*pos.value);check(!eof.value && !eof.error);
    check(f.file().page_count()==4);for(PageId p=1;p<4;++p)check(f.file().page_allocation_state(p).value==PageAllocationState::kAllocated);
}
}
int main()try{lifecycle();batches();invalid_and_collision();corrupt_and_retired();io_and_capacity();std::cout<<"heap-delete tests passed\n";}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
