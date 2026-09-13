#include "tinydbms/storage.hpp"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <source_location>
using namespace tinydbms;
using namespace tinydbms::storage;
void check(bool ok,std::source_location at=std::source_location::current()){
    if(!ok)throw std::runtime_error("metadata audit assertion line "+std::to_string(at.line()));}
struct Temp{std::filesystem::path path=std::filesystem::temp_directory_path()/
    ("tinydbms-v1-meta-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp(){
        check(std::filesystem::create_directory(path));
        std::ofstream metadata(path / "storage.meta");
        metadata << "TINYDBMS_STORAGE_V1\nEND\n";
        check(static_cast<bool>(metadata));
    }
    ~Temp(){std::error_code e;std::filesystem::remove_all(path,e);}};
int main()try{
    const std::string valid="TABLE 0 first\nCOLUMN INT32 id\nENDTABLE\n";
    for(const auto& content:std::vector<std::string>{
        valid+"TABLE 1 first\nCOLUMN INT32 id\nENDTABLE\nEND\n",
        valid+valid+"END\n",
        "TABLE 0 bad-name\nCOLUMN INT32 id\nENDTABLE\nEND\n",
        "TABLE 0 first\nCOLUMN INT32 id\nCOLUMN INT32 id\nENDTABLE\nEND\n",
        "TABLE 0 first extra\nCOLUMN INT32 id\nENDTABLE\nEND\n",
        valid+"END\nTRAILING\n",
        "TABLE 0 first\nCOLUMN INT32 id extra\nENDTABLE\nEND\n"}){
        Temp temp;check(!open_storage({temp.path.string()}).error);
        check(!create_table({0,"first",{{"id",Type::kInt}}}).error);check(!create_table({1,"second",{{"id",Type::kInt}}}).error);check(!close_storage({}).error);
        {std::ofstream out(temp.path/"storage.meta");out<<"TINYDBMS_STORAGE_V1\n"<<content;out.close();check(!out.fail());}
        auto result=open_storage({temp.path.string()});
        if(!result.error){(void)close_storage({});throw std::runtime_error("accepted corrupt metadata: "+content);}
        check(result.error->kind==StorageErrorKind::kCorrupt);check(!close_storage({}).error);
    }
    {
        Temp temp;
        check(!open_storage({temp.path.string()}).error);
        check(!create_table({0,"legacy",{{"id",Type::kInt},{"name",Type::kVarchar}}}).error);
        check(!close_storage({}).error);
        {
            std::ifstream in(temp.path/"storage.meta");std::string magic;
            check(static_cast<bool>(std::getline(in,magic)));check(magic=="TINYDBMS_STORAGE_V2");
        }
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
        check(!close_storage({}).error);
        {
            std::ifstream in(temp.path/"storage.meta");std::string magic;
            check(static_cast<bool>(std::getline(in,magic)));check(magic=="TINYDBMS_STORAGE_V1");
        }
        check(!open_storage({temp.path.string()}).error);
        check(!create_table({1,"events",{{"event_id",Type::kBigInt}}}).error);
        check(!close_storage({}).error);
        std::ifstream in(temp.path/"storage.meta");
        const std::string metadata((std::istreambuf_iterator<char>(in)),{});
        in.close();check(!in.fail());
        check(metadata.find("TINYDBMS_STORAGE_V2\n")==0);
        check(metadata.find("TABLE 0 legacy V1\n")!=std::string::npos);
        check(metadata.find("COLUMN INT32 NOT_NULL id\n")!=std::string::npos);
        check(metadata.find("TABLE 1 events V2\n")!=std::string::npos);
        check(metadata.find("COLUMN INT64 NOT_NULL event_id\n")!=std::string::npos);
        check(!open_storage({temp.path.string()}).error);
        const auto tables=list_tables({});check(!tables.error && tables.tables.size()==2);
        check(tables.tables[0].columns[0].type==Type::kInt);
        check(tables.tables[1].columns[0].type==Type::kBigInt);
        check(!close_storage({}).error);
    }
    std::cout<<"metadata integrity regressions passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
