#pragma once
#include "oledb_session.h"

namespace sdgo::friend_fix {
inline bool Prepare(Session& db,const std::wstring& expectedDatabase,const std::wstring& gameDatabase=L"") {
    std::wstring database;
    Need(db.String(L"SELECT CASE WHEN DB_ID("+Literal(expectedDatabase)+L")=DB_ID() THEN N'1' ELSE N'0' END",database),"friend database identity unavailable");
    if(database!=L"1")return false;
    std::wstring nameCollation;
    Need(db.String(L"SELECT ISNULL((SELECT collation_name FROM sys.columns WHERE object_id=OBJECT_ID(N'dbo.tbl_Account_Pilot',N'U') AND name=N'f_pilot' AND system_type_id=231 AND max_length=40),N'')",nameCollation),"native pilot-name collation unavailable");
    Need(!nameCollation.empty(),"native pilot-name column layout differs");
    std::wstring signature;
    Need(db.String(L"SELECT CASE WHEN COUNT(*)=5 AND SUM(CASE WHEN system_type_id=56 AND max_length=4 AND is_output=CASE WHEN parameter_id<=3 THEN 0 ELSE 1 END THEN 1 ELSE 0 END)=5 THEN N'1' ELSE N'0' END FROM sys.parameters WHERE object_id=OBJECT_ID(N'dbo.sp_FriendsList',N'P') AND parameter_id>0",signature));
    Need(signature==L"1","friend procedure parameter signature differs");
    Need(db.Begin(),"friend owned transaction unavailable");
    struct Rollback { Session& db; ~Rollback(){db.Abort();} } rollback{db};
    // SQLOLEDB defers the SQL transaction until a table access. A constant-only
    // SELECT does not start it, and sp_getapplock(Transaction) returns -999.
    std::wstring transactionCount;
    Need(db.String(L"SELECT CONVERT(nvarchar(20),@@TRANCOUNT) FROM sys.objects WHERE object_id=OBJECT_ID(N'dbo.sp_FriendsList')",transactionCount),"friend transaction probe unavailable");
    Need(transactionCount==L"1","friend SQL transaction did not start");
    std::wstring lock;
    Need(db.String(L"DECLARE @r int; EXEC @r=sys.sp_getapplock @Resource=N'sdgo.friend_list_fix.mod.v1',@LockMode='Exclusive',@LockOwner='Transaction',@LockTimeout=2000; SELECT CONVERT(nvarchar(20),@r)",lock),"friend lock result unavailable");
    if(lock.empty()||lock[0]==L'-')throw std::runtime_error("friend policy SQL lock unavailable ("+std::to_string(std::stoi(lock))+")");
    const auto schema=Identifier(kSchema);std::wstring exists;
    Need(db.String(L"SELECT CASE WHEN SCHEMA_ID(N'sdgo_friend_list_fix') IS NULL THEN N'0' ELSE N'1' END",exists));
    if(exists==L"0") {
        Need(db.Exec(L"CREATE SCHEMA "+schema+L" AUTHORIZATION dbo"),"friend schema creation failed");
        Need(db.Exec(L"EXEC sys.sp_addextendedproperty @name=N'sdgo_owner',@value=N'friend_list_fix/v1',@level0type=N'SCHEMA',@level0name=N'sdgo_friend_list_fix'"),"friend ownership marker creation failed");
        Need(db.Exec(L"CREATE TABLE "+schema+L".state(procedure_name nvarchar(128) NOT NULL PRIMARY KEY,source_hash char(64) NOT NULL,installed_hash char(64) NOT NULL,sql_options int NOT NULL)"),"friend state creation failed");
    }
    std::wstring owner;
    Need(db.String(L"SELECT ISNULL(CONVERT(nvarchar(128),(SELECT value FROM sys.extended_properties WHERE class=3 AND major_id=SCHEMA_ID(N'sdgo_friend_list_fix') AND name=N'sdgo_owner')),N'')",owner));
    if(owner!=L"friend_list_fix/v1")throw std::runtime_error("friend policy schema belongs to another owner");
    std::wstring sessionOptions;
    Need(db.String(L"SELECT CONVERT(nvarchar(10),CONVERT(int,SESSIONPROPERTY('ANSI_NULLS'))+2*CONVERT(int,SESSIONPROPERTY('QUOTED_IDENTIFIER')))",sessionOptions));
    const int oldOptions=std::stoi(sessionOptions);
    const auto setOptions=[&](int flags){return db.Exec(std::wstring(L"SET ANSI_NULLS ")+(flags&1?L"ON":L"OFF"))&&db.Exec(std::wstring(L"SET QUOTED_IDENTIFIER ")+(flags&2?L"ON":L"OFF"));};
    try {
        for(const auto* rawName:kNames) {
            const std::wstring name=rawName;
            const auto native=Literal(L"dbo."+name),alias=Literal(std::wstring(kSchema)+L"."+name);
            std::wstring source,current,flags,savedHash,savedOptions,currentOptions;
            Need(db.String(L"SELECT ISNULL(OBJECT_DEFINITION(OBJECT_ID("+native+L",N'P')),N'')",source));
            if(source.empty())throw std::runtime_error("native friend procedure unavailable");
            Need(db.String(L"SELECT CONVERT(nvarchar(10),CONVERT(int,uses_ansi_nulls)+2*CONVERT(int,uses_quoted_identifier)) FROM sys.sql_modules WHERE object_id=OBJECT_ID("+native+L",N'P')",flags));
            Need(db.String(L"SELECT ISNULL(OBJECT_DEFINITION(OBJECT_ID("+alias+L",N'P')),N'')",current));
            Need(db.String(L"SELECT ISNULL((SELECT CONVERT(nvarchar(64),installed_hash) FROM "+schema+L".state WHERE procedure_name="+Literal(name)+L"),N'')",savedHash));
            if((current.empty()!=savedHash.empty())||(!current.empty()&&Hash(Canonical(current,name,kSchema))!=savedHash))
                throw std::runtime_error("friend policy object has an external edit");
            if(!current.empty()) {
                Need(db.String(L"SELECT CONVERT(nvarchar(10),sql_options) FROM "+schema+L".state WHERE procedure_name="+Literal(name),savedOptions));
                Need(db.String(L"SELECT CONVERT(nvarchar(10),CONVERT(int,uses_ansi_nulls)+2*CONVERT(int,uses_quoted_identifier)) FROM sys.sql_modules WHERE object_id=OBJECT_ID("+alias+L",N'P')",currentOptions));
                if(savedOptions!=currentOptions)throw std::runtime_error("friend policy SQL options have an external edit");
            }
            const auto clone=Clone(source,name,!current.empty(),nameCollation,gameDatabase);
            const auto sourceHash=Hash(Canonical(source,name,L"dbo"));
            const auto targetHash=Hash(Canonical(clone,name,kSchema));
            const int options=std::stoi(flags);
            if(current.empty()||targetHash!=savedHash||savedOptions!=flags) {
                Need(setOptions(options),"friend creation options failed");Need(db.Exec(clone),"friend clone creation failed");
                std::wstring installed;Need(db.String(L"SELECT OBJECT_DEFINITION(OBJECT_ID("+alias+L",N'P'))",installed));
                if(Hash(Canonical(installed,name,kSchema))!=targetHash)throw std::runtime_error("friend policy readback mismatch");
                Need(db.Exec(L"DELETE FROM "+schema+L".state WHERE procedure_name="+Literal(name)));
                Need(db.Exec(L"INSERT "+schema+L".state(procedure_name,source_hash,installed_hash,sql_options) VALUES("+Literal(name)+L","+Literal(sourceHash)+L","+Literal(targetHash)+L","+std::to_wstring(options)+L")"));
            }
        }
        Need(setOptions(oldOptions));Need(db.Commit());return true;
    }catch(...){db.Abort();Need(setOptions(oldOptions),"friend SQL option restoration failed");throw;}
}
}
