#include "tinydbms/storage.hpp"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace tinydbms;
using namespace tinydbms::storage;
void check(bool ok){if(!ok)throw std::runtime_error("metadata audit assertion");}
struct Temp{std::filesystem::path path=std::filesystem::temp_directory_path()/
    ("tinydbms-v1-meta-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp(){check(std::filesystem::create_directory(path));}~Temp(){std::error_code e;std::filesystem::remove_all(path,e);}};
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
    std::cout<<"metadata integrity regressions passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
