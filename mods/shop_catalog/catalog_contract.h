#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sdgo::shop_catalog {
struct Product {
    std::wstring key;
    int id = 0;
    std::wstring sku;
    int templateId = 0;
    int category = 0;
    int price = 0;
    int payment = 1; // Native ordinary shop: 1 GB/GP, 2 MB/GC.
    int validity = 1;
    bool enabled = true;
    std::wstring name;
    std::wstring label;
    int timeLimit = -1;
    int search1 = 0;
    int search2 = 0;
};
struct Catalog {
    std::wstring id;
    std::wstring revision;
    std::vector<Product> products;
};
struct StoreItemsLayout {
    bool subitemCountComputed = false;
    bool validityComputed = false;
    bool priceComputed = false;
    bool paymentComputed = false;
};
inline std::wstring Quote(const std::wstring& text)
{
    std::wstring result = L"N'";
    for (wchar_t c : text) {
        result += c;
        if (c == L'\'') result += L'\'';
    }
    return result + L"'";
}
inline std::wstring Identifier(const std::wstring& text)
{
    std::wstring result = L"[";
    for (wchar_t c : text) { result += c; if (c == L']') result += L']'; }
    return result + L"]";
}

// The caller owns the connection and commits only after reading the result and
// preparing the client Unicode profile. This batch does not change any native
// procedure, balance, inventory, log, or unowned product.
inline std::wstring ApplySql(const Catalog& catalog, const StoreItemsLayout& layout)
{
    std::wstring sql = LR"SQL(
SET NOCOUNT ON; SET XACT_ABORT ON;
BEGIN TRANSACTION;
DECLARE @lock int;
EXEC @lock=sys.sp_getapplock @Resource=N'sdgo.shop_catalog',@LockMode='Exclusive',@LockOwner='Session',@LockTimeout=5000;
IF @lock<0 BEGIN RAISERROR('Shop catalog is busy',16,1); RETURN; END;
IF OBJECT_ID(N'dbo.tbls_StoreItems',N'U') IS NULL OR OBJECT_ID(N'dbo.tbls_SubItems',N'U') IS NULL OR OBJECT_ID(N'dbo.tbls_StoreItemsPlus',N'U') IS NULL
BEGIN RAISERROR('Native shop schema missing',16,1); RETURN; END;
)SQL";
    sql += L"IF ISNULL(COLUMNPROPERTY(OBJECT_ID(N'dbo.tbls_StoreItems'),N'f_subitemCnt','IsComputed'),-1)<>" +
        std::to_wstring(layout.subitemCountComputed ? 1 : 0) +
        L" OR ISNULL(COLUMNPROPERTY(OBJECT_ID(N'dbo.tbls_StoreItems'),N'f_validity','IsComputed'),-1)<>" +
        std::to_wstring(layout.validityComputed ? 1 : 0) +
        L" OR ISNULL(COLUMNPROPERTY(OBJECT_ID(N'dbo.tbls_StoreItems'),N'f_price','IsComputed'),-1)<>" +
        std::to_wstring(layout.priceComputed ? 1 : 0) +
        L" OR ISNULL(COLUMNPROPERTY(OBJECT_ID(N'dbo.tbls_StoreItems'),N'f_payment','IsComputed'),-1)<>" +
        std::to_wstring(layout.paymentComputed ? 1 : 0) + LR"SQL(
BEGIN RAISERROR('Native shop computed-column layout changed',16,1); RETURN; END;
IF OBJECT_ID(N'dbo.sdgo_ShopCatalog',N'U') IS NULL
CREATE TABLE dbo.sdgo_ShopCatalog(
 CatalogId nvarchar(64) NOT NULL, ProductKey nvarchar(64) NOT NULL,
 ProductId int NOT NULL UNIQUE, ItemId char(6) NOT NULL UNIQUE, OptionId int NOT NULL UNIQUE,
 Revision nvarchar(64) NOT NULL, PRIMARY KEY(CatalogId,ProductKey));
DECLARE @owner nvarchar(64),@revision nvarchar(64),@key nvarchar(64),@pid int,@sku char(6),@template int,
 @category int,@price int,@payment char(1),@validity int,@visible char(1),@name nvarchar(50),@label nvarchar(50),
 @option int,@tab int,@mode int,@ref int,@kind int,@s1 int,@s2 int,@qty int,@prefix char(1),@unitSearch1 int,@unitSearch2 int;
