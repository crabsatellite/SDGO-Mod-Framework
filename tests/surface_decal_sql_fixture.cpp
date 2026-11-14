// Emits an integration batch for an existing isolated SQL Server connection.
// The runner must select a task-owned fixture database, never the game's
// database. Execute this output with a native SQL client (not the game host).
#include "sdgo/surface_decal_sql.h"
#include <iostream>

using namespace sdgo::surface_decal;
void Query(const Appearance* value,int status,int revision,unsigned actor=101,std::uint64_t uid=1001) {
    auto s=sql::Request(actor,uid,value);
    std::wcout<<L"GO\n-- EXPECT "<<status<<L" "<<revision<<L"\n"<<s<<L"\n";
}
int main() {
    std::wcout<<LR"SQL(SET NOCOUNT ON;
IF DB_NAME() <> 'SDGOSurfaceDecalFixture' THROW 51000,'select the isolated fixture database',1;
IF OBJECT_ID('dbo.tbl_UserUnits') IS NULL CREATE TABLE dbo.tbl_UserUnits(UserUnitNumber bigint PRIMARY KEY,AccountNumber int,UnitNumber int,UnitDecalNumber_1 int,UnitDecalNumber_2 int,UnitDecalNumber_3 int);
DELETE dbo.sdgo_mod_surface_decal; DELETE dbo.tbl_UserUnits;
INSERT dbo.tbl_UserUnits VALUES(1001,101,100,10,20,0);
)SQL";
    Query(nullptr,0,1);
    Appearance a;a.unitSerial=1001;a.unitType=100;a.revision=1;a.slots[0].sticker=10;a.slots[1].sticker=20;
    a.slots[0].positioned=true;a.slots[0].placement.anchor.geometry=99;
    Query(&a,0,2);Query(&a,0,2);
    a.slots[0].placement.width=2;Query(&a,12,2);
    Query(&a,9,0,102);Query(nullptr,6,0,101,9999);
    std::wcout<<L"GO\nUPDATE dbo.tbl_UserUnits SET UnitDecalNumber_1=11 WHERE UserUnitNumber=1001;\n";
    Query(nullptr,0,3);
    a.revision=3;a.slots[0].sticker=11;Query(&a,0,4);Query(nullptr,0,4,102);
}
