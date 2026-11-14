#pragma once
#include "sdgo/rgb_paint_protocol.h"
namespace sdgo::rgb_paint::sql {
inline std::wstring Hex(const std::uint8_t* p,std::size_t n){const wchar_t digits[]=L"0123456789ABCDEF";std::wstring s=L"0x";for(std::size_t i=0;i<n;++i){s+=digits[p[i]>>4];s+=digits[p[i]&15];}return s;}
inline bool Unhex(const std::wstring& s,std::uint8_t* p,std::size_t n){
    if(s.size()!=2*n)return false;const auto digit=[](wchar_t c)->int{if(c>=L'0'&&c<=L'9')return c-L'0';if(c>=L'A'&&c<=L'F')return c-L'A'+10;if(c>=L'a'&&c<=L'f')return c-L'a'+10;return -1;};
    for(std::size_t i=0;i<n;++i){const int a=digit(s[i*2]),b=digit(s[i*2+1]);if(a<0||b<0)return false;p[i]=static_cast<std::uint8_t>(a*16+b);}return true;
}
// Only validated integers and canonical binary hex enter this batch. Clients
// cannot supply SQL text, identifiers, owner identity or connection settings.
inline std::wstring Request(std::uint32_t actor,std::uint64_t uid,const Record* proposal){
    if(!actor||actor>INT32_MAX||!uid||uid>INT64_MAX)return {};
    wire::Bytes encoded{};if(proposal&&(!wire::Encode(*proposal,encoded)||proposal->state.identity.unit!=uid||proposal->state.revision>=INT64_MAX))return {};
    std::wstring s=L"SET NOCOUNT ON; SET XACT_ABORT ON; BEGIN TRY BEGIN TRANSACTION; DECLARE @uid bigint="+std::to_wstring(uid)+L",@actor int="+std::to_wstring(actor)+L",@owner int,@unit int,@result int=0,@native binary(12); ";
    for(unsigned i=1;i<=6;++i)s+=L"DECLARE @c"+std::to_wstring(i)+L" int; ";
    s+=L"SELECT @owner=AccountNumber,@unit=UnitNumber";
    for(unsigned i=1;i<=6;++i)s+=L",@c"+std::to_wstring(i)+L"=CONVERT(int,UnitColorNumber_"+std::to_wstring(i)+L") & 65535";
    s+=L" FROM dbo.tbl_UserUnits WITH(UPDLOCK,HOLDLOCK) WHERE UserUnitNumber=@uid; IF @owner IS NULL BEGIN ROLLBACK; SELECT 6; RETURN; END; ";
    if(proposal)s+=L"IF @owner<>@actor BEGIN ROLLBACK; SELECT 9; RETURN; END; ";
    s+=L"SET @native=";
    for(unsigned i=1;i<=6;++i){if(i>1)s+=L"+";const auto c=L"@c"+std::to_wstring(i);s+=L"CONVERT(binary(1),"+c+L" & 255)+CONVERT(binary(1),"+c+L" / 256)";}
    s+=L"; IF NOT EXISTS(SELECT 1 FROM dbo.sdgo_mod_rgb_paint WITH(UPDLOCK,HOLDLOCK) WHERE UserUnitNumber=@uid) INSERT dbo.sdgo_mod_rgb_paint(UserUnitNumber,UnitNumber,Revision,NativeColors) VALUES(@uid,@unit,1,@native); "
       L"UPDATE dbo.sdgo_mod_rgb_paint SET UnitNumber=@unit,NativeColors=@native,Colors=NULL,Revision=Revision+1 WHERE UserUnitNumber=@uid AND (UnitNumber<>@unit OR NativeColors<>@native); ";
    if(proposal){const auto colors=Hex(encoded.data()+36,24);
        s+=L"IF @unit<>"+std::to_wstring(proposal->type)+L" OR @native<>"+Hex(encoded.data()+24,12)+L" SET @result=12; ELSE IF EXISTS(SELECT 1 FROM dbo.sdgo_mod_rgb_paint WHERE UserUnitNumber=@uid AND Revision="+std::to_wstring(proposal->state.revision)+L") UPDATE dbo.sdgo_mod_rgb_paint SET Colors="+colors+L",Revision=Revision+1 WHERE UserUnitNumber=@uid; ELSE IF NOT EXISTS(SELECT 1 FROM dbo.sdgo_mod_rgb_paint WHERE UserUnitNumber=@uid AND Revision="+std::to_wstring(proposal->state.revision+1)+L" AND Colors="+colors+L") SET @result=12; ";
    }
    s+=L"SELECT @result,UnitNumber,Revision,CONVERT(varchar(24),NativeColors,2),ISNULL(CONVERT(varchar(48),Colors,2),'') FROM dbo.sdgo_mod_rgb_paint WHERE UserUnitNumber=@uid; COMMIT; END TRY BEGIN CATCH IF @@TRANCOUNT>0 ROLLBACK; THROW; END CATCH;";
    return s;
}
}
