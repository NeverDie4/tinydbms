#include "buffer_pool.h"
#include "record_page.h"

#include <chrono>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace tinydbms::storage::internal {
struct BufferPoolTestAccess {
    static const auto& frames(const BufferPool& pool) { return pool.frames_; }
    static const auto& table(const BufferPool& pool) { return pool.page_table_; }
    static bool open(const BufferPool& pool) { return pool.open_; }
};
}
namespace {
using namespace tinydbms;
using namespace tinydbms::storage;
using namespace tinydbms::storage::internal;
void check(bool condition, std::source_location at=std::source_location::current()) {
    if (!condition) throw std::runtime_error("flush contract at line " + std::to_string(at.line()));
}
void failed(std::optional<BufferPoolError> error, BufferPoolErrorKind kind) {
    check(error && error->kind==kind);
}
void invariants(const BufferPool& pool) {
    const auto& frames=BufferPoolTestAccess::frames(pool);
    const auto& table=BufferPoolTestAccess::table(pool);
    std::size_t occupied=0;
    for (std::size_t i=0;i<frames.size();++i) {
        if (frames[i].key) {
            ++occupied; auto entry=table.find(*frames[i].key);
            check(entry!=table.end() && entry->second==i);
        } else check(!frames[i].dirty && frames[i].pin_count==0);
    }
    check(occupied==table.size());
}
const Frame& frame(const BufferPool& pool, PageKey key) {
    return BufferPoolTestAccess::frames(pool).at(BufferPoolTestAccess::table(pool).at(key));
}
struct Fixture {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("tinydbms-flush-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::unique_ptr<FileManager> files;
    std::unique_ptr<BufferPool> pool;
    std::vector<PageKey> writes;
    std::optional<std::size_t> fail_at;
    PageFileErrorKind failure=PageFileErrorKind::kIo;
    explicit Fixture(bool adapter=true) {
        check(std::filesystem::create_directory(path));
        auto result=FileManager::open(path); check(result.value.has_value()); files=std::move(*result.value);
        for (TableId table : {0U,1U}) {
            auto created=files->create_table_file(table); check(created.value.has_value());
            for (unsigned i=1;i<=3;++i) check((*created.value)->allocate_page().value==i);
        }
        BufferPool::WritePage write;
        if (adapter) write=[this](PageKey key,const RawPage& page)->std::optional<PageFileError> {
            writes.push_back(key);
            if (fail_at && writes.size()==*fail_at) return PageFileError{failure,"injected write failure"};
            return files->find_table_file(key.table_id)->write_page(key.page_id,page);
        };
        auto made=BufferPool::create(*files,3,{},std::move(write));
        check(made.value.has_value()); pool=std::move(*made.value);
    }
    ~Fixture() {
        pool.reset(); files.reset(); std::error_code ec; std::filesystem::remove_all(path,ec);
    }
    PageGuard fetch(PageKey key) {
        auto result=pool->fetch_page(key); check(result.value.has_value()); return std::move(*result.value);
    }
    void dirty(PageKey key) { auto g=fetch(key); g.mutable_page().bytes[0]=std::byte{42}; g.mark_dirty(); }
};

void page_and_guard() {
    Fixture f; auto g=f.fetch({0,1}); const auto initial=f.pool->stats();
    check(!frame(*f.pool,{0,1}).dirty);
    g.mutable_page().bytes[0]=std::byte{9};
    check(!f.pool->flush_page({0,1}) && f.writes.empty());
    check(!f.pool->flush_page({99,99}) && f.writes.empty());
    g.mark_dirty(); check(frame(*f.pool,{0,1}).dirty);
    const auto saved=g.page().bytes;
    f.fail_at=1;
    failed(f.pool->flush_page({0,1}),BufferPoolErrorKind::kIo);
    check(frame(*f.pool,{0,1}).dirty && g.pin_count()==1 && g.page().bytes==saved);
    check(f.pool->stats().dirty_flush_count==0); invariants(*f.pool);
    f.fail_at.reset(); check(!f.pool->flush_page({0,1}));
    check(!frame(*f.pool,{0,1}).dirty && g.pin_count()==1 && f.pool->stats().dirty_flush_count==1);
    check(!f.pool->flush_page({0,1}) && f.writes.size()==2);
    g.mark_dirty(); PageGuard moved(std::move(g)); check(!g.valid());
    for (int op=0;op<3;++op) {
        bool threw=false;
        try { if(op==0) g.mark_dirty(); else if(op==1) (void)g.mutable_page(); else (void)g.page(); }
        catch(const std::logic_error&) { threw=true; } check(threw);
    }
    moved.release(); moved.release();
    check(frame(*f.pool,{0,1}).dirty && frame(*f.pool,{0,1}).pin_count==0);
    bool threw=false; try { moved.mark_dirty(); } catch(const std::logic_error&) { threw=true; } check(threw);
    check(!f.pool->flush_all()); check(f.pool->stats().dirty_flush_count==2);
    check(f.pool->stats().fetch_count==initial.fetch_count && f.pool->stats().hit_count==initial.hit_count &&
          f.pool->stats().miss_count==initial.miss_count);
    check(!f.pool->close());
}

void batches() {
    // Frame order deliberately differs from TableId/PageId order.
    const std::vector<PageKey> keys{{1,2},{0,2},{0,1}};
    for (int operation=0;operation<3;++operation) {
        Fixture f; for(auto key:keys) f.dirty(key);
        if (operation==0) {
            f.fail_at=2; failed(f.pool->flush_all(),BufferPoolErrorKind::kIo);
            check(f.writes==std::vector<PageKey>({keys[0],keys[1]}));
            check(!frame(*f.pool,keys[0]).dirty && frame(*f.pool,keys[1]).dirty && frame(*f.pool,keys[2]).dirty);
        } else if(operation==1) {
            // prepare must notice later pinned frame before flushing earlier dirty frame.
            auto pinned=f.fetch(keys[2]);
            failed(f.pool->release_table(0),BufferPoolErrorKind::kInvalidArgument); check(f.writes.empty());
            pinned.release(); f.fail_at=2;
            failed(f.pool->release_table(0),BufferPoolErrorKind::kIo);
            check(f.writes==std::vector<PageKey>({keys[1],keys[2]}));
            check(frame(*f.pool,keys[0]).dirty && !frame(*f.pool,keys[1]).dirty && frame(*f.pool,keys[2]).dirty);
        } else {
            auto pinned=f.fetch(keys[2]);
            failed(f.pool->close(),BufferPoolErrorKind::kInvalidArgument); check(f.writes.empty());
            pinned.release(); f.fail_at=2; failed(f.pool->close(),BufferPoolErrorKind::kIo);
            check(!frame(*f.pool,keys[0]).dirty && frame(*f.pool,keys[1]).dirty && frame(*f.pool,keys[2]).dirty);
        }
        invariants(*f.pool); check(BufferPoolTestAccess::table(*f.pool).size()==3);
        check(BufferPoolTestAccess::open(*f.pool) && f.pool->stats().dirty_flush_count==1);
        for(auto key:keys) { auto g=f.fetch(key); check(g.page().bytes[0]==std::byte{42}); }
        {
            std::vector<PageGuard> pinned;
            for(auto key:keys) pinned.push_back(f.fetch(key));
            auto full=f.pool->fetch_page({1,3}); check(full.error && full.error->kind==BufferPoolErrorKind::kNoVictim);
        }
        f.fail_at.reset();
        if(operation==1) {
            check(!f.pool->release_table(0)); check(f.pool->stats().dirty_flush_count==2);
            check(BufferPoolTestAccess::table(*f.pool).size()==1 && frame(*f.pool,keys[0]).dirty);
            check(f.files->find_table_file(0)->is_open());
            auto fresh=f.fetch({0,3}); check(fresh.page().bytes[0]==std::byte{0}); fresh.release();
            const auto count=f.writes.size(); check(!f.pool->release_table(0)); check(f.writes.size()==count);
            check(!f.pool->release_table(99));
        } else if(operation==0) {
            check(!f.pool->flush_all());
            check(f.writes==std::vector<PageKey>({keys[0],keys[1],keys[1],keys[2]}));
        }
        check(!f.pool->close()); invariants(*f.pool);
        check(f.pool->stats().dirty_flush_count==3 && !BufferPoolTestAccess::open(*f.pool));
        check(BufferPoolTestAccess::table(*f.pool).empty());
        check(f.pool->fetch_page(keys[0]).error.has_value());
        const auto count=f.writes.size(); check(!f.pool->close()); check(f.writes.size()==count);
    }
}

void errors_and_persistence() {
    Fixture f;
    for(auto kind:{PageFileErrorKind::kCorrupt,PageFileErrorKind::kInvalidArgument}) {
        f.dirty({0,1}); f.failure=kind; f.fail_at=f.writes.size()+1;
        failed(f.pool->flush_page({0,1}),kind==PageFileErrorKind::kCorrupt ? BufferPoolErrorKind::kCorrupt : BufferPoolErrorKind::kInvalidArgument);
        check(frame(*f.pool,{0,1}).dirty); invariants(*f.pool);
    }
    f.fail_at.reset(); check(!f.pool->close());
    Fixture real(false); // Default production write path, no adapter.
    TableMeta meta{0,"record",{{"id",Type::kInt}}};
    auto g=real.fetch({0,1}); check(!RecordPage::initialize(g.mutable_page(),1));
    auto record=RecordPage::insert_record(g.mutable_page(),1,meta,{Value{std::int32_t{123}}});
    check(record.value.has_value()); g.mark_dirty(); g.release();
    check(!real.pool->close()); check(real.pool->stats().dirty_flush_count==1);
    check(!real.files->close_all());
    auto opened=real.files->open_table_file(0); check(opened.value.has_value());
    auto raw=(*opened.value)->read_page(1); check(raw.value.has_value());
    auto restored=RecordPage::get_record(*raw.value,1,meta,*record.value);
    check(restored.value && std::get<std::int32_t>(restored.value->values[0].data)==123);
}
}
int main() {
    try { page_and_guard(); batches(); errors_and_persistence(); }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
    return 0;
}