CREATE TABLE #sdgo_ShopInput(
 ProductKey nvarchar(64) COLLATE DATABASE_DEFAULT NOT NULL PRIMARY KEY,ProductId int NOT NULL UNIQUE,ItemId char(6) COLLATE DATABASE_DEFAULT NOT NULL UNIQUE,
 TemplateId int NOT NULL,Category int NOT NULL,Price int NOT NULL,Payment char(1) COLLATE DATABASE_DEFAULT NOT NULL,
 Validity int NOT NULL,Visible char(1) COLLATE DATABASE_DEFAULT NOT NULL,Name nvarchar(50) COLLATE DATABASE_DEFAULT NOT NULL,Label nvarchar(50) COLLATE DATABASE_DEFAULT NOT NULL,
 Mode int NOT NULL,Search1 int NOT NULL,Search2 int NOT NULL);
)SQL";
    sql += L"SET @owner=" + Quote(catalog.id) + L";SET @revision=" + Quote(catalog.revision) + L";\n";
    for (const Product& p : catalog.products) {
        sql += L"INSERT #sdgo_ShopInput VALUES(" + Quote(p.key) + L"," + std::to_wstring(p.id) +
            L"," + Quote(p.sku) + L"," + std::to_wstring(p.templateId) +
            L"," + std::to_wstring(p.category) + L"," + std::to_wstring(p.price) +
            L"," + Quote(std::to_wstring(p.payment)) + L"," + std::to_wstring(p.validity) +
            L"," + Quote(p.enabled ? L"1" : L"0") + L"," + Quote(p.name) + L"," + Quote(p.label) +
            L"," + std::to_wstring(p.timeLimit) + L"," + std::to_wstring(p.search1) + L"," + std::to_wstring(p.search2) + L");\n";
    }
    const std::wstring fetch = L"FETCH NEXT FROM sdgo_ShopRows INTO @key,@pid,@sku,@template,@category,@price,@payment,@validity,@visible,@name,@label,@mode,@unitSearch1,@unitSearch2;\n";
    sql += LR"SQL(
DECLARE sdgo_ShopRows CURSOR LOCAL FAST_FORWARD FOR
 SELECT ProductKey,ProductId,ItemId,TemplateId,Category,Price,Payment,Validity,Visible,Name,Label,Mode,Search1,Search2
 FROM #sdgo_ShopInput ORDER BY ProductId;
OPEN sdgo_ShopRows;
)SQL" + fetch + L"WHILE @@FETCH_STATUS=0 BEGIN\n";
    sql += LR"SQL(
SET @option=NULL; SET @tab=NULL; SET @s1=0; SET @s2=0;
IF (SELECT COUNT(*) FROM dbo.tbls_StoreItems WHERE f_productid=@template AND ISNULL(f_package,'0')='0')<>1
BEGIN RAISERROR('Native product template is ambiguous or missing',16,1); RETURN; END;
SELECT @tab=f_depth1,@s1=ISNULL(f_search1,0),@s2=ISNULL(f_search2,0),@prefix=LEFT(f_itemid,1) FROM dbo.tbls_StoreItems WHERE f_productid=@template AND ISNULL(f_package,'0')='0';
IF @tab IS NULL OR @tab=20 OR @pid<=(@tab+100)*10000 OR @pid>=(@tab+101)*10000
BEGIN RAISERROR('Invalid native product template or tab range',16,1); RETURN; END;
IF LEFT(@sku,1)<>@prefix BEGIN RAISERROR('SKU kind does not match native template',16,1); RETURN; END;
)SQL";
        sql += L"SET @kind=@category/100000;\n";
        sql += LR"SQL(
IF @mode IS NULL BEGIN RAISERROR('Item category is unknown to the game database',16,1); RETURN; END;
IF @kind=11 AND @mode=0 AND @validity<>1
BEGIN RAISERROR('Permanent unit validity must be one inventory item',16,1); RETURN; END;
)SQL";
        sql += L"IF NOT EXISTS(SELECT 1 FROM dbo.tbls_SubItems t JOIN dbo.tbls_StoreItems p ON p.f_itemid=t.f_itemid WHERE p.f_productid=@template AND t.f_categoryNumber/100000=@kind)\n";
        sql += LR"SQL(
