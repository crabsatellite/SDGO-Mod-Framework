#include "sdgo/unit_form_graph_format.h"
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    const bool batch=argc>=5&&!std::strcmp(argv[1],"--batch");
    if ((!batch&&argc!=3)||(batch&&((argc-2)%3||(argc-2)/3>64)))return 2;
    auto read = [](const char* path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) throw std::runtime_error("unit form file unavailable");
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), {});
    };
    try {
        sdgo::unit_forms::GraphRuntimeInput input;std::string error;
        const int start=batch?2:1,step=batch?3:argc;
        for(int at=start;at<argc;at+=step){
            const auto candidate=read(argv[at]),authority=read(argv[at+1]);
            const bool parsed=input.Parse(candidate,authority,error);
            if(batch){
                if(std::strcmp(argv[at+2],"0")&&std::strcmp(argv[at+2],"1"))return 2;
                const bool want=argv[at+2][0]=='0';
                if(parsed!=want){std::cerr<<"batch case "<<(at-2)/3<<" differs: "<<error<<'\n';return 1;}
                if(!parsed&&(!input.nativeDefinitions.empty()||!input.candidates.empty()||input.bindings||
                    !input.candidateSha256.empty()||!input.serverFingerprint.empty())){
                    std::cerr<<"failed parse retained runtime state\n";return 1;}
            }else if(!parsed){std::cerr<<error<<'\n';return 1;}
            if(parsed)std::cout << "unit form candidate/independent authority parsed: " << input.candidates.size()
                << " owners; " << input.nativeDefinitions.size() << " native expectations; no native activation\n";
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n';return 1; }
}
