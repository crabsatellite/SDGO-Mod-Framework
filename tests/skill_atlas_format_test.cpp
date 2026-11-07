#include "sdgo/skill_atlas_format.h"
#include "../support/native_resource_overlay/native_resource_overlay.h"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace {
std::vector<std::uint8_t> Read(const char* path){
    std::ifstream stream(path,std::ios::binary);if(!stream)throw std::runtime_error("atlas test input missing");
    return {std::istreambuf_iterator<char>(stream),{}};
}
std::wstring Wide(const char* text){
    const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,nullptr,0);if(!size)throw std::runtime_error("atlas test path encoding");
    std::wstring result(static_cast<std::size_t>(size),L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,result.data(),size);result.pop_back();return result;
}
int failures=0;
void Check(bool valid,const char* message){std::printf("%s %s\n",valid?"PASS":"FAIL",message);if(!valid)++failures;}
}
int main(int argc,char** argv){
    try{
        if(argc!=4)return 2;
        auto bytes=Read(argv[1]),catalogBytes=Read(argv[2]);
        sdgo::unit_catalog::Catalog catalog;std::string error;
        if(!catalog.Load(Wide(argv[2]),error))throw std::runtime_error(error);
        sdgo::resource_overlay::BundleRegistration resources;
        if(!sdgo::resource_overlay::InspectUnitBundleFiles(Wide(argv[3]).c_str(),resources,error))throw std::runtime_error(error);
        std::set<std::wstring> paths;for(const auto& entry:resources.entries)paths.insert(entry.first);
        const auto hash=sdgo::scripts::Sha256(catalogBytes);sdgo::skill_atlas::Bundle atlas;
        Check(atlas.Parse(bytes,hash,resources.identity,paths,catalog,error),error.empty()?"native parser accepts exact catalog/archive-bound atlas":error.c_str());
        Check(!atlas.entries.empty(),"native atlas retains actual typed skill records");
        const auto id=atlas.entries.begin()->first;
        Check(!atlas.Parse(bytes,std::string(64,'0'),resources.identity,paths,catalog,error)&&atlas.entries.empty(),"changed catalog hash refuses and clears stale atlas entries");
        Check(!atlas.Parse(bytes,hash,std::string(64,'0')+":"+std::string(64,'0'),paths,catalog,error),"changed native archive hashes refuse");
        Check(!atlas.Parse(bytes,hash,resources.identity,{},catalog,error),"sprite outside the sealed unit archive refuses");
        const auto index=catalog.Find(id);catalog.records[index].bytes[5]^=1;
        Check(!atlas.Parse(bytes,hash,resources.identity,paths,catalog,error),"changed complete skill record refuses");catalog.records[index].bytes[5]^=1;
        bytes.push_back(0);Check(!atlas.Parse(bytes,hash,resources.identity,paths,catalog,error),"trailing sidecar bytes refuse");
    }catch(const std::exception& error){std::printf("FAIL %s\n",error.what());return 1;}
    return failures?1:0;
}
