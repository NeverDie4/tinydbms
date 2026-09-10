#include "buffer_pool.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <source_location>
#include <stdexcept>

namespace tinydbms::storage::internal {
struct BufferPoolTestAccess {
    static const auto& frames(const BufferPool& p) { return p.frames_; }
    static const auto& table(const BufferPool& p) { return p.page_table_; }
    static const auto& order(const BufferPool& p) { return p.fifo_.order_; }
};
}
namespace {
using namespace tinydbms;
using namespace tinydbms::storage::internal;
void check(bool ok, std::source_location at=std::source_location::current()) {
    if(!ok) throw std::runtime_error("FIFO check at line "+std::to_string(at.line()));
}
constexpr PageKey A{0,1}, B{0,2}, C{1,1}, D{1,2}, E{0,3};
struct Fixture {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("tinydbms-fifo-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::unique_ptr<FileManager> files;
    std::unique_ptr<BufferPool> pool;
    std::size_t reads=0,writes=0;
    std::optional<std::size_t> fail_read,fail_write;
    std::string events;
    std::string logs;
    Fixture(ReplacementPolicy policy=ReplacementPolicy::kFifo, bool logging=true) {
        check(std::filesystem::create_directory(path));
        auto fm=FileManager::open(path); check(fm.value.has_value()); files=std::move(*fm.value);
        for(TableId t:{0U,1U}) {
            auto f=files->create_table_file(t); check(f.value.has_value());
            for(unsigned i=1;i<=3;++i) check((*f.value)->allocate_page().value==i);
        }
        auto made=BufferPool::create(*files,3,
            [this](PageKey k)->PageFileResult<RawPage> {
                ++reads; events+='R';
                if(fail_read==reads) return {std::nullopt,PageFileError{PageFileErrorKind::kIo,"read fault"}};
                return files->find_table_file(k.table_id)->read_page(k.page_id);
            },
            [this](PageKey k,const RawPage& p)->std::optional<PageFileError> {
                ++writes; events+='W';
                if(fail_write==writes) return PageFileError{PageFileErrorKind::kIo,"write fault"};
                return files->find_table_file(k.table_id)->write_page(k.page_id,p);
            }, policy, logging ? BufferPool::LogSink{[this](std::string_view text) {
                logs.append(text); logs+='\n';
            }} : BufferPool::LogSink{});
        check(made.value.has_value()); pool=std::move(*made.value);
    }
    ~Fixture() { pool.reset(); files.reset(); std::error_code ec; std::filesystem::remove_all(path,ec); }
    void verify() {
        const auto& frames=BufferPoolTestAccess::frames(*pool);
        const auto& table=BufferPoolTestAccess::table(*pool);
        const auto& order=BufferPoolTestAccess::order(*pool);
        check(order.size()==table.size());
        for(FrameId i=0;i<frames.size();++i) {
            auto count=std::count(order.begin(),order.end(),i);
            if(frames[i].key) {
                check(count==1 && table.at(*frames[i].key)==i);
            } else check(count==0 && !frames[i].dirty && frames[i].pin_count==0);
        }
        for(auto id:order) check(id<frames.size() && frames[id].key.has_value());
    }
    PageGuard fetch(PageKey k) { auto g=pool->fetch_page(k); check(g.value.has_value()); verify(); return std::move(*g.value); }
    bool has(PageKey k) { return BufferPoolTestAccess::table(*pool).contains(k); }
    void load() { for(auto k:{A,B,C}) { auto g=fetch(k); } events.clear(); }
};
void ordering() {
    Fixture f; f.load();
    for(int i=0;i<5;++i) { auto a=f.fetch(A); }
    { auto d=f.fetch(D); }
    check(!f.has(A) && f.has(B) && f.has(C) && f.has(D));
    check(f.events=="R" && f.writes==0);
    check(f.pool->stats().eviction_count==1);
    Fixture pinned; pinned.load();
    auto a=pinned.fetch(A);
    { auto d=pinned.fetch(D); }
    check(pinned.has(A) && !pinned.has(B));
    a.release(); { auto e=pinned.fetch(E); }
    check(!pinned.has(A) && pinned.has(C));
    check(pinned.pool->stats().eviction_count==2);
}
void failures(ReplacementPolicy policy=ReplacementPolicy::kFifo) {
    Fixture f(policy); f.load();
    auto a=f.fetch(A),b=f.fetch(B),c=f.fetch(C);
    a.mutable_page().bytes[0]=std::byte{77}; a.mark_dirty();
    const auto before=BufferPoolTestAccess::frames(*f.pool);
    const auto order=BufferPoolTestAccess::order(*f.pool);
    auto no=f.pool->fetch_page(D); check(no.error && no.error->kind==BufferPoolErrorKind::kNoVictim);
    check(f.events.empty());
    f.verify(); check(BufferPoolTestAccess::order(*f.pool)==order);
    for(std::size_t i=0;i<before.size();++i) {
        const auto& now=BufferPoolTestAccess::frames(*f.pool)[i];
        check(now.page.bytes==before[i].page.bytes && now.key==before[i].key &&
              now.pin_count==before[i].pin_count && now.dirty==before[i].dirty);
    }
    a.release(); b.release(); c.release();
    const auto old=BufferPoolTestAccess::frames(*f.pool);
    auto unchanged=[&] {
        f.verify(); check(BufferPoolTestAccess::order(*f.pool)==order);
        check(f.pool->stats().eviction_count==0);
        const auto& now=BufferPoolTestAccess::frames(*f.pool);
        check(f.has(A) && f.has(B) && f.has(C) && !f.has(D));
        for(std::size_t i=0;i<old.size();++i)
            check(now[i].key==old[i].key && now[i].page.bytes==old[i].page.bytes &&
                  now[i].dirty==old[i].dirty && now[i].pin_count==old[i].pin_count);
    };
    f.fail_read=f.reads+1;
    auto r=f.pool->fetch_page(D); check(r.error && r.error->kind==BufferPoolErrorKind::kIo);
    check(f.events=="R" && f.writes==0); unchanged();
    f.fail_read.reset(); f.fail_write=f.writes+1; f.events.clear();
    auto w=f.pool->fetch_page(D); check(w.error && w.error->kind==BufferPoolErrorKind::kIo);
    check(f.events=="RW" && f.pool->stats().dirty_flush_count==0); unchanged();
    check(f.logs.find("Evict ")==std::string::npos);
    check(f.logs.find("Flush dirty table=0 page=1")!=std::string::npos &&
          f.logs.find("result=failure")!=std::string::npos);
    f.fail_write.reset(); f.events.clear(); { auto d=f.fetch(D); }
    check(f.events=="RW" && !f.has(A) && f.has(D));
    check(f.pool->stats().dirty_flush_count==1);
    check(f.pool->stats().eviction_count==1);
    check(f.logs.find("result=success")!=std::string::npos);
    auto disk=f.files->find_table_file(0)->read_page(1); check(disk.value && disk.value->bytes[0]==std::byte{77});
    check(f.pool->stats().fetch_count==f.pool->stats().hit_count+f.pool->stats().miss_count);
}
void lifecycle(ReplacementPolicy policy=ReplacementPolicy::kFifo) {
    Fixture f(policy); f.load(); auto a=f.fetch(A); a.mark_dirty();
    const auto order=BufferPoolTestAccess::order(*f.pool);
    check(f.pool->release_table(0).has_value() && f.events.empty());
    check(f.pool->close().has_value() && f.events.empty());
    a.release(); f.fail_write=1;
    check(f.pool->release_table(0).has_value() && f.has(A) && f.has(B));
    f.fail_write=2; check(f.pool->close().has_value() && f.has(A));
    f.verify(); check(BufferPoolTestAccess::order(*f.pool)==order);
    f.fail_write.reset(); check(!f.pool->release_table(0));
    check(!f.has(A) && !f.has(B) && f.has(C));
    f.verify(); check(BufferPoolTestAccess::order(*f.pool)==std::vector<FrameId>{2});
    f.events.clear(); { auto d=f.fetch(D); } { auto e=f.fetch(E); }
    check(f.has(C) && f.events=="RR"); // Empty frames before any victim.
    check(f.pool->stats().eviction_count==0);
    { auto a2=f.fetch(A); } check(!f.has(C));
    check(!f.pool->close()); check(BufferPoolTestAccess::table(*f.pool).empty());
    f.verify(); check(BufferPoolTestAccess::order(*f.pool).empty());
}
void lru_and_logs() {
    for(auto policy:{ReplacementPolicy::kFifo,ReplacementPolicy::kLru}) {
        Fixture f(policy); f.load(); { auto a=f.fetch(A); } { auto d=f.fetch(D); }
        check(f.has(A)==(policy==ReplacementPolicy::kLru));
        check(f.has(B)==(policy==ReplacementPolicy::kFifo));
        auto stats=f.pool->stats();
        check(stats.fetch_count==5 && stats.hit_count==1 && stats.miss_count==4 &&
              stats.eviction_count==1 && stats.dirty_flush_count==0 && stats.hit_rate()==0.2);
        check(f.logs.find("Buffer HIT table=0 page=1 frame=")!=std::string::npos);
        check(f.logs.find("Buffer MISS table=")!=std::string::npos);
        check(f.logs.find(policy==ReplacementPolicy::kFifo ? "policy=FIFO" : "policy=LRU")!=std::string::npos);
    }
    Fixture f(ReplacementPolicy::kLru);
    auto a=f.fetch(A), b=f.fetch(B), c=f.fetch(C);
    b.release(); c.release(); { auto d=f.fetch(D); } check(!f.has(B));
    a.mark_dirty(); check(!f.pool->flush_page(A)); a.release();
    { auto e=f.fetch(E); } check(!f.has(A)); // unpin/dirty/flush did not renew A.
    Fixture disabled(ReplacementPolicy::kLru,false); disabled.load(); { auto d=disabled.fetch(D); }
    check(disabled.logs.empty());
    auto invalid=BufferPool::create(*disabled.files,3,{},{},static_cast<ReplacementPolicy>(99));
    check(invalid.error && invalid.error->kind==BufferPoolErrorKind::kInvalidArgument);
    auto throwing=BufferPool::create(*disabled.files,1,{},{},ReplacementPolicy::kLru,
        [](std::string_view) { throw std::runtime_error("log sink failure"); });
    check(throwing.value.has_value());
    { auto g=(*throwing.value)->fetch_page(A); check(g.value.has_value()); }
    check(!(*throwing.value)->close()); // Diagnostics cannot change storage results.
}
void component() {
    FifoReplacer fifo(3);
    fifo.record_load(0); fifo.record_load(1); fifo.record_load(2);
    auto all=[](FrameId) { return true; };
    check(fifo.choose_victim(all)==0 && fifo.choose_victim(all)==0);
    check(fifo.choose_victim([](FrameId i) { return i!=0; })==1);
    check(fifo.choose_victim(all)==0);
    fifo.remove(0); fifo.record_load(0); check(fifo.choose_victim(all)==1);
    fifo.clear(); check(!fifo.choose_victim(all));
}
}
int main() {
    try { component(); ordering(); lru_and_logs(); failures(); lifecycle();
          failures(ReplacementPolicy::kLru); lifecycle(ReplacementPolicy::kLru); }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
    return 0;
}
