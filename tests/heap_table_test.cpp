#include "heap_table.h"
#include <chrono>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace tinydbms::storage::internal {
struct BufferPoolTestAccess {
    static bool no_pins(const BufferPool& p) {
        for (const auto& f : p.frames_) if (f.pin_count) return false;
        return true;
    }
    static bool clean(const BufferPool& p) {
        for (const auto& f : p.frames_) if (f.dirty) return false;
        return true;
    }
};
struct HeapTableTestAccess {
    static auto postvalidation_insert(HeapTable& t, PageFile& f, const std::vector<Value>& values) {
        return t.create_record_page(f, values);
    }
};
}
namespace {
using namespace tinydbms;
using namespace tinydbms::storage;
using namespace tinydbms::storage::internal;
void check(bool ok, std::source_location at = std::source_location::current()) {
    if (!ok) throw std::runtime_error("heap-table line " + std::to_string(at.line()));
}
struct Temp {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("tinydbms-heap-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { check(std::filesystem::create_directory(path)); }
    ~Temp() { std::error_code e; std::filesystem::remove_all(path,e); }
};
TableMeta schema(TableId id = 0) { return {id,"heap",{{"s",Type::kVarchar}}}; }
std::vector<Value> row(std::size_t n = 10) { return {{std::string(n,'x')}}; }
PageId page_id(RecordId rid) { auto r=RecordIdCodec::decode(rid); check(r.value.has_value()); return r.value->page_id; }
PageFileError io_error() { return {PageFileErrorKind::kIo,"injected I/O failure"}; }
struct Fixture {
    Temp temp;
    std::unique_ptr<FileManager> files;
    std::unique_ptr<BufferPool> pool;
    Fixture() {
        auto f=FileManager::open(temp.path); check(f.value.has_value()); files=std::move(*f.value);
        check(files->create_table_file(0).value.has_value()); reset_pool();
    }
    void reset_pool(BufferPool::ReadPage read = {}, BufferPool::WritePage write = {}) {
        if(pool) check(!pool->close());
        auto p=BufferPool::create(*files,1,std::move(read),std::move(write));
        check(p.value.has_value()); pool=std::move(*p.value);
    }
    PageFile& file(TableId id=0) { auto f=files->find_table_file(id); check(f!=nullptr); return *f; }
    void expect(RecordId rid, const std::vector<Value>& values, TableId id=0) {
        auto g=pool->fetch_page({id,page_id(rid)}); check(g.value.has_value());
        auto r=RecordPage::get_record(g.value->page(),page_id(rid),schema(id),rid); check(r.value.has_value());
        check(r.value->values.size()==values.size());
        for(std::size_t i=0;i<values.size();++i) check(r.value->values[i].data==values[i].data);
    }
    // Existing lower-level erase is fixture preparation, not a HeapTable DELETE API.
    void prepare_space(RecordId rid) {
        auto g=pool->fetch_page({0,page_id(rid)}); check(g.value.has_value());
        check(!RecordPage::erase_record(g.value->mutable_page(),page_id(rid),rid)); g.value->mark_dirty();
    }
    void empty(PageId id) {
        auto g=pool->fetch_page({0,id}); check(g.value.has_value());
        check(!SlottedPage::validate(g.value->page(),id));
        check(g.value->page().bytes[14]==std::byte{0} && g.value->page().bytes[15]==std::byte{0});
    }
};
void first_fit_and_persistence() {
    Fixture f; HeapTable table(schema(),*f.files,*f.pool);
    std::vector<RecordId> ids;
    for(int i=0;i<10;++i) { auto r=table.insert_record(row(1024)); check(r.value.has_value()); ids.push_back(*r.value); }
    check(page_id(ids[0])==1 && page_id(ids[2])==1 && page_id(ids[3])==2 && page_id(ids[9])==4);
    check(f.file().page_count()==5); check(BufferPoolTestAccess::no_pins(*f.pool));
    f.prepare_space(ids[1]);
    auto reused=table.insert_record(row(1024)); check(reused.value.has_value()); check(page_id(*reused.value)==1);
    check(RecordIdCodec::decode(*reused.value).value->generation==2);
    for(int i=3;i<6;++i) f.prepare_space(ids[i]);
    auto earlier=table.insert_record(row(1024)); check(earlier.value.has_value()); check(page_id(*earlier.value)==2);
    for(auto i : {0,2,6,7,8,9}) f.expect(ids[i],row(1024));
    check(!f.pool->close()); check(!f.files->close_all());
    check(f.files->open_table_file(0).value.has_value()); f.reset_pool();
    for(auto i : {0,2,6,7,8,9}) f.expect(ids[i],row(1024));
    f.expect(*reused.value,row(1024)); f.expect(*earlier.value,row(1024));
    check(!f.pool->close());
}
void free_and_corrupt() {
    Fixture f;
    check(f.file().allocate_page().value==1); check(!f.file().free_page(1));
    check(f.file().allocate_page().value==1); // Setup allocated zero/non-TSP1 page.
    HeapTable t(schema(),*f.files,*f.pool);
    auto corrupt=t.insert_record(row()); check(corrupt.error && corrupt.error->kind==HeapTableErrorKind::kCorrupt);
    check(f.file().page_count()==2); check(BufferPoolTestAccess::clean(*f.pool));
    check(!f.pool->release_table(0)); check(!f.file().free_page(1));
    // Reopen a genuine hole before testing skip and eventual allocation reuse.
    check(!f.pool->close()); check(!f.files->close_all());
    check(f.files->open_table_file(0).value.has_value()); f.reset_pool();
    HeapTable reopened(schema(),*f.files,*f.pool);
    auto inserted=reopened.insert_record(row()); check(inserted.value.has_value()); check(page_id(*inserted.value)==1);
    check(!f.pool->close());
}
void prevalidation() {
    Fixture f; HeapTable t(schema(),*f.files,*f.pool);
    for(const auto& values : std::vector<std::vector<Value>>{{},{{std::int32_t{1}}},{{std::string("\xC0\xAF")}}}) {
        auto r=t.insert_record(values); check(r.error && r.error->kind==HeapTableErrorKind::kInvalidArgument);
    }
    auto large=t.insert_record(row(1025)); check(large.error && large.error->kind==HeapTableErrorKind::kValueTooLarge);
    TableMeta many{0,"many",{}}; std::vector<Value> values;
    for(int i=0;i<5;++i) { many.columns.push_back({"s",Type::kVarchar}); values.push_back({std::string(1024,'x')}); }
    HeapTable wide(many,*f.files,*f.pool);
    auto logical=wide.insert_record(values); check(logical.error && logical.error->kind==HeapTableErrorKind::kValueTooLarge);
    many.columns.pop_back(); values.pop_back(); HeapTable physical(many,*f.files,*f.pool);
    auto encoded=physical.insert_record(values); check(encoded.error && encoded.error->kind==HeapTableErrorKind::kValueTooLarge);
    check(f.file().page_count()==1 && f.pool->stats().fetch_count==0);
    check(BufferPoolTestAccess::clean(*f.pool));
}
void failures() {
    { Fixture f; int frees=0;
      NewRecordPageIo ops;
      ops.allocate=[](PageFile&)->PageFileResult<PageId>{return {std::nullopt,io_error()};};
      ops.compensate_free=[&](PageFile&,PageId)->std::optional<PageFileError>{++frees; return {};};
      HeapTable t(schema(),*f.files,*f.pool,ops); auto r=t.insert_record(row());
      check(r.error && r.error->kind==HeapTableErrorKind::kIo); check(frees==0 && f.file().page_count()==1); }
    for(bool fail_free : {false,true}) {
        Fixture f; int frees=0;
        NewRecordPageIo ops;
        ops.bootstrap_write=[](PageFile&,PageId,const RawPage&)->std::optional<PageFileError>{return io_error();};
        ops.compensate_free=[&](PageFile& file,PageId id)->std::optional<PageFileError>{
            ++frees; return fail_free ? std::optional{io_error()} : file.free_page(id); };
        HeapTable t(schema(),*f.files,*f.pool,ops); auto r=t.insert_record(row());
        check(r.error && r.error->kind==HeapTableErrorKind::kIo); check(frees==1 && f.pool->stats().fetch_count==0);
        HeapTable retry(schema(),*f.files,*f.pool);
        if(fail_free) {
            auto again=retry.insert_record(row()); check(again.error && again.error->kind==HeapTableErrorKind::kIo);
            check(f.file().page_allocation_state(1).error->kind==PageFileErrorKind::kIo);
        } else {
            check(f.file().page_allocation_state(1).value==PageAllocationState::kFree);
            auto again=retry.insert_record(row()); check(again.value.has_value() && page_id(*again.value)==1);
        }
        check(BufferPoolTestAccess::no_pins(*f.pool)); check(!f.pool->close());
    }
    { Fixture f; bool fail=true;
      f.reset_pool([&](PageKey key)->PageFileResult<RawPage>{
          if(fail) return {std::nullopt,io_error()}; return f.file(key.table_id).read_page(key.page_id); });
      HeapTable t(schema(),*f.files,*f.pool); auto r=t.insert_record(row());
      check(r.error && r.error->kind==HeapTableErrorKind::kIo);
      check(f.file().page_count()==2 && f.file().page_allocation_state(1).value==PageAllocationState::kAllocated);
      fail=false; f.empty(1); auto retry=t.insert_record(row()); check(retry.value.has_value());
      check(page_id(*retry.value)==1 && f.file().page_count()==2); check(!f.pool->close()); }
    { Fixture f;
      HeapTable t(schema(),*f.files,*f.pool);
      // Direct helper test supplies a bad row AFTER the normal prevalidation boundary.
      auto r=HeapTableTestAccess::postvalidation_insert(t,f.file(),{});
      check(r.error && r.error->kind==HeapTableErrorKind::kInvalidArgument);
      f.empty(1); check(BufferPoolTestAccess::clean(*f.pool)); check(BufferPoolTestAccess::no_pins(*f.pool)); }
}
void no_victim_and_batch() {
    Fixture f; check(f.files->create_table_file(1).value.has_value());
    HeapTable t0(schema(),*f.files,*f.pool), t1(schema(1),*f.files,*f.pool);
    auto a=t0.insert_record(row()); check(a.value.has_value());
    auto pin=f.pool->fetch_page({0,1}); check(pin.value.has_value());
    auto blocked=t1.insert_record(row()); check(blocked.error && blocked.error->kind==HeapTableErrorKind::kNoVictim);
    check(f.file(1).page_count()==2); pin.value->release();
    auto b=t1.insert_record(row(20)); check(b.value.has_value()); check(f.file(1).page_count()==2);
    f.expect(*a.value,row()); f.expect(*b.value,row(20),1);
    auto batch=t0.insert_batch({row(),row(),{{std::int32_t{3}}},row()});
    check(batch.record_ids.size()==2 && batch.error && batch.error->kind==HeapTableErrorKind::kInvalidArgument);
    for(auto rid:batch.record_ids) f.expect(rid,row());
    auto success=t0.insert_batch({row(),row()}); check(!success.error && success.record_ids.size()==2);
    auto empty=t0.insert_batch({}); check(!empty.error && empty.record_ids.empty());
    HeapTable missing(schema(99),*f.files,*f.pool); check(missing.insert_batch({}).error.has_value());
    check(BufferPoolTestAccess::no_pins(*f.pool)); check(!f.pool->close());
}
void holes_and_empty_directory() {
    Fixture f;
    check(f.file().allocate_page().value==1); check(f.file().allocate_page().value==2);
    RawPage scratch; check(!RecordPage::initialize(scratch,2));
    check(!f.file().write_page(2,scratch)); check(!f.file().free_page(1));
    check(!f.pool->close()); check(!f.files->close_all());
    check(f.files->open_table_file(0).value.has_value()); f.reset_pool();
    HeapTable t(schema(),*f.files,*f.pool);
    auto r=t.insert_record(row()); check(r.value.has_value() && page_id(*r.value)==2);
    check(f.file().page_allocation_state(1).value==PageAllocationState::kFree);
    check(!f.pool->close());

    Fixture churn;
    check(churn.file().allocate_page().value==1);
    check(!RecordPage::initialize(scratch,1));
    std::vector<SlotHandle> handles;
    const std::vector<std::byte> byte{std::byte{1}};
    for(int i=0;i<400;++i) { auto s=SlottedPage::insert(scratch,1,byte); check(s.value.has_value()); handles.push_back(*s.value); }
    for(auto s:handles) check(!SlottedPage::erase(scratch,1,s));
    check(!SlottedPage::validate(scratch,1)); check(!churn.file().write_page(1,scratch));
    HeapTable ct(schema(),*churn.files,*churn.pool);
    auto big=ct.insert_record(row(1024)); check(big.value.has_value() && page_id(*big.value)==2);
    auto small=ct.insert_record(row()); check(small.value.has_value() && page_id(*small.value)==1);
    check(!churn.pool->close());
}
void stop_on_existing_io_and_preserve_clean() {
    Fixture f;
    HeapTable initial(schema(),*f.files,*f.pool);
    check(initial.insert_batch({row(1024),row(1024),row(1024)}).record_ids.size()==3);
    check(!f.pool->flush_all());
    bool fail_write=true;
    f.reset_pool({},[&](PageKey key,const RawPage& page)->std::optional<PageFileError>{
        if(fail_write) return io_error(); return f.file(key.table_id).write_page(key.page_id,page); });
    HeapTable t(schema(),*f.files,*f.pool);
    { auto g=f.pool->fetch_page({0,1}); check(g.value.has_value()); g.value->mark_dirty(); }
    auto failure=t.insert_record(row(1024));
    check(failure.error && failure.error->kind==HeapTableErrorKind::kIo);
    check(f.file().page_count()==3); check(BufferPoolTestAccess::no_pins(*f.pool));
    // Valid new Page 2 remains; retry must not allocate Page 3.
    fail_write=false;
    auto retry=t.insert_record(row(1024)); check(retry.value.has_value() && page_id(*retry.value)==2);
    check(f.file().page_count()==3); check(!f.pool->flush_all());
    f.reset_pool([](PageKey)->PageFileResult<RawPage>{return {std::nullopt,io_error()};});
    HeapTable unreadable(schema(),*f.files,*f.pool);
    auto stopped=unreadable.insert_record(row()); check(stopped.error && stopped.error->kind==HeapTableErrorKind::kIo);
    check(f.file().page_count()==3 && BufferPoolTestAccess::clean(*f.pool));
    check(BufferPoolTestAccess::no_pins(*f.pool));
}
}
int main() try {
    first_fit_and_persistence(); free_and_corrupt(); prevalidation(); failures(); no_victim_and_batch();
    holes_and_empty_directory(); stop_on_existing_io_and_preserve_clean();
    std::cout << "heap-table tests passed\n";
} catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
