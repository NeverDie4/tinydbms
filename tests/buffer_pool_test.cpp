#include "buffer_pool.h"

#include <chrono>
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace tinydbms::storage::internal;
void check(bool condition, std::source_location at = std::source_location::current()) {
    if (!condition) throw std::runtime_error("BufferPool check at line " + std::to_string(at.line()));
}
template <typename T> void error(const BufferPoolResult<T>& result, BufferPoolErrorKind kind) {
    check(!result.value && result.error && result.error->kind == kind);
}
struct Directory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("tinydbms-buffer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Directory() { check(std::filesystem::create_directory(path)); }
    ~Directory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
struct Fixture {
    Directory directory;
    std::unique_ptr<FileManager> files;
    Fixture() {
        auto opened = FileManager::open(directory.path); check(opened.value.has_value());
        files = std::move(*opened.value);
        for (tinydbms::TableId table : {0U,1U}) {
            auto created = files->create_table_file(table); check(created.value.has_value());
            for (unsigned i=1; i<=3; ++i) {
                auto allocated = (*created.value)->allocate_page(); check(allocated.value == i);
                RawPage page; page.bytes.fill(std::byte{static_cast<unsigned char>(table*10+i)});
                check(!(*created.value)->write_page(i,page));
            }
        }
    }
};
void frame_contract() {
    Frame frame; check(!frame.key && !frame.dirty && frame.pin_count==0);
    check(!frame.try_unpin()); check(frame.pin_count==0);
    check(frame.try_pin()); check(frame.try_unpin());
    frame.pin_count=std::numeric_limits<std::uint32_t>::max();
    check(!frame.try_pin()); check(frame.pin_count==std::numeric_limits<std::uint32_t>::max());
    check(PageKey{0,1}==PageKey{0,1}); check(!(PageKey{0,1}==PageKey{1,1}));
    check(PageKeyHash{}({0,1})==PageKeyHash{}({0,1}));
}
void guard_and_cache() {
    Fixture fixture;
    error(BufferPool::create(*fixture.files,0),BufferPoolErrorKind::kInvalidArgument);
    std::size_t reads=0;
    auto made=BufferPool::create(*fixture.files,2,[&](PageKey key) {
        ++reads; return fixture.files->find_table_file(key.table_id)->read_page(key.page_id);
    });
    check(made.value.has_value()); auto pool=std::move(*made.value);
    check(pool->stats().hit_rate()==0);
    {
        auto a=pool->fetch_page({0,1}); check(a.value.has_value());
        check(a.value->pin_count()==1 && a.value->page().bytes[0]==std::byte{1});
        const auto* address=&a.value->page();
        auto b=pool->fetch_page({0,1}); check(b.value.has_value());
        check(b.value->pin_count()==2 && &b.value->page()==address && reads==1);
        PageGuard moved(std::move(*b.value)); check(!b.value->valid() && moved.valid());
        moved.release(); moved.release(); check(a.value->pin_count()==1);
        auto c=pool->fetch_page({1,1}); check(c.value.has_value());
        check(c.value->page().bytes[0]==std::byte{11});
        // Move assignment releases its previous, different frame pin.
        auto same=pool->fetch_page({0,1}); check(same.value.has_value());
        *same.value=std::move(*c.value); check(!c.value->valid());
        check(a.value->pin_count()==1 && same.value->page().bytes[0]==std::byte{11});
        check(pool->close().has_value()); // Still pinned; pool remains usable.
        error(pool->fetch_page({0,2}),BufferPoolErrorKind::kNoVictim);
        check(&a.value->page()==address && a.value->page().bytes[0]==std::byte{1});
    }
    auto a=pool->fetch_page({0,1}); check(a.value->pin_count()==1); a.value->release();
    { auto replacement=pool->fetch_page({0,2}); check(replacement.value.has_value()); } // Phase 3D FIFO.
    for (int i=0;i<10;++i) { auto guard=pool->fetch_page({0,1}); check(guard.value->pin_count()==1); }
    auto stats=pool->stats(); check(stats.fetch_count==17 && stats.hit_count==12 && stats.miss_count==5);
    check(stats.fetch_count==stats.hit_count+stats.miss_count && stats.hit_rate()==12.0/17.0);
    check(reads==4);
    check(!pool->close()); check(!pool->close());
    error(pool->fetch_page({0,1}),BufferPoolErrorKind::kInvalidArgument);
    check(pool->stats().fetch_count==stats.fetch_count);
    PageGuard invalid; invalid.release(); check(!invalid.valid());
    bool threw=false; try { (void)invalid.page(); } catch(const std::logic_error&) { threw=true; }
    check(threw);
}
void failures() {
    Fixture fixture;
    auto made=BufferPool::create(*fixture.files,3); check(made.value.has_value());
    auto pool=std::move(*made.value);
    error(pool->fetch_page({0,0}),BufferPoolErrorKind::kInvalidArgument);
    error(pool->fetch_page({99,1}),BufferPoolErrorKind::kInvalidArgument);
    check(pool->stats().fetch_count==0);
    error(pool->fetch_page({0,99}),BufferPoolErrorKind::kInvalidArgument);
    check(!fixture.files->find_table_file(0)->free_page(3));
    error(pool->fetch_page({0,3}),BufferPoolErrorKind::kInvalidArgument);
    for (auto key : {PageKey{0,1},PageKey{0,2},PageKey{1,1}}) {
        auto result=pool->fetch_page(key); check(result.value.has_value());
    }
    check(pool->stats().miss_count==5); check(!pool->close());

    std::optional<PageFileErrorKind> fail=PageFileErrorKind::kIo;
    std::size_t reads=0;
    auto injected=BufferPool::create(*fixture.files,2,[&](PageKey key) -> PageFileResult<RawPage> {
        ++reads;
        if (fail) return {std::nullopt,PageFileError{*fail,"injected read failure"}};
        return fixture.files->find_table_file(key.table_id)->read_page(key.page_id);
    });
    check(injected.value.has_value()); auto fault_pool=std::move(*injected.value);
    error(fault_pool->fetch_page({0,1}),BufferPoolErrorKind::kIo);
    fail=PageFileErrorKind::kCorrupt;
    error(fault_pool->fetch_page({0,1}),BufferPoolErrorKind::kCorrupt);
    fail.reset(); auto a=fault_pool->fetch_page({0,1}); check(a.value.has_value());
    const auto saved=a.value->page().bytes;
    fail=PageFileErrorKind::kIo;
    error(fault_pool->fetch_page({0,2}),BufferPoolErrorKind::kIo);
    check(a.value->page().bytes==saved && a.value->pin_count()==1);
    auto hit=fault_pool->fetch_page({0,1}); check(hit.value.has_value() && reads==4);
    fail.reset(); auto b=fault_pool->fetch_page({0,2}); check(b.value.has_value());
    check(fault_pool->stats().fetch_count==6 && fault_pool->stats().miss_count==5);
}
static_assert(!std::is_copy_constructible_v<PageGuard> && !std::is_copy_assignable_v<PageGuard>);
static_assert(std::is_nothrow_move_constructible_v<PageGuard> && std::is_nothrow_move_assignable_v<PageGuard>);
static_assert(std::is_same_v<decltype(std::declval<PageGuard>().page()),const RawPage&>);
}
int main() {
    try { frame_contract(); guard_and_cache(); failures(); }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
    return 0;
}
