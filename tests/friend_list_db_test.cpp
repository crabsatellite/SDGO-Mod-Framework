#include <windows.h>
#include <msdasc.h>
#include <cstdio>
#include "../mods/friend_list_fix/policy.h"
using namespace sdgo::friend_fix;
static std::wstring Read(const wchar_t* path,const wchar_t* key) {
    wchar_t text[2048]{};const auto n=GetPrivateProfileStringW(L"fixture",key,L"",text,2048,path);
    if(!n||n>=2047)throw std::runtime_error("fixture setting missing");return text;
}
static std::wstring Quote(const std::wstring& s) {
    std::wstring out=L"\"";for(auto c:s){out+=c;if(c==L'"')out+=c;}return out+L"\"";
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2||FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 2;
    try {
        Com<IDataInitialize> init;Com<IDBInitialize> source;Com<IDBCreateSession> maker;Com<IDBCreateCommand> factory;
        const auto database=Read(argv[1],L"database");
        const auto connection=L"Provider=SQLOLEDB;OLE DB Services=-2;Data Source="+Quote(Read(argv[1],L"server"))+
            L";Initial Catalog="+Quote(database)+L";User ID=sa;Password="+Quote(Read(argv[1],L"password"))+L";";
        Need(SUCCEEDED(CoCreateInstance(CLSID_MSDAINITIALIZE,nullptr,CLSCTX_INPROC_SERVER,__uuidof(IDataInitialize),reinterpret_cast<void**>(init.put()))),"fixture IDataInitialize unavailable");
        const auto sourceResult=init->GetDataSource(nullptr,CLSCTX_INPROC_SERVER,connection.c_str(),__uuidof(IDBInitialize),reinterpret_cast<IUnknown**>(source.put()));
        if(FAILED(sourceResult)){fprintf(stderr,"fixture provider HRESULT=%08lX\n",static_cast<unsigned long>(sourceResult));throw std::runtime_error("fixture data source creation failed");}
        Need(SUCCEEDED(source->Initialize()),"fixture data source authentication failed");
        Need(SUCCEEDED(source->QueryInterface(__uuidof(IDBCreateSession),reinterpret_cast<void**>(maker.put()))),"fixture session factory unavailable");
        Need(SUCCEEDED(maker->CreateSession(nullptr,__uuidof(IDBCreateCommand),reinterpret_cast<IUnknown**>(factory.put()))),"fixture command factory creation failed");
        Com<ICommandText> native;Need(SUCCEEDED(factory->CreateCommand(nullptr,__uuidof(ICommandText),reinterpret_cast<IUnknown**>(native.put()))),"fixture native command creation failed");
        struct Connection { BYTE pad[8]{};ICommandText* command;BYTE rest[0x240-12]{};int state=0; } lease{};
        static_assert(offsetof(Connection,command)==8&&offsetof(Connection,state)==0x240);
        lease.command=native.get();Session db;Need(db.Open(&lease),"fixture borrowed command session unavailable");
        puts("fixture: borrowed session open");fflush(stdout);
        std::wstring before,after;
        Need(db.String(L"SELECT OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_FriendsList'))",before));
        puts("fixture: native definition read");fflush(stdout);
        Need(!Prepare(db,L"wrong_database"));
        Need(db.Begin());bool refused=false;
        try{Prepare(db,database);}catch(...){refused=true;}
        Need(refused,"joined caller transaction");db.Abort();
        puts("fixture: caller transaction refusal passed");fflush(stdout);
        Need(Prepare(db,database));Need(Prepare(db,database));
        puts("fixture: policy initialization passed twice");fflush(stdout);
        Need(db.String(L"SELECT OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_FriendsList'))",after));
        Need(before==after,"native SQL definition changed");
        puts("fixture: native definition preserved");fflush(stdout);
        Need(db.Begin());
        Need(db.Exec(L"ALTER PROCEDURE [sdgo_friend_list_fix].[sp_FriendsList] AS SELECT 1"));
        // Commit is deliberate inside this disposable database: initialization
        // must detect an external edit on the next authenticated session.
        Need(db.Commit());refused=false;
        try{Prepare(db,database);}catch(...){refused=true;}
        Need(refused,"external clone edit was overwritten");
        // Restore the fixture's owned procedure and its original creation options.
        std::wstring flags;
        Need(db.String(L"SELECT CONVERT(nvarchar(10),CONVERT(int,uses_ansi_nulls)+2*CONVERT(int,uses_quoted_identifier)) FROM sys.sql_modules WHERE object_id=OBJECT_ID(N'dbo.sp_FriendsList')",flags));
        const int options=std::stoi(flags);
        Need(db.Exec(std::wstring(L"SET ANSI_NULLS ")+(options&1?L"ON":L"OFF")));
        Need(db.Exec(std::wstring(L"SET QUOTED_IDENTIFIER ")+(options&2?L"ON":L"OFF")));
        Need(db.Exec(Clone(before,L"sp_FriendsList",true)));
        Need(Prepare(db,database));
        puts("PASS actual SQLOLEDB borrowed session, ownership, retry, source preservation and external-edit refusal");
        return 0;
    }catch(const std::exception& e){fprintf(stderr,"friend DB fixture: %s\n",e.what());return 1;}
}
