#include "sdgo/skill_identity_format.h"
#include <cstdio>
#include <fstream>
#include <iterator>

namespace {
std::vector<std::uint8_t> Read(const char* path){
    std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("identity test input missing");
    return {std::istreambuf_iterator<char>(file),{}};
}
int failures=0;
void Check(bool yes,const char* label){std::printf("%s %s\n",yes?"PASS":"FAIL",label);if(!yes)++failures;}
}
int main(int argc,char** argv){
    try{
        if(argc!=3)return 2;
        const auto candidate=Read(argv[1]),authority=Read(argv[2]);
        sdgo::skill_identity::RuntimeInput input;std::string error;
        Check(input.Parse(candidate,authority,error),error.empty()?"native parser accepts independently authorized allocation":error.c_str());
        Check(!input.candidates.empty()&&!input.serverFingerprint.empty(),"complete native allocation and server fingerprint retained");
        auto bad=candidate;bad.back()^=1;
        Check(!input.Parse(bad,authority,error)&&input.candidates.empty(),"changed content refuses and clears previous candidates");
        bad=authority;bad[28]^=1;
        Check(!input.Parse(candidate,bad,error),"independent candidate authorization hash is required");
        bad=authority;bad[92]^=1;
        Check(!input.Parse(candidate,bad,error),"authority for a different native server profile refuses");
        bad=authority;bad.back()^=1;
        Check(!input.Parse(candidate,bad,error),"installed allocation payload must authorize the exact source and all hashes");
        bad=authority;bad.push_back(0);
        Check(!input.Parse(candidate,bad,error),"authority extent and trailing data refuse");
    }catch(const std::exception& exception){std::printf("FAIL %s\n",exception.what());return 1;}
    return failures?1:0;
}
