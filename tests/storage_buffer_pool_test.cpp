#include "storage_test_access.h"
#include "record_page.h"
#include "tinydbms/storage.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>

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
            observed_closing=list_tables({}).error.has_value();
            check(create_table({9,"blocked",{{"id",Type::kInt}}}).error.has_value());
            if(fail) return PageFileError{PageFileErrorKind::kIo,"injected flush failure"};
            return files->find_table_file(key.table_id)->write_page(key.page_id,page);
        }));
    check(!open_storage({actual.path.string()}).error);
    check(!create_table({0,"zero",{{"id",Type::kInt},{"name",Type::kVarchar}}}).error);
    files=StorageTestAccess::file_manager(); auto* pool=StorageTestAccess::buffer_pool();
    check(files && pool); check(files->find_table_file(0)->allocate_page().value==1);
    auto fetched=pool->fetch_page({0,1}); check(fetched.value.has_value());
    auto& guard=*fetched.value;
    check(!RecordPage::initialize(guard.mutable_page(),1));
    TableMeta meta{0,"zero",{{"id",Type::kInt},{"name",Type::kVarchar}}};
    const std::vector<Value> values{Value{std::int32_t{7}},Value{std::string{"persist"}}};
    auto record=RecordPage::insert_record(guard.mutable_page(),1,meta,values); check(record.value.has_value());
    guard.mark_dirty();
    check(close_storage({}).error.has_value()); // Active pin: recover Open, no flush.
    check(!list_tables({}).error && files->find_table_file(0)->is_open() && !observed_closing);
    guard.release(); fail=true;
    auto failed=close_storage({}); check(failed.error && failed.error->kind==StorageErrorKind::kIoError);
    check(observed_closing && !list_tables({}).error && files->find_table_file(0)->is_open());
    check(StorageTestAccess::buffer_pool()==pool && pool->stats().dirty_flush_count==0);
    fail=false; check(!close_storage({}).error);
    check(!StorageTestAccess::buffer_pool() && !StorageTestAccess::file_manager());
    check(StorageTestAccess::configure(2));
    check(!open_storage({actual.path.string()}).error);
    auto restored=StorageTestAccess::buffer_pool()->fetch_page({0,1}); check(restored.value.has_value());
    auto row=RecordPage::get_record(restored.value->page(),1,meta,*record.value);
    check(row.value && row.value->rid.value==record.value->value);
    check(row.value->values[0].data==values[0].data && row.value->values[1].data==values[1].data);
    restored.value->release();
    StorageTestAccess::fail_next_file_close();
    check(close_storage({}).error.has_value());
    check(list_tables({}).error.has_value() && open_storage({actual.path.string()}).error.has_value());
    check(StorageTestAccess::file_manager()->find_table_file(0)!=nullptr);
    check(!close_storage({}).error); // Retry finishing cleanup.
    check(!open_storage({actual.path.string()}).error);
    // Metadata close failure also stays Closing after files are gone.
    auto tmp=actual.path/"storage.meta.tmp";
    check(std::filesystem::create_directory(tmp));
    { std::ofstream blocker(tmp/"block"); blocker<<"x"; }
    check(close_storage({}).error.has_value()); check(list_tables({}).error.has_value());
    std::filesystem::remove(tmp/"block"); std::filesystem::remove(tmp);
    check(!close_storage({}).error && !close_storage({}).error);
}
}
int main() { try { run(); } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; } return 0; }
