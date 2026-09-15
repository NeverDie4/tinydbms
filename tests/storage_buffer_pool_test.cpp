#include "storage_test_access.h"
#include "record_page.h"
#include "tinydbms/storage.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <source_location>
#include <stdexcept>
#include <thread>

namespace {
using namespace tinydbms;
using namespace tinydbms::storage;
using namespace tinydbms::storage::internal;
void check(bool ok,std::source_location at=std::source_location::current()) {
    if(!ok) throw std::runtime_error("Storage BufferPool check at line "+std::to_string(at.line()));
}
struct Directory {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("tinydbms-storage-buffer-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Directory() { check(std::filesystem::create_directory(path)); }
    ~Directory() { (void)close_storage({}); (void)StorageTestAccess::configure();
        std::error_code ec; std::filesystem::remove_all(path,ec); }
};

struct Gate {
    void enter_and_wait() {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
    }
    void wait_until_entered() {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return entered; });
    }
    bool wait_until_entered_for(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return entered; });
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
};

void catalog_dirty_victim_cancel_then_marker_flush_survives_restart() {
    Directory directory;
    Gate write_gate;
    std::atomic<bool> gate_enabled = false, gate_used = false;
    check(StorageTestAccess::configure(1, ReplacementPolicy::kFifo,
        [&](PageKey key, const RawPage& page) -> std::optional<PageFileError> {
            if (gate_enabled.load(std::memory_order_acquire) && key == PageKey{0, 1} &&
                !gate_used.exchange(true, std::memory_order_acq_rel)) {
                write_gate.enter_and_wait();
            }
            auto* files = StorageTestAccess::file_manager();
            return files->find_table_file(key.table_id)->write_page(key.page_id, page);
        }));
    check(!open_storage({directory.path.string()}).error);
    auto* pool = StorageTestAccess::buffer_pool();
    auto* files = StorageTestAccess::file_manager();
    check(pool && files);
    auto* system_tables = files->find_table_file(0);
    check(system_tables);

    const auto old_page = system_tables->allocate_page();
    check(old_page.value.has_value());
    auto original = pool->fetch_page({0, *old_page.value}); check(original.value.has_value());
    const RawPage original_page = original.value->page();
    original.value->mutable_page().bytes[0] = std::byte{0};
    original.value->mark_dirty();
    original.value->release();
    const auto new_page = system_tables->allocate_page();
    check(new_page.value.has_value());

    gate_enabled.store(true, std::memory_order_release);
    BufferPoolResult<PageGuard> replacement;
    std::thread creator([&] { replacement = pool->fetch_page({0, *new_page.value}); });
    const bool write_started = write_gate.wait_until_entered_for(std::chrono::seconds(2));
    if (!write_started) {
        write_gate.release();
        creator.join();
        check(false);
    }
    std::atomic<bool> restored = false;
    std::thread restorer([&] {
        auto old = pool->fetch_page({0, *old_page.value});
        if (!old.value) return;
        old.value->mutable_page() = original_page;
        old.value->mark_dirty();
        old.value->release();
        restored.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!restored.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool restored_before_write_release = restored.load(std::memory_order_acquire);
    write_gate.release();
    creator.join();
    restorer.join();

    check(restored_before_write_release);
    check(!replacement.value && replacement.error && replacement.error->kind == BufferPoolErrorKind::kNoVictim);
    check(!pool->flush_page({0, *old_page.value}));
    check(!pool->release_table(0));
    check(!system_tables->free_page(*new_page.value));
    check(!system_tables->free_page(*old_page.value));
    check(!close_storage({}).error);

    check(StorageTestAccess::configure(1));
    check(!open_storage({directory.path.string()}).error);
    check(!create_table({2, "catalog_conc6", {{"id", Type::kInt}}}).error);
    check(!close_storage({}).error);
    check(!open_storage({directory.path.string()}).error);
    const auto tables = list_tables({});
    check(!tables.error && tables.tables.size() == 3);
    check(!close_storage({}).error);
}

void run() {
    Directory d;
    check(StorageTestAccess::configure(0));
    check(open_storage({d.path.string()}).error.has_value());
    check(!StorageTestAccess::buffer_pool() && !StorageTestAccess::file_manager());
    check(list_tables({}).error.has_value());
    check(StorageTestAccess::configure(2,ReplacementPolicy::kLru));
    check(!open_storage({d.path.string()}).error); // Same directory must remain retryable.
    check(!close_storage({}).error);
    Directory actual;
    bool fail=false, observed_closing=false;
    FileManager* files=nullptr;
    check(StorageTestAccess::configure(2,ReplacementPolicy::kLru,
        [&](PageKey key,const RawPage& page)->std::optional<PageFileError> {
            if (list_tables({}).error.has_value()) {
                observed_closing=true;
                check(create_table({9,"blocked",{{"id",Type::kInt}}}).error.has_value());
            }
            if(fail) return PageFileError{PageFileErrorKind::kIo,"injected flush failure"};
            auto* current_files=files ? files : StorageTestAccess::file_manager();
            return current_files->find_table_file(key.table_id)->write_page(key.page_id,page);
        }));
    check(!open_storage({actual.path.string()}).error);
    check(!create_table({2,"zero",{{"id",Type::kInt},{"name",Type::kVarchar}}}).error);
    files=StorageTestAccess::file_manager(); auto* pool=StorageTestAccess::buffer_pool();
    check(files && pool); check(files->find_table_file(2)->allocate_page().value==1);
    auto fetched=pool->fetch_page({2,1}); check(fetched.value.has_value());
    auto& guard=*fetched.value;
    check(!RecordPage::initialize(guard.mutable_page(),1));
    TableMeta meta{2,"zero",{{"id",Type::kInt},{"name",Type::kVarchar}}};
    const std::vector<Value> values{Value{std::int32_t{7}},Value{std::string{"persist"}}};
    auto record=RecordPage::insert_record(guard.mutable_page(),1,meta,values); check(record.value.has_value());
    guard.mark_dirty();
    const auto flushes_before_close=pool->stats().dirty_flush_count;
    check(close_storage({}).error.has_value()); // Active pin: recover Open, no flush.
    check(!list_tables({}).error && files->find_table_file(2)->is_open() && !observed_closing);
    guard.release(); fail=true;
    auto failed=close_storage({}); check(failed.error && failed.error->kind==StorageErrorKind::kIoError);
    check(observed_closing && !list_tables({}).error && files->find_table_file(2)->is_open());
    check(StorageTestAccess::buffer_pool()==pool && pool->stats().dirty_flush_count==flushes_before_close);
    fail=false; check(!close_storage({}).error);
    check(!StorageTestAccess::buffer_pool() && !StorageTestAccess::file_manager());
    check(StorageTestAccess::configure(2));
    check(!open_storage({actual.path.string()}).error);
    auto restored=StorageTestAccess::buffer_pool()->fetch_page({2,1}); check(restored.value.has_value());
    auto row=RecordPage::get_record(restored.value->page(),1,meta,*record.value);
    check(row.value && row.value->rid.value==record.value->value);
    check(row.value->values[0].data==values[0].data && row.value->values[1].data==values[1].data);
    restored.value->release();
    StorageTestAccess::fail_next_file_close();
    check(close_storage({}).error.has_value());
    check(list_tables({}).error.has_value() && open_storage({actual.path.string()}).error.has_value());
    check(StorageTestAccess::file_manager()->find_table_file(2)!=nullptr);
    check(!close_storage({}).error); // Retry finishing cleanup.
    check(!open_storage({actual.path.string()}).error);
    // Metadata close failure also stays Closing after files are gone.
    auto tmp=actual.path/"storage.meta.tmp";
    check(std::filesystem::create_directory(tmp));
    { std::ofstream blocker(tmp/"block"); blocker<<"x"; }
    check(close_storage({}).error.has_value()); check(list_tables({}).error.has_value());
    std::filesystem::remove(tmp/"block"); std::filesystem::remove(tmp);
    check(!close_storage({}).error && !close_storage({}).error);
    catalog_dirty_victim_cancel_then_marker_flush_survives_restart();
}
}
int main() { try { run(); } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } return 0; }
