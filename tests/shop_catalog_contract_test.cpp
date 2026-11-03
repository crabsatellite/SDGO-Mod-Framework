#include "../mods/shop_catalog/catalog_contract.h"

#include <iostream>
#include <string>

namespace {
std::wstring Statement(const std::wstring& sql, const std::wstring& prefix)
{
    const size_t begin = sql.find(prefix);
    if (begin == std::wstring::npos) return {};
    const size_t end = sql.find(L";", begin);
    if (end == std::wstring::npos) return {};
    return sql.substr(begin, end - begin + 1);
}

bool Has(const std::wstring& text, const wchar_t* value)
{
    return text.find(value) != std::wstring::npos;
}

int Fail(const char* message)
{
    std::cerr << "SHOP CATALOG CONTRACT FAIL: " << message << "\n";
    return 1;
}
}

int main()
{
    sdgo::shop_catalog::Catalog catalog;
    catalog.id = L"contract";
    catalog.revision = L"test";
    sdgo::shop_catalog::Product product;
    product.key = L"unit";
    product.id = 1019001;
    product.sku = L"190001";
    product.templateId = 1010001;
    product.category = 1120022;
    product.price = 500;
    product.payment = 1;
    product.validity = 1;
    product.enabled = true;
    product.name = L"Unit";
    product.label = L"Permanent";
    product.timeLimit = 0;
    catalog.products.push_back(product);

    sdgo::shop_catalog::StoreItemsLayout computed;
    computed.subitemCountComputed = true;
    computed.validityComputed = true;
    computed.priceComputed = true;
    computed.paymentComputed = true;
    const std::wstring native = sdgo::shop_catalog::ApplySql(catalog, computed);
    const std::wstring nativeInsert = Statement(native, L"INSERT dbo.tbls_StoreItems(");
    const std::wstring nativeUpdate = Statement(native, L"UPDATE dbo.tbls_StoreItems SET");
    if (nativeInsert.empty() || nativeUpdate.empty()) return Fail("missing generated StoreItems statements");
    for (const wchar_t* column : {L"f_subitemCnt", L"f_validity", L"f_price", L"f_payment"}) {
        if (Has(nativeInsert, column)) return Fail("computed column included in StoreItems INSERT");
    }
    for (const wchar_t* assignment : {L"f_validity=@validity", L"f_price=@price", L"f_payment=@payment"}) {
        if (Has(nativeUpdate, assignment)) return Fail("computed column included in StoreItems UPDATE");
    }
    if (!Has(native, L"Native shop computed-column layout changed") ||
        !Has(native, L"Managed product business fields did not round-trip through native schema") ||
        !Has(native, L"p.f_subitemCnt=1") || !Has(native, L"p.f_validity=@validity") ||
        !Has(native, L"p.f_price=@price") || !Has(native, L"p.f_payment=@payment"))
        return Fail("native computed layout lacks metadata or effective-value guards");

    const sdgo::shop_catalog::StoreItemsLayout writable;
    const std::wstring legacy = sdgo::shop_catalog::ApplySql(catalog, writable);
    const std::wstring legacyInsert = Statement(legacy, L"INSERT dbo.tbls_StoreItems(");
    const std::wstring legacyUpdate = Statement(legacy, L"UPDATE dbo.tbls_StoreItems SET");
    for (const wchar_t* column : {L"f_subitemCnt", L"f_validity", L"f_price", L"f_payment"}) {
        if (!Has(legacyInsert, column)) return Fail("writable column omitted from StoreItems INSERT");
    }
    for (const wchar_t* assignment : {L"f_validity=@validity", L"f_price=@price", L"f_payment=@payment"}) {
        if (!Has(legacyUpdate, assignment)) return Fail("writable column omitted from StoreItems UPDATE");
    }

    std::cout << "SHOP CATALOG CONTRACT PASS: computed and writable native layouts stay strict\n";
    return 0;
}