BEGIN RAISERROR('Template uses a different native item kind',16,1); RETURN; END;
)SQL";
        sql += L"IF @kind=11 BEGIN SET @s1=@unitSearch1;SET @s2=@unitSearch2; END;\n";
        sql += LR"SQL(
SELECT @option=OptionId FROM dbo.sdgo_ShopCatalog WHERE CatalogId=@owner AND ProductKey=@key AND ProductId=@pid AND ItemId=@sku;
IF @option IS NULL
BEGIN
 IF EXISTS(SELECT 1 FROM dbo.sdgo_ShopCatalog WHERE CatalogId=@owner AND ProductKey=@key)
  OR EXISTS(SELECT 1 FROM dbo.tbls_StoreItems WHERE f_productid=@pid OR f_itemid=@sku)
  OR EXISTS(SELECT 1 FROM dbo.tbls_SubItems WHERE f_itemid=@sku)
  OR EXISTS(SELECT 1 FROM dbo.tbls_StoreItemsPlus WHERE f_productid=@pid)
  OR EXISTS(SELECT 1 FROM dbo.sdgo_ShopCatalog WHERE ProductId=@pid OR ItemId=@sku)
 BEGIN RAISERROR('Shop ID collision or immutable identity changed',16,1); RETURN; END;
)SQL";
        std::wstring columns = L"f_productid,f_itemid,f_itemNm,f_depth0,f_depth1,f_depth2,f_depth3";
        std::wstring values = L"@pid,@sku,@name,3,@tab,CASE WHEN @kind=11 AND @mode=1 THEN 2 ELSE 1 END,0";
        const auto field = [&](const wchar_t* name, const wchar_t* value) {
            columns += L"," + std::wstring(name);
            values += L"," + std::wstring(value);
        };
        if (!layout.subitemCountComputed) field(L"f_subitemCnt", L"1");
        if (!layout.validityComputed) field(L"f_validity", L"@validity");
        if (!layout.priceComputed) field(L"f_price", L"@price");
        field(L"f_DC", L"0");
        field(L"f_DCrate", L"0");
        if (!layout.paymentComputed) field(L"f_payment", L"@payment");
        field(L"f_limitedAdition", L"'2'");
        field(L"f_quantity", L"99999999");
        field(L"f_package", L"'0'");
        field(L"f_display", L"@visible");
        field(L"f_search1", L"@s1");
        field(L"f_search2", L"@s2");
        field(L"f_limitedGift", L"'1'");
        sql += L" INSERT dbo.tbls_StoreItems(" + columns + L") VALUES(" + values + L");\n";
        sql += LR"SQL(
 INSERT dbo.tbls_SubItems(f_itemid,f_menu_id,f_itemNm,f_price,f_validity,f_payment,f_categoryNumber,f_display,f_index,f_reProductID,f_imgIitemid,f_regdate)
 VALUES(@sku,'',@label,@price,@validity,@payment,@category,@visible,'1',@pid,'0',GETDATE());
 SET @option=CONVERT(int,SCOPE_IDENTITY());
 INSERT dbo.sdgo_ShopCatalog(CatalogId,ProductKey,ProductId,ItemId,OptionId,Revision) VALUES(@owner,@key,@pid,@sku,@option,@revision);
END;
IF (SELECT COUNT(*) FROM dbo.tbls_StoreItems WHERE f_productid=@pid AND f_itemid=@sku)<>1
 OR (SELECT COUNT(*) FROM dbo.tbls_SubItems WHERE f_itemtblidx=@option AND f_itemid=@sku)<>1
 OR (SELECT COUNT(*) FROM dbo.tbls_SubItems WHERE f_itemid=@sku)<>1
BEGIN RAISERROR('Managed product identity has drifted',16,1); RETURN; END;
IF NOT EXISTS(SELECT 1 FROM dbo.tbls_StoreItemsPlus WHERE f_productid=@pid)
 INSERT dbo.tbls_StoreItemsPlus(f_productid,f_desc1,f_desc2,f_desc3,f_info1,f_info2,f_info3,f_info4,f_info5,f_info6,f_info7,f_info8,f_info9,f_info10,f_info11,f_regdate)
 VALUES(@pid,N'',N'',N'',0,0,0,0,0,0,0,0,0,0,0,GETDATE());
)SQL";
        sql += L"UPDATE dbo.tbls_StoreItems SET f_itemNm=@name";
        if (!layout.validityComputed) sql += L",f_validity=@validity";
        if (!layout.priceComputed) sql += L",f_price=@price";
        if (!layout.paymentComputed) sql += L",f_payment=@payment";
        sql += LR"SQL(,f_display=@visible,
 f_depth2=CASE WHEN @kind=11 AND @mode=1 THEN 2 ELSE 1 END,f_search1=@s1,f_search2=@s2 WHERE f_productid=@pid AND f_itemid=@sku;
