#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "../mods/friend_list_fix/sql_policy.h"
int wmain(int argc,wchar_t** argv) {
    if(argc!=3&&argc!=4)return 2;
    try {
        std::ifstream input(std::filesystem::path(argv[1]),std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)),{});
        if(bytes.empty())return 2;
        const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);
        if(!count)return 2;
        std::wstring sql(count,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),sql.data(),count);
        const auto clone=sdgo::friend_fix::Clone(sql,L"sp_FriendsList",false,argc==4?argv[3]:L"DATABASE_DEFAULT");
        const auto size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,clone.data(),static_cast<int>(clone.size()),nullptr,0,nullptr,nullptr);
        if(!size)return 2;
        std::string out(size,'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,clone.data(),static_cast<int>(clone.size()),out.data(),size,nullptr,nullptr);
        std::ofstream output(std::filesystem::path(argv[2]),std::ios::binary);output<<out;
        return output?0:1;
    }catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}
}
