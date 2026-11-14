#pragma once
#include "sdgo/surface_decal_protocol.h"
#include <string>

namespace sdgo::surface_decal::sql {
inline std::wstring Hex(const std::uint8_t* data,std::size_t n) {
    static constexpr wchar_t digits[]=L"0123456789ABCDEF";std::wstring result=L"0x";
    for(std::size_t i=0;i<n;++i){result+=digits[data[i]>>4];result+=digits[data[i]&15];}return result;
}
// The caller supplies integers from authenticated transport and validated wire
// data only. No SQL identifiers, strings or connection settings come from peers.
inline std::wstring Request(std::uint32_t actor,std::uint64_t serial,const Appearance* proposal) {
    if(!actor||actor>INT32_MAX||!serial||serial>INT64_MAX)return {};
    wire::Bytes encoded{};
    if(proposal&&(!wire::Encode(*proposal,encoded)||proposal->unitSerial!=serial||proposal->revision>=INT64_MAX))return {};
    std::wstring s=L"SET NOCOUNT ON; SET XACT_ABORT ON; BEGIN TRY BEGIN TRANSACTION; DECLARE @uid bigint="+std::to_wstring(serial)+
        L", @actor int="+std::to_wstring(actor)+L", @owner int, @unit int, @d1 int, @d2 int, @d3 int, @result int=0; "
        L"SELECT @owner=AccountNumber,@unit=UnitNumber,@d1=ISNULL(UnitDecalNumber_1,0),@d2=ISNULL(UnitDecalNumber_2,0),@d3=ISNULL(UnitDecalNumber_3,0) "
        L"FROM dbo.tbl_UserUnits WITH(UPDLOCK,HOLDLOCK) WHERE UserUnitNumber=@uid; "
        L"IF @owner IS NULL BEGIN ROLLBACK; SELECT 6; RETURN; END; ";
    if(proposal)s+=L"IF @owner<>@actor BEGIN ROLLBACK; SELECT 9; RETURN; END; ";
    s+=L"IF NOT EXISTS(SELECT 1 FROM dbo.sdgo_mod_surface_decal WITH(UPDLOCK,HOLDLOCK) WHERE UserUnitNumber=@uid) "
        L"INSERT dbo.sdgo_mod_surface_decal(UserUnitNumber,UnitNumber,Revision,Sticker1,Sticker2,Sticker3) VALUES(@uid,@unit,1,@d1,@d2,@d3); "
        L"UPDATE dbo.sdgo_mod_surface_decal SET "
        L"Placement1=CASE WHEN UnitNumber<>@unit OR Sticker1<>@d1 THEN NULL ELSE Placement1 END,"
        L"Placement2=CASE WHEN UnitNumber<>@unit OR Sticker2<>@d2 THEN NULL ELSE Placement2 END,"
        L"Placement3=CASE WHEN UnitNumber<>@unit OR Sticker3<>@d3 THEN NULL ELSE Placement3 END,"
        L"UnitNumber=@unit,Sticker1=@d1,Sticker2=@d2,Sticker3=@d3,Revision=Revision+1 "
        L"WHERE UserUnitNumber=@uid AND (UnitNumber<>@unit OR Sticker1<>@d1 OR Sticker2<>@d2 OR Sticker3<>@d3); ";
    if(proposal){
        s+=L"IF @unit<>"+std::to_wstring(proposal->unitType);
        for(unsigned i=0;i<3;++i)s+=L" OR @d"+std::to_wstring(i+1)+L"<>"+std::to_wstring(proposal->slots[i].sticker);
        s+=L" SET @result=12; ELSE IF EXISTS(SELECT 1 FROM dbo.sdgo_mod_surface_decal WHERE UserUnitNumber=@uid AND Revision="+std::to_wstring(proposal->revision)+L") BEGIN UPDATE dbo.sdgo_mod_surface_decal SET Revision=Revision+1";
        for(unsigned i=0;i<3;++i)s+=L",Placement"+std::to_wstring(i+1)+L"="+Hex(encoded.data()+wire::headerBytes+i*wire::slotBytes,wire::slotBytes);
        s+=L" WHERE UserUnitNumber=@uid; END ELSE IF NOT EXISTS(SELECT 1 FROM dbo.sdgo_mod_surface_decal WHERE UserUnitNumber=@uid AND Revision="+std::to_wstring(proposal->revision+1);
        for(unsigned i=0;i<3;++i)s+=L" AND Placement"+std::to_wstring(i+1)+L"="+Hex(encoded.data()+wire::headerBytes+i*wire::slotBytes,wire::slotBytes);
        s+=L") SET @result=12; "; // exact retry, never applies a mutation twice
    }
    s+=L"SELECT @result,UnitNumber,Revision,Sticker1,Sticker2,Sticker3,"
       L"ISNULL(CONVERT(varchar(208),Placement1,2),''),ISNULL(CONVERT(varchar(208),Placement2,2),''),ISNULL(CONVERT(varchar(208),Placement3,2),'') "
       L"FROM dbo.sdgo_mod_surface_decal WHERE UserUnitNumber=@uid; COMMIT; END TRY BEGIN CATCH IF @@TRANCOUNT>0 ROLLBACK; THROW; END CATCH;";
    return s;
}
inline bool Unhex(const std::wstring& text,std::uint8_t* out,std::size_t count) {
    if(text.size()!=count*2)return false;
    const auto digit=[](wchar_t c)->int {if(c>=L'0'&&c<=L'9')return c-L'0';if(c>=L'A'&&c<=L'F')return c-L'A'+10;if(c>=L'a'&&c<=L'f')return c-L'a'+10;return -1;};
    for(std::size_t i=0;i<count;++i){const int a=digit(text[i*2]),b=digit(text[i*2+1]);if(a<0||b<0)return false;out[i]=static_cast<std::uint8_t>(a*16+b);}return true;
}
}