UPDATE dbo.tbls_SubItems SET f_itemNm=@label,f_price=@price,f_validity=@validity,f_payment=@payment,f_categoryNumber=@category,f_display=@visible,f_imgIitemid='0',f_reProductID=@pid
 WHERE f_itemtblidx=@option AND f_itemid=@sku;
UPDATE dbo.sdgo_ShopCatalog SET Revision=@revision WHERE CatalogId=@owner AND ProductKey=@key;
)SQL";
        std::wstring parentFields = L"p.f_itemNm=@name AND p.f_display=@visible AND p.f_depth2=CASE WHEN @kind=11 AND @mode=1 THEN 2 ELSE 1 END AND ISNULL(p.f_search1,0)=@s1 AND ISNULL(p.f_search2,0)=@s2";
        parentFields += layout.subitemCountComputed
            ? L" AND ((@visible='1' AND p.f_subitemCnt=1) OR (@visible='0' AND p.f_subitemCnt=0))"
            : L" AND p.f_subitemCnt=1";
        parentFields += layout.validityComputed
            ? L" AND ((@visible='1' AND p.f_validity=@validity) OR (@visible='0' AND p.f_validity IS NULL))"
            : L" AND p.f_validity=@validity";
        parentFields += layout.priceComputed
            ? L" AND ((@visible='1' AND p.f_price=@price) OR (@visible='0' AND p.f_price=0))"
            : L" AND p.f_price=@price";
        parentFields += layout.paymentComputed
            ? L" AND ((@visible='1' AND p.f_payment=@payment) OR (@visible='0' AND p.f_payment IS NULL))"
            : L" AND p.f_payment=@payment";
        sql += L"IF NOT EXISTS(SELECT 1 FROM dbo.tbls_StoreItems p WHERE p.f_productid=@pid AND p.f_itemid=@sku AND " +
            parentFields + LR"SQL()
 OR NOT EXISTS(SELECT 1 FROM dbo.tbls_SubItems s WHERE s.f_itemtblidx=@option AND s.f_itemid=@sku
  AND s.f_itemNm=@label AND s.f_price=@price AND s.f_validity=@validity AND s.f_payment=@payment
  AND s.f_categoryNumber=@category AND s.f_display=@visible AND s.f_imgIitemid='0' AND s.f_reProductID=@pid)
BEGIN RAISERROR('Managed product business fields did not round-trip through native schema',16,1); RETURN; END;
)SQL";
    sql += fetch + L"END;\nCLOSE sdgo_ShopRows;DEALLOCATE sdgo_ShopRows;\n";
    sql += L"UPDATE p SET f_display='0' FROM dbo.tbls_StoreItems p JOIN dbo.sdgo_ShopCatalog s ON s.ProductId=p.f_productid AND s.ItemId=p.f_itemid WHERE s.CatalogId=@owner AND NOT EXISTS(SELECT 1 FROM #sdgo_ShopInput i WHERE i.ProductKey=s.ProductKey);\n";
    sql += L"UPDATE p SET f_display='0' FROM dbo.tbls_SubItems p JOIN dbo.sdgo_ShopCatalog s ON s.OptionId=p.f_itemtblidx AND s.ItemId=p.f_itemid WHERE s.CatalogId=@owner AND NOT EXISTS(SELECT 1 FROM #sdgo_ShopInput i WHERE i.ProductKey=s.ProductKey);\n";
    sql += LR"SQL(
SELECT s.ProductId,s.OptionId,p.f_categoryNumber,p.f_validity,p.f_payment,p.f_price,p.f_itemNm
 FROM dbo.sdgo_ShopCatalog s JOIN dbo.tbls_SubItems p ON p.f_itemtblidx=s.OptionId AND p.f_itemid=s.ItemId
 WHERE s.CatalogId=@owner AND p.f_display='1' ORDER BY s.ProductId;
DROP TABLE #sdgo_ShopInput;
)SQL";
    return sql;
}
} // namespace sdgo::shop_catalog
