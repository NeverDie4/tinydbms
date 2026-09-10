#include "heap_table.h"
#include "cursor_state.h"
#include <chrono>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <limits>
namespace tinydbms::storage::internal {
struct CursorRegistryTestAccess {
    // Only advance at the end of this isolated test process; never reset/reuse IDs.
    static void approach_exhaustion(){CursorRegistry::next_id()=std::numeric_limits<CursorId>::max();}
};
struct BufferPoolTestAccess {
    static bool unpinned_clean(const BufferPool& pool) {
        for(const auto& f:pool.frames_) if(f.pin_count || f.dirty) return false;
        return true;
    }
};
}
namespace {
using namespace tinydbms;
using namespace tinydbms::storage;
using namespace tinydbms::storage::internal;
void check(bool ok,std::source_location at=std::source_location::current()) {
    if(!ok) throw std::runtime_error("heap-scan line " + std::to_string(at.line()));
}
struct Temp {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("tinydbms-scan-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp(){check(std::filesystem::create_directory(path));}
    ~Temp(){std::error_code e;std::filesystem::remove_all(path,e);}
};
TableMeta meta(){return {0,"scan",{{"text",Type::kVarchar}}};}
struct Fixture {
    Temp temp;
    std::unique_ptr<FileManager> files;
    std::unique_ptr<BufferPool> pool;
    Fixture(){auto f=FileManager::open(temp.path);check(f.value.has_value());files=std::move(*f.value);
        check(files->create_table_file(0).value.has_value());reset_pool();}
    void reset_pool(BufferPool::ReadPage read={}) {
        if(pool)check(!pool->close());
        auto p=BufferPool::create(*files,1,std::move(read));check(p.value.has_value());pool=std::move(*p.value);
    }
    PageFile& file(){return *files->find_table_file(0);}
};
void scan_roundtrip() {
    Fixture f; HeapTable t(meta(),*f.files,*f.pool);
    auto empty=t.begin_scan();check(empty.value.has_value());
    auto eof=t.next_record(*empty.value);check(!eof.value && !eof.error);
    std::vector<RecordId> ids;
    for(int i=0;i<10;++i){auto r=t.insert_record({{std::string(1024,static_cast<char>('a'+i))}});
        check(r.value.has_value());ids.push_back(*r.value);}
    check(!f.pool->flush_all());
    auto pos=t.begin_scan();check(pos.value.has_value());
    for(int i=0;i<10;++i){auto r=t.next_record(*pos.value);check(r.value.has_value());
        check(r.value->record_id.value==ids[i].value);
        check(std::get<std::string>(r.value->values[0].data)==std::string(1024,static_cast<char>('a'+i)));
        check(BufferPoolTestAccess::unpinned_clean(*f.pool));}
    auto end=t.next_record(*pos.value);check(!end.value && !end.error);
    check(!f.pool->close());check(!f.files->close_all());
    check(f.files->open_table_file(0).value.has_value());f.reset_pool();
    HeapTable reopened(meta(),*f.files,*f.pool);auto restored=reopened.begin_scan();check(restored.value.has_value());
    for(auto id:ids){auto r=reopened.next_record(*restored.value);check(r.value && r.value->record_id.value==id.value);}
    check(!reopened.next_record(*restored.value).value);
}
void cursors() {
    Fixture f;HeapTable t(meta(),*f.files,*f.pool);
    auto inserted=t.insert_record({{std::string("owned")}});check(inserted.value.has_value());
    auto second=t.insert_record({{std::string("second")}});check(second.value.has_value());
    check(!f.pool->flush_all());
    CursorRegistry registry(*f.files,*f.pool);
    auto a=registry.create(meta()),b=registry.create(meta());check(a.value && b.value && *a.value<*b.value);
    check(*a.value==0);
    check(registry.has_cursor(0));
    check(!registry.has_cursor(99));
    auto first_a=registry.next_record(*a.value);check(first_a.value && first_a.value->record_id.value==inserted.value->value);
    check(registry.lookup(*a.value).value->position.next_slot==1);
    check(registry.lookup(*b.value).value->position.next_slot==0);
    for(auto id:{*a.value,*b.value}) {
        if(id==*b.value){auto r=registry.next_record(id);check(r.value && r.value->record_id.value==inserted.value->value);}
        auto r=registry.next_record(id);check(r.value && r.value->record_id.value==second.value->value);
        check(BufferPoolTestAccess::unpinned_clean(*f.pool));
        auto end=registry.next_record(id);check(!end.value && !end.error);
        check(registry.lookup(id).value->status==CursorStatus::kEof);
        check(!registry.next_record(id).error);check(registry.has_cursor(0));
    }
    check(!registry.close(*a.value));check(registry.close(*a.value)->kind==CursorErrorKind::kCursorInvalid);
    check(registry.lookup(*a.value).error->kind==CursorErrorKind::kCursorInvalid);
    registry.clear();check(!registry.has_cursor(0));check(registry.lookup(*b.value).error.has_value());
    check(!f.pool->close());check(!f.files->close_all());
    check(f.files->open_table_file(0).value.has_value());f.reset_pool();
    CursorRegistry reopened(*f.files,*f.pool);auto c=reopened.create(meta());check(c.value && *c.value>*b.value);
}
void holes_deleted_retired() {
    Fixture f;
    for(PageId i=1;i<=4;++i)check(f.file().allocate_page().value==i);
    check(!f.file().free_page(1));
    std::vector<RecordId> expected;
    for(PageId id=2;id<=4;++id) {
        RawPage p;check(!RecordPage::initialize(p,id));
        if(id==3) {
            auto a=RecordPage::insert_record(p,id,meta(),{{std::string("a")}});
            auto b=RecordPage::insert_record(p,id,meta(),{{std::string("deleted")}});
            auto c=RecordPage::insert_record(p,id,meta(),{{std::string("retired")}});
            auto d=RecordPage::insert_record(p,id,meta(),{{std::string("d")}});
            check(a.value && b.value && c.value && d.value);
            check(!RecordPage::erase_record(p,id,*b.value));
            check(!RecordPage::erase_record(p,id,*c.value));
            p.bytes[52]=std::byte{0xff};p.bytes[53]=std::byte{0xff};p.bytes[54]=std::byte{2};
            check(!SlottedPage::validate(p,id));expected={*a.value,*d.value};
        }
        check(!f.file().write_page(id,p)); // Never-resident fixture bootstrap only.
    }
    check(!f.pool->close());check(!f.files->close_all());
    check(f.files->open_table_file(0).value.has_value());f.reset_pool();
    HeapTable t(meta(),*f.files,*f.pool);auto pos=t.begin_scan();check(pos.value.has_value());
    for(auto id:expected) {
        auto r=t.next_record(*pos.value);check(r.value && r.value->record_id.value==id.value);
        check(pos.value->next_page==3);check(BufferPoolTestAccess::unpinned_clean(*f.pool));
    }
    auto eof=t.next_record(*pos.value);check(!eof.value && !eof.error);
    check(pos.value->next_page==5 && pos.value->next_slot==0);
    check(BufferPoolTestAccess::unpinned_clean(*f.pool));
}
void boundary_snapshot() {
    Fixture f;HeapTable t(meta(),*f.files,*f.pool);
    // Freeze one empty allocated page, then add a later page using private INSERT.
    check(f.file().allocate_page().value==1);RawPage p;check(!RecordPage::initialize(p,1));check(!f.file().write_page(1,p));
    CursorRegistry cursors(*f.files,*f.pool);auto id=cursors.create(meta());check(id.value.has_value());
    for(int i=0;i<4;++i)check(t.insert_record({{std::string(1024,'s')}}).value.has_value());
    check(f.file().page_count()==3);check(!f.pool->flush_all());
    for(int i=0;i<3;++i)check(cursors.next_record(*id.value).value.has_value());
    auto eof=cursors.next_record(*id.value);check(!eof.value && !eof.error);
    check(cursors.lookup(*id.value).value->position.page_end_exclusive==2);
    const auto fetches=f.pool->stats().fetch_count;
    check(!cursors.next_record(*id.value).error && f.pool->stats().fetch_count==fetches);
    check(BufferPoolTestAccess::unpinned_clean(*f.pool));
    auto pos=t.begin_scan();check(pos.value.has_value());
    for(const auto invalid:std::vector<HeapScanPosition>{{0,0,3},{1,0,0},{4,0,3},
        {1,0,std::uint64_t{std::numeric_limits<PageId>::max()}+2},{1,kMaxSlotsPerPage+1,3},{3,1,3}}) {
        auto v=invalid;auto r=t.next_record(v);check(r.error && r.error->kind==HeapTableErrorKind::kInvalidArgument);
        check(v==invalid);
    }
}
void failed_cursor() {
    for(bool corrupt:{false,true}) {
        Fixture f;HeapTable t(meta(),*f.files,*f.pool);
        if(corrupt) check(f.file().allocate_page().value==1); // Allocated non-TSP1.
        else {check(t.insert_record({{std::string("row")}}).value.has_value());check(!f.pool->flush_all());}
        int reads=0;bool fail_io=!corrupt;
        f.reset_pool([&](PageKey key)->PageFileResult<RawPage>{++reads;
            if(fail_io)return {std::nullopt,PageFileError{PageFileErrorKind::kIo,"injected scan read"}};
            return f.file().read_page(key.page_id);});
        CursorRegistry registry(*f.files,*f.pool);auto id=registry.create(meta());check(id.value.has_value());
        auto before=registry.lookup(*id.value).value->position;
        auto r=registry.next_record(*id.value);check(r.error.has_value());
        check(r.error->kind==(corrupt?CursorErrorKind::kCorrupt:CursorErrorKind::kIo));
        auto state=registry.lookup(*id.value);check(state.value->status==CursorStatus::kFailed);
        check(state.value->position==before && registry.has_cursor(0));
        fail_io=false;const auto count=reads;
        auto again=registry.next_record(*id.value);check(again.error && again.error->kind==r.error->kind && again.error->message==r.error->message);
        check(reads==count && BufferPoolTestAccess::unpinned_clean(*f.pool));
        check(!registry.close(*id.value));check(registry.next_record(*id.value).error->kind==CursorErrorKind::kCursorInvalid);
    }
    Fixture f;HeapTable t(meta(),*f.files,*f.pool);
    for(int i=0;i<4;++i)check(t.insert_record({{std::string(1024,'x')}}).value.has_value());
    check(!f.pool->flush_all());
    CursorRegistry registry(*f.files,*f.pool);auto id=registry.create(meta());check(id.value.has_value());
    auto guard=f.pool->fetch_page({0,2});check(guard.value.has_value());
    auto no_victim=registry.next_record(*id.value);check(no_victim.error && no_victim.error->kind==CursorErrorKind::kNoVictim);
    check(guard.value->pin_count()==1);guard.value->release();
    check(BufferPoolTestAccess::unpinned_clean(*f.pool));
    check(registry.next_record(*id.value).error->kind==CursorErrorKind::kNoVictim);
}
void exhaustion() {
    Fixture f;CursorRegistry registry(*f.files,*f.pool);
    CursorRegistryTestAccess::approach_exhaustion();
    auto last=registry.create(meta());check(last.value && *last.value==std::numeric_limits<CursorId>::max());
    check(registry.create(meta()).error->kind==CursorErrorKind::kInvalidArgument);
    registry.clear();CursorRegistry reopened(*f.files,*f.pool);
    check(reopened.create(meta()).error->kind==CursorErrorKind::kInvalidArgument);
}
void poison_lifetime() {
    Fixture f;NewRecordPageIo io;
    io.bootstrap_write=[](PageFile&,PageId,const RawPage&)->std::optional<PageFileError>{return PageFileError{PageFileErrorKind::kIo,"bootstrap"};};
    io.compensate_free=[](PageFile&,PageId)->std::optional<PageFileError>{return PageFileError{PageFileErrorKind::kIo,"free"};};
    {HeapTable temporary(meta(),*f.files,*f.pool,io);check(temporary.insert_record({{std::string("x")}}).error.has_value());}
    HeapTable another(meta(),*f.files,*f.pool);check(another.begin_scan().error->kind==HeapTableErrorKind::kIo);
    CursorRegistry registry(*f.files,*f.pool);check(registry.create(meta()).error->kind==CursorErrorKind::kIo);
    check(!f.pool->close());check(!f.files->close_all());
    check(f.files->open_table_file(0).value.has_value());f.reset_pool();
    HeapTable reopened(meta(),*f.files,*f.pool);auto pos=reopened.begin_scan();check(pos.value.has_value());
    auto r=reopened.next_record(*pos.value);check(r.error && r.error->kind==HeapTableErrorKind::kCorrupt);
    check(BufferPoolTestAccess::unpinned_clean(*f.pool));
}
}
int main()try{scan_roundtrip();cursors();holes_deleted_retired();boundary_snapshot();failed_cursor();poison_lifetime();exhaustion();
    std::cout<<"heap-scan tests passed\n";}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
