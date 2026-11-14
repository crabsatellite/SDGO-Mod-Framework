#include "sdgo/shop_profile.h"
#include <filesystem>
#include <fstream>
#include <iostream>

int wmain(int argc,wchar_t** argv)
{
    if(argc!=2)return 2;
    const std::filesystem::path root=argv[1];std::filesystem::create_directories(root);
    const auto file=root/L"profile.ini";
    auto write=[&](const std::wstring& text){std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(text.data()),static_cast<std::streamsize>(text.size()*2));};
    auto refuses=[&](){try{sdgo::ShopProfile profile(file.c_str());}catch(const std::exception&){return;}throw std::runtime_error("malformed profile accepted");};
    try {
        write(L"\xFEFF; generic profile\r\n[shop]\r\nRevision=fixture\r\n[option:42]\r\nCategory=1120001\r\nText=\"中文=永久\"\r\n");
        sdgo::ShopProfile profile(file.c_str());
        if(profile.Value(L"OPTION:42",L"text")!=L"中文=永久"||profile.Number(L"option:42",L"category")!=1120001)
            throw std::runtime_error("Unicode, equals, quoted text or case-insensitive fields changed");
        write(L"\xFEFF[shop]\nSchema=1\nschema=2\n");refuses();
        write(L"\xFEFF[shop]\nSchema=1\n[SHOP]\nSchema=1\n");refuses();
        write(std::wstring(L"\xFEFF[shop]\nRevision=")+std::wstring(1,L'\0')+L"hidden\n");refuses();
        {std::ofstream out(file,std::ios::binary);const unsigned char odd[]={0xFF,0xFE,'['};out.write(reinterpret_cast<const char*>(odd),3);}refuses();
        {std::ofstream out(file,std::ios::binary);out<<"[shop]\nRevision=ascii\n";}
        if(sdgo::ShopProfile(file.c_str()).Value(L"shop",L"Revision")!=L"ascii")throw std::runtime_error("ASCII compatibility changed");
        std::filesystem::resize_file(file,sdgo::kShopProfileMaximumBytes+1);refuses();
        std::filesystem::remove(file);std::error_code ignored;std::filesystem::remove(root,ignored);
        std::cout<<"SHOP PROFILE PASS: Unicode and legacy text, duplicate refusal, truncated input and byte budget\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
