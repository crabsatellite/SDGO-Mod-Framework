#include "ai_test_support.h"
#include "../mods/friend_list_fix/friend_list_fix.cpp"
#include <thread>

namespace {
int attempts=0,nativeCalls=0;
bool available=false;
void* captured[4]{};
int capturedCapacity=0;
bool PrepareFixture(void*){++attempts;return available;}
int __fastcall Native(void* self,void*,void* connection,void* args,void* rows,int capacity,int* count) {
    ++nativeCalls;captured[0]=self;captured[1]=connection;captured[2]=args;captured[3]=rows;
    capturedCapacity=capacity;*count=3;SetLastError(77);return -23;
}
void* Symbol(SdgoMod*,const char* name) {
    if(!strcmp(name,"friends.command"))return const_cast<wchar_t*>(L"{call dbo.sp_FriendsList(?,?,?,?,?)}");
    return reinterpret_cast<void*>(1);
}
int Hook(SdgoMod*,const char* name,void*,void** original) {
    CHECK(!strcmp(name,"friends.list"));*original=reinterpret_cast<void*>(&Native);return 1;
}
int Slot(SdgoMod*,const char* name,void* replacementText,void** original) {
    CHECK(!strcmp(name,"friends.command_slot"));
    CHECK(!wcscmp(static_cast<const wchar_t*>(replacementText),L"{call [sdgo_friend_list_fix].[sp_FriendsList](?,?,?,?,?)}"));
    *original=const_cast<wchar_t*>(L"{call dbo.sp_FriendsList(?,?,?,?,?)}");return 1;
}
}
int main() {
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);std::wstring ini(path);
    ini.replace(ini.find_last_of(L'.'),std::wstring::npos,L".ini");
    CHECK(WritePrivateProfileStringW(L"DB Member",L"DBName",L"friend_fixture",ini.c_str())!=0);
    auto api=ai_test::api;api.Symbol=Symbol;api.HookCode=Hook;api.HookSlot=Slot;
    CHECK(SdgoModLoad(&api,reinterpret_cast<SdgoMod*>(1))==1);
    CHECK(database==L"friend_fixture");preparePolicy=PrepareFixture;
    int count=8;SetLastError(42);
    SCENARIO("failed initialization cannot publish an empty successful list");
    CHECK(List(nullptr,nullptr,reinterpret_cast<void*>(2),nullptr,nullptr,8,&count)==1);
    CHECK(count==0&&nativeCalls==0&&attempts==1&&GetLastError()==42);
    available=true;
    SCENARIO("retry preserves the five-stack-argument native ABI and native result");
    void* self=reinterpret_cast<void*>(1);void* connection=reinterpret_cast<void*>(2);
    void* args=reinterpret_cast<void*>(3);void* rows=reinterpret_cast<void*>(4);
    CHECK(List(self,nullptr,connection,args,rows,8,&count)==-23);
    CHECK(count==3&&attempts==2&&nativeCalls==1&&capturedCapacity==8&&GetLastError()==77);
    CHECK(captured[0]==self&&captured[1]==connection&&captured[2]==args&&captured[3]==rows);
    CHECK(List(self,nullptr,connection,args,rows,8,&count)==-23);
    CHECK(attempts==2&&nativeCalls==2);
    CHECK(DeleteFileW(ini.c_str())!=0);
    SCENARIO("SQL parser respects quoted content and nested comments");
    using namespace sdgo::friend_fix;
    const auto values=Literals(L"-- 'ignored'\n/* outer /* 'ignored' */ */ SELECT N'a''b',[x'y],\"z'q\",'c';");
    CHECK(values.size()==2&&values[0].value==L"a'b"&&values[1].value==L"c");
    bool refused=false;try{Clone(L"CREATE PROC sp_FriendsList AS SELECT 1",L"sp_FriendsList",false);}catch(...){refused=true;}
    CHECK(refused);
    SCENARIO("canonical Unicode repairs only the packed native name field");
    std::array<unsigned char,kFriendRowBytes*2> packed{};
    packed.fill(0xA5);
    const std::uint32_t chineseId=1001,asciiId=1002;
    memcpy(packed.data(),&chineseId,4);
    memcpy(packed.data()+4,"????????",9);
    memcpy(packed.data()+kFriendRowBytes,&asciiId,4);
    memcpy(packed.data()+kFriendRowBytes+4,"PilotTwo",9);
    const auto untouched=packed;
    unsigned fetches=0;
    CHECK(RepairFriendNames(packed.data(),2,936,[&](std::uint32_t id,std::wstring& value) {
        ++fetches;CHECK(id==chineseId);value=L"\u4e2d\u6587\u597d\u53cb";return true;
    })==1);
    CHECK(fetches==1);
    wchar_t decoded[20]{};
    CHECK(MultiByteToWideChar(936,MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(packed.data()+4),-1,decoded,20)>0);
    CHECK(!wcscmp(decoded,L"\u4e2d\u6587\u597d\u53cb"));
    for(size_t i=0;i<packed.size();++i)
        if(i<4||i>=4+kFriendNameBytes)CHECK(packed[i]==untouched[i]);
    SCENARIO("native name limit keeps complete multibyte characters and failed lookup retains data");
    std::array<char,kFriendNameBytes> encoded{};
    CHECK(EncodeFriendName(std::wstring(12,L'\u4e2d'),936,encoded));
    CHECK(strlen(encoded.data())==16);
    CHECK(MultiByteToWideChar(936,MB_ERR_INVALID_CHARS,encoded.data(),-1,decoded,20)==9);
    CHECK(!EncodeFriendName(std::wstring(L"x\0y",3),936,encoded));
    auto missing=untouched;
    CHECK(RepairFriendNames(missing.data(),1,936,[](std::uint32_t,std::wstring&){return false;})==0);
    CHECK(missing==untouched);
    return ai_test::Finish();
}
