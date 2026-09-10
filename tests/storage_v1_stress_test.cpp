#include "tinydbms/storage.hpp"
#include "storage_test_access.h"
#include "record_page.h"
#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <source_location>
#include <stdexcept>
namespace tinydbms::storage::internal {
struct BufferPoolTestAccess {
    static bool no_pins(const BufferPool& p){for(const auto& f:p.frames_)if(f.pin_count)return false;return true;}
};
}
namespace {
using namespace tinydbms;
using namespace tinydbms::storage;
using namespace tinydbms::storage::internal;
void check(bool ok,std::source_location at=std::source_location::current()){
    if(!ok)throw std::runtime_error("V1 stress line "+std::to_string(at.line()));
}
struct Temp {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("tinydbms-v1-stress-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp(){check(std::filesystem::create_directory(path));}
    ~Temp(){std::error_code e;std::filesystem::remove_all(path,e);}
};
using Rows=std::map<std::uint64_t,std::vector<Value>>;
std::vector<Value> row(int id,TableId table){return {{std::int32_t{id}},{std::int32_t{id*100000}},
    {std::string(16,table?'b':'a')}};}
void metrics(){auto* p=StorageTestAccess::buffer_pool();auto s=p->stats();
    check(s.fetch_count==s.hit_count+s.miss_count);check(s.hit_rate()>=0 && s.hit_rate()<=1);
    check(BufferPoolTestAccess::no_pins(*p));}
void verify(TableId table,const Rows& expected){
    auto c=open_table({table});check(c.cursor && !c.error);std::set<std::uint64_t> seen;
    for(;;){auto r=scan_next({*c.cursor});check(!r.error);metrics();if(!r.record)break;
        auto found=expected.find(r.record->rid.value);check(found!=expected.end());
        check(seen.insert(found->first).second);check(found->second.size()==r.record->values.size());
        for(std::size_t i=0;i<found->second.size();++i)check(found->second[i].data==r.record->values[i].data);}
    check(seen.size()==expected.size());check(!close_cursor({*c.cursor}).error);
}
void run(std::size_t capacity,ReplacementPolicy policy,int count){
    const auto started=std::chrono::steady_clock::now();Temp temp;
    check(StorageTestAccess::configure(capacity,policy));check(!open_storage({temp.path.string()}).error);
    Rows expected[2];std::uint64_t pages[2]{};
    for(TableId t=0;t<2;++t){
        check(!create_table({t,t?"second":"first",{{"i",Type::kInt},{"l",Type::kInt},
            {"s",Type::kVarchar}}}).error);
        std::vector<std::vector<Value>> values;for(int i=0;i<count;++i)values.push_back(row(i,t));
        auto inserted=insert({t,values});check(!inserted.error && inserted.rids.size()==values.size());
        for(std::size_t i=0;i<values.size();++i)expected[t].emplace(inserted.rids[i].value,values[i]);
        pages[t]=StorageTestAccess::file_manager()->find_table_file(t)->page_count();check(pages[t]>3);verify(t,expected[t]);
    }
    check(expected[0].begin()->first==expected[1].begin()->first); // Same physical RID, distinct scope.
    std::size_t reused=0;
    for(TableId t=0;t<2;++t){
        std::vector<RecordId> removed;std::map<std::pair<PageId,SlotId>,std::uint16_t> generations;
        int ordinal=0;for(const auto& [id,values]:expected[t])if(ordinal++%2==0){
            removed.push_back({id});auto p=RecordIdCodec::decode({id});check(p.value.has_value());
            generations[{p.value->page_id,p.value->slot_id}]=p.value->generation;}
        auto deleted=delete_records({t,removed});check(!deleted.error && deleted.deleted_count==removed.size());
        for(auto id:removed)expected[t].erase(id.value);verify(t,expected[t]);
        std::vector<std::vector<Value>> replacement;for(std::size_t i=0;i<removed.size();++i)replacement.push_back(row(count+static_cast<int>(i),t));
        auto inserted=insert({t,replacement});check(!inserted.error && inserted.rids.size()==replacement.size());
        for(std::size_t i=0;i<replacement.size();++i){auto id=inserted.rids[i];auto p=RecordIdCodec::decode(id);check(p.value.has_value());
            auto old=generations.find({p.value->page_id,p.value->slot_id});check(old!=generations.end());
            check(p.value->generation==old->second+1);++reused;expected[t].emplace(id.value,replacement[i]);}
        for(auto old:removed){auto failed=delete_records({t,{old}});check(failed.error && failed.error->kind==StorageErrorKind::kInvalidRequest && failed.deleted_count==0);}
        check(StorageTestAccess::file_manager()->find_table_file(t)->page_count()==pages[t]);verify(t,expected[t]);
    }
    metrics();auto stats=StorageTestAccess::buffer_pool()->stats();check(stats.eviction_count>0 && stats.dirty_flush_count>0);
    check(!close_storage({}).error);
    for(int cycle=0;cycle<3;++cycle){
        check(!open_storage({temp.path.string()}).error);
        for(TableId t=0;t<2;++t){verify(t,expected[t]);auto extra=insert({t,{row(20000+cycle,t)}});check(!extra.error && extra.rids.size()==1);
            expected[t].emplace(extra.rids[0].value,row(20000+cycle,t));
            auto old=expected[t].begin()->first;check(!delete_records({t,{{old}}}).error);expected[t].erase(old);verify(t,expected[t]);}
        metrics();check(!close_storage({}).error);
    }
    check(!open_storage({temp.path.string()}).error);for(TableId t=0;t<2;++t)verify(t,expected[t]);check(!close_storage({}).error);
    std::cout<<"capacity="<<capacity<<" policy="<<(policy==ReplacementPolicy::kFifo?"FIFO":"LRU")
        <<" initial_rows="<<2*count<<" reused="<<reused<<" pages="<<pages[0]-1<<"+"<<pages[1]-1
        <<" fetch="<<stats.fetch_count<<" hit="<<stats.hit_count<<" miss="<<stats.miss_count
        <<" eviction="<<stats.eviction_count<<" dirty_flush="<<stats.dirty_flush_count
        <<" elapsed_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count()<<'\n';
}
void policy_demo(ReplacementPolicy policy){
    Temp temp;auto opened=FileManager::open(temp.path);check(opened.value.has_value());auto& files=**opened.value;
    auto made=files.create_table_file(0);check(made.value.has_value());
    for(int i=0;i<3;++i)check((*made.value)->allocate_page().value.has_value());
    std::vector<std::string> events;
    auto pool=BufferPool::create(files,2,{},{},policy,[&](std::string_view event){events.emplace_back(event);});check(pool.value.has_value());
    for(PageId id:{1,2,1,3,1}){auto guard=(*pool.value)->fetch_page({0,id});check(guard.value.has_value());}
    auto stats=(*pool.value)->stats();bool fifo=policy==ReplacementPolicy::kFifo;
    check(stats.fetch_count==5 && stats.hit_count==(fifo?1U:2U) && stats.miss_count==(fifo?4U:3U));
    check(stats.eviction_count==(fifo?2U:1U) && stats.dirty_flush_count==0);
    check(stats.hit_rate()==(fifo?0.2:0.4));check(!events.empty());
    std::cout<<"demo "<<(fifo?"FIFO":"LRU")<<" sequence=1,2,1,3,1 hit="<<stats.hit_count<<" miss="<<stats.miss_count<<" rate="<<stats.hit_rate()<<'\n';
    check(!(*pool.value)->close());check(!files.close_all());
}
}
int main()try{for(auto policy:{ReplacementPolicy::kFifo,ReplacementPolicy::kLru}){
    for(std::size_t capacity:{1,2,3})run(capacity,policy,capacity==1 && policy==ReplacementPolicy::kFifo?5000:600);
    policy_demo(policy);}}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
