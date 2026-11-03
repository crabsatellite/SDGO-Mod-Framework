#include <windows.h>
#include <bcrypt.h>
#include <sql.h>
#include <sqlext.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../shop_localization/dictionary.h"
#include "catalog_contract.h"
#include "catalog_runtime.h"
#include "search_sql.h"

namespace sdgo::shop_catalog {
namespace {
constexpr std::uint64_t kMaximumSearchIndexBytes = 8ull * 1024 * 1024;
constexpr char kOriginalSearchDefinitionHash[] =
    "595974c93de41196927d7cc665d6154a9932713182d0c1824246d57d579a43b6";

bool Success(SQLRETURN value) { return value == SQL_SUCCESS || value == SQL_SUCCESS_WITH_INFO; }
std::wstring Absolute(const std::wstring& path)
{
    std::vector<wchar_t> buffer(32768);
    const DWORD count = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!count || count >= buffer.size()) throw std::runtime_error("invalid catalog path");
    return {buffer.data(), count};
}
std::wstring Value(const std::wstring& file, const wchar_t* section, const wchar_t* key,
                   const wchar_t* fallback = L"")
{
    wchar_t text[2048] = {};
    const DWORD count = GetPrivateProfileStringW(section, key, fallback, text, 2048, file.c_str());
    if (count >= 2047) throw std::runtime_error("INI value too long");
    return text;
}
std::vector<std::uint8_t> ReadBytes(
    const std::wstring& path, std::uint64_t maximum, const char* label)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) ||
        (attributes.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        throw std::runtime_error(std::string(label) + " missing or is not a regular file");
    ULARGE_INTEGER size = {};
    size.HighPart = attributes.nFileSizeHigh;
    size.LowPart = attributes.nFileSizeLow;
    if (!size.QuadPart || size.QuadPart > maximum ||
        size.QuadPart > static_cast<std::uint64_t>(std::numeric_limits<DWORD>::max()))
        throw std::runtime_error(std::string(label) + " size outside supported range");
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error(std::string("cannot open ") + label);
    std::vector<std::uint8_t> result(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = ReadFile(
        file, result.data(), static_cast<DWORD>(result.size()), &read, nullptr) &&
        read == result.size();
    CloseHandle(file);
    if (!ok) throw std::runtime_error(std::string("incomplete ") + label + " read");
    return result;
}
std::wstring Utf8(const std::vector<std::uint8_t>& input)
{
    const char* data = reinterpret_cast<const char*>(input.data());
    int size = static_cast<int>(input.size());
    if (size >= 3 && static_cast<std::uint8_t>(data[0]) == 0xEF &&
        static_cast<std::uint8_t>(data[1]) == 0xBB &&
        static_cast<std::uint8_t>(data[2]) == 0xBF) {
        data += 3;
        size -= 3;
    }
    if (!size) throw std::runtime_error("empty UTF-8 shop search dictionary");
    const int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, size, nullptr, 0);
    if (units <= 0) throw std::runtime_error("invalid UTF-8 shop search dictionary");
    std::wstring result(static_cast<std::size_t>(units), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, size, result.data(), units) != units)
        throw std::runtime_error("incomplete UTF-8 shop search dictionary conversion");
    return result;
}
std::string Utf8(const std::wstring& input)
{
    if (input.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("invalid UTF-16 SQL definition");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
            result.data(), size, nullptr, nullptr) != size)
        throw std::runtime_error("incomplete UTF-8 SQL definition conversion");
    return result;
}
std::string Sha256(const void* data, std::size_t size)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    std::array<std::uint8_t, 32> digest = {};
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        throw std::runtime_error("SHA256 provider unavailable");
    const NTSTATUS status = BCryptHash(
        algorithm, nullptr, 0,
        const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(data)),
        static_cast<ULONG>(size), digest.data(),
        static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!BCRYPT_SUCCESS(status)) throw std::runtime_error("SHA256 failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto value : digest) {
        result.push_back(hex[value >> 4]);
        result.push_back(hex[value & 0x0F]);
    }
    return result;
}
std::wstring WidenAscii(const std::string& value)
{
    return {value.begin(), value.end()};
}
int Integer(const std::wstring& file, const wchar_t* section, const wchar_t* key, int fallback,
            int minimum, int maximum)
{
    const std::wstring text = Value(file, section, key, std::to_wstring(fallback).c_str());
    wchar_t* end = nullptr;
    const long long value = _wcstoi64(text.c_str(), &end, 10);
    if (!end || end == text.c_str() || *end || value < minimum || value > maximum)
        throw std::runtime_error("invalid numeric catalog value");
    return static_cast<int>(value);
}
bool Key(const std::wstring& value)
{
    return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](wchar_t ch) {
        return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
               (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_' || ch == L'.';
    });
}
void Text(const std::wstring& value)
{
    if (value.empty() || value.size() > 50 || value.find_first_of(L"\r\n") != std::wstring::npos)
        throw std::runtime_error("catalog label must contain 1..50 UTF-16 units on one line");
}
Catalog Load(const std::wstring& file, bool headerOnly)
{
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("catalog.ini missing");
    if (Integer(file, L"shop", L"Schema", 0, 1, 1) != 1) throw std::runtime_error("unsupported catalog schema");
    Catalog result;
    result.id = Value(file, L"shop", L"CatalogId");
    result.revision = Value(file, L"shop", L"Revision", L"manual");
    if (!Key(result.id) || !Key(result.revision)) throw std::runtime_error("invalid catalog identity");
    if (headerOnly) return result;
    std::vector<wchar_t> sections(65536);
    const DWORD size = GetPrivateProfileSectionNamesW(sections.data(), static_cast<DWORD>(sections.size()), file.c_str());
    if (size >= sections.size() - 2) throw std::runtime_error("catalog section list too long");
    std::set<std::wstring> keys, skus;
    std::set<int> ids;
    for (const wchar_t* section = sections.data(); *section; section += wcslen(section) + 1) {
        if (wcsncmp(section, L"product:", 8) != 0) continue;
        if (result.products.size() >= 256) throw std::runtime_error("catalog exceeds 256 products");
        Product product;
        product.key = section + 8;
        product.id = Integer(file, section, L"ProductId", 0, 1, 2147483647);
        product.templateId = Integer(file, section, L"TemplateId", 0, 1, 2147483647);
        product.sku = Value(file, section, L"ItemId");
        product.category = Integer(file, section, L"Category", 0, 1, 2147483647);
        product.price = Integer(file, section, L"Price", -1, 1, 2147483647);
        product.validity = Integer(file, section, L"Validity", 0, 1, 1000000);
        const auto currency = Value(file, section, L"Currency");
        if (currency != L"GB" && currency != L"MB") throw std::runtime_error("currency must be GB or MB");
        product.payment = currency == L"GB" ? 1 : 2;
        product.enabled = Integer(file, section, L"Enabled", 1, 0, 1) != 0;
        product.name = Value(file, section, L"Name");
        product.label = Value(file, section, L"Label");
        Text(product.name); Text(product.label);
        const auto chinese = [](const std::wstring& text) {
            return std::any_of(text.begin(), text.end(), [](wchar_t c) { return (c >= 0x3400 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF); });
        };
        if (!chinese(product.name) || !chinese(product.label))
            throw std::runtime_error("Chinese product name and sales label are required before publication");
        if (!Key(product.key) || !keys.insert(product.key).second || !ids.insert(product.id).second ||
            product.sku.size() != 6 || !std::all_of(product.sku.begin(), product.sku.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }) ||
            !skus.insert(product.sku).second || product.id == product.templateId)
            throw std::runtime_error("duplicate or invalid managed product identity");
        result.products.push_back(std::move(product));
    }
    return result;
}
std::wstring EscapeConnection(const std::wstring& value)
{
    std::wstring result = L"{";
    for (wchar_t c : value) { result += c; if (c == L'}') result += c; }
    return result + L"}";
}
class Database {
    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC connection = SQL_NULL_HDBC;
public:
    SQLHSTMT statement = SQL_NULL_HSTMT;
    Database(const std::wstring& ini, const wchar_t* section)
    {
        try {
        if (!Success(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env)) ||
            !Success(SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0)) ||
            !Success(SQLAllocHandle(SQL_HANDLE_DBC, env, &connection)))
            throw std::runtime_error("ODBC initialization failed");
        SQLSetConnectAttrW(connection, SQL_LOGIN_TIMEOUT, reinterpret_cast<SQLPOINTER>(5), 0);
        const auto server = Value(ini, section, L"DBServerName"), database = Value(ini, section, L"DBName"), user = Value(ini, section, L"ID");
        if (server.empty() || database.empty() || user.empty())
            throw std::runtime_error("host database settings missing; no connection attempted");
        std::wstring text = L"DRIVER={SQL Server};SERVER=" + EscapeConnection(server) +
            L";DATABASE=" + EscapeConnection(database) + L";UID=" + EscapeConnection(user) +
            L";PWD=" + EscapeConnection(Value(ini, section, L"PWD")) + L";";
        const SQLRETURN opened = SQLDriverConnectW(connection, nullptr, reinterpret_cast<SQLWCHAR*>(text.data()), SQL_NTS,
                                                   nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
        SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
        if (!Success(opened)) throw std::runtime_error("ODBC connection failed; credentials not logged");
        if (!Success(SQLAllocHandle(SQL_HANDLE_STMT, connection, &statement))) throw std::runtime_error("ODBC statement allocation failed");
        SQLSetStmtAttrW(statement, SQL_ATTR_QUERY_TIMEOUT, reinterpret_cast<SQLPOINTER>(20), 0);
        } catch (...) { Close(); throw; }
    }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database() { Close(); }
    void Close()
    {
        if (statement != SQL_NULL_HSTMT) SQLFreeHandle(SQL_HANDLE_STMT, statement);
        if (connection != SQL_NULL_HDBC) { SQLDisconnect(connection); SQLFreeHandle(SQL_HANDLE_DBC, connection); }
        if (env != SQL_NULL_HENV) SQLFreeHandle(SQL_HANDLE_ENV, env);
        statement = SQL_NULL_HSTMT; connection = SQL_NULL_HDBC; env = SQL_NULL_HENV;
    }
    void Execute(const std::wstring& text)
    {
        SQLFreeStmt(statement, SQL_CLOSE);
        const SQLRETURN result = SQLExecDirectW(statement, reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(text.c_str())), SQL_NTS);
        if (!Success(result)) {
            SQLWCHAR state[6] = {}, message[768] = {}; SQLINTEGER native = 0; SQLSMALLINT length = 0;
            SQLGetDiagRecW(SQL_HANDLE_STMT, statement, 1, state, &native, message, 768, &length);
            // Statement diagnostics contain no connection string or credentials.
            std::wstring description = L"SQL catalog statement failed: " + std::wstring(reinterpret_cast<wchar_t*>(state)) +
                                       L" / " + std::to_wstring(native) + L" / " + reinterpret_cast<wchar_t*>(message);
            std::string utf8(description.size() * 4 + 1, '\0');
            const int size = WideCharToMultiByte(CP_UTF8, 0, description.c_str(), -1, utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
            if (size > 0) utf8.resize(static_cast<size_t>(size - 1));
            throw std::runtime_error(utf8);
        }
    }
    void Complete()
    {
        for (;;) {
            const SQLRETURN result = SQLMoreResults(statement);
            if (result == SQL_NO_DATA) return;
            if (!Success(result)) throw std::runtime_error("SQL shop search batch did not complete");
        }
    }
    int Number(int column)
    {
        SQLINTEGER number = 0; SQLLEN length = 0;
        if (!Success(SQLGetData(statement, static_cast<SQLUSMALLINT>(column), SQL_C_SLONG, &number, sizeof(number), &length)) || length == SQL_NULL_DATA)
            throw std::runtime_error("invalid SQL catalog numeric result");
        return number;
    }
    std::wstring String(int column)
    {
        wchar_t text[512] = {}; SQLLEN length = 0;
        if (!Success(SQLGetData(statement, static_cast<SQLUSMALLINT>(column), SQL_C_WCHAR, text, sizeof(text), &length)) || length == SQL_NULL_DATA || length >= sizeof(text))
            throw std::runtime_error("invalid SQL Unicode label result");
        return text;
    }
    std::wstring LongString(int column)
    {
        std::wstring result;
        for (;;) {
            wchar_t text[2048] = {};
            SQLLEN length = 0;
            const SQLRETURN status = SQLGetData(
                statement, static_cast<SQLUSMALLINT>(column), SQL_C_WCHAR,
                text, sizeof(text), &length);
            if (status == SQL_NO_DATA && !result.empty()) break;
            if (!Success(status) || length == SQL_NULL_DATA)
                throw std::runtime_error("invalid SQL long Unicode result");
            const std::size_t units = wcsnlen_s(text, std::size(text));
            result.append(text, units);
            if (result.size() > 1024 * 1024)
                throw std::runtime_error("SQL definition exceeds supported size");
            if (status == SQL_SUCCESS) break;
        }
        if (result.empty()) throw std::runtime_error("empty SQL definition");
        return result;
    }
};
void Metadata(Database& game, Product& product)
{
    game.Execute(L"SELECT c.TimeLimit,ISNULL(u.UnitGradeNumber-1,0),ISNULL(u.MasteredWeaponType,0),CASE WHEN c.CatalogueID/100000=11 THEN ISNULL(u.SallyReady,0) ELSE 1 END FROM dbo.tbl_ItemCategory c LEFT JOIN dbo.tbl_Units u ON c.CatalogueID/100000=11 AND u.UnitNumber=c.RefIndex WHERE c.CatalogueID=" + std::to_wstring(product.category));
    if (SQLFetch(game.statement) != SQL_SUCCESS) throw std::runtime_error("unknown game item category");
    product.timeLimit = game.Number(1); product.search1 = game.Number(2); product.search2 = game.Number(3);
    if (game.Number(4) != 1) throw std::runtime_error("unit category is not deployable by the native game");
    if (SQLFetch(game.statement) != SQL_NO_DATA || product.timeLimit < 0 || product.timeLimit > 2)
        throw std::runtime_error("ambiguous or unsupported category metadata");
    if (product.category / 100000 == 11 && product.timeLimit != 0 && product.timeLimit != 1)
        throw std::runtime_error("unsupported unit rental mode");
    if (product.timeLimit == 1 && product.validity > 36500)
        throw std::runtime_error("rental duration exceeds supported display range");
}
StoreItemsLayout StoreItemsMetadata(Database& billing)
{
    billing.Execute(
        L"SELECT c.name,CONVERT(int,c.is_computed) "
        L"FROM sys.columns c "
        L"WHERE c.object_id=OBJECT_ID(N'dbo.tbls_StoreItems') "
        L"AND c.name IN(N'f_subitemCnt',N'f_validity',N'f_price',N'f_payment') "
        L"ORDER BY c.column_id");
    StoreItemsLayout layout;
    std::set<std::wstring> seen;
    SQLRETURN fetched = SQL_SUCCESS;
    while ((fetched = SQLFetch(billing.statement)) != SQL_NO_DATA) {
        if (!Success(fetched)) throw std::runtime_error("cannot read native shop column metadata");
        const std::wstring name = billing.String(1);
        const bool computed = billing.Number(2) != 0;
        if (!seen.insert(name).second) throw std::runtime_error("duplicate native shop column metadata");
        if (name == L"f_subitemCnt") layout.subitemCountComputed = computed;
        else if (name == L"f_validity") layout.validityComputed = computed;
        else if (name == L"f_price") layout.priceComputed = computed;
        else if (name == L"f_payment") layout.paymentComputed = computed;
        else throw std::runtime_error("unexpected native shop column metadata");
    }
    if (seen.size() != 4) throw std::runtime_error("native shop business columns missing");
    return layout;
}
void WriteUnicode(const std::wstring& path, const std::wstring& text)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot write generated Unicode profile");
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    const DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    const bool ok = WriteFile(file, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom) &&
                    WriteFile(file, text.data(), bytes, &written, nullptr) && written == bytes && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) throw std::runtime_error("incomplete generated Unicode profile");
}
void WriteUtf8(const std::wstring& path, const std::string& text)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot write shop search result");
    DWORD written = 0;
    const bool ok = text.size() <= std::numeric_limits<DWORD>::max() &&
                    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) throw std::runtime_error("incomplete shop search result");
}
std::vector<sdgo::shop_search::Alias> ParseAliases(const std::wstring& text)
{
    const auto dictionary = sdgo::shop_localization::Parse(text);
    std::vector<sdgo::shop_search::Alias> result;
    for (const auto& entry : dictionary) {
        const bool productKey = entry.first.rfind(L"@product:", 0) == 0;
        const bool searchKey = entry.first.rfind(L"@search:", 0) == 0;
        if (entry.second.kind != sdgo::shop_localization::Kind::Name || (!productKey && !searchKey))
            continue;
        const std::size_t prefix = productKey ? 9 : 8;
        const std::size_t separator = entry.first.find(L':', prefix);
        if (separator == std::wstring::npos || separator == prefix ||
            separator + 1 == entry.first.size())
            throw std::runtime_error("invalid product-scoped shop search alias");
        const std::wstring productText = entry.first.substr(prefix, separator - prefix);
        if (!std::all_of(productText.begin(), productText.end(), [](wchar_t value) {
                return value >= L'0' && value <= L'9';
            }))
            throw std::runtime_error("invalid product id in shop search alias");
        wchar_t* end = nullptr;
        const unsigned long long product = _wcstoui64(productText.c_str(), &end, 10);
        if (!end || *end || !product ||
            product > static_cast<unsigned long long>(std::numeric_limits<std::int32_t>::max()))
            throw std::runtime_error("product id outside shop search range");
        const std::wstring original = entry.first.substr(separator + 1);
        if (original.size() > 512 || original.find_first_of(L"\r\n\0") != std::wstring::npos ||
            entry.second.text.size() > 512 ||
            entry.second.text.find_first_of(L"\r\n\0") != std::wstring::npos)
            throw std::runtime_error("shop search alias exceeds 512 UTF-16 units or contains a line break");
        result.push_back({
            static_cast<std::uint32_t>(product), original, entry.second.text});
    }
    if (result.empty()) throw std::runtime_error("shop search dictionary has no product aliases");
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.product != right.product) return left.product < right.product;
        if (left.original != right.original) return left.original < right.original;
        return left.name < right.name;
    });
    std::vector<sdgo::shop_search::Alias> normalized;
    for (auto& alias : result) {
        if (!normalized.empty() && normalized.back().product == alias.product &&
            normalized.back().original == alias.original) {
            if (normalized.back().name != alias.name)
                throw std::runtime_error("conflicting shop search alias for product and original name");
            continue;
        }
        normalized.push_back(std::move(alias));
    }
    return normalized;
}
} // namespace

std::vector<sdgo::shop_search::Alias> ParseSearchAliases(
    const std::wstring& dictionaryText)
{
    return ParseAliases(dictionaryText);
}

bool Apply(const std::wstring& hostIni, const std::wstring& catalogIni,
           const std::wstring& outputDirectory, bool disable, LogFn log)
{
    bool committed = false;
    std::wstring temporary;
    try {
        const std::wstring hostFile = Absolute(hostIni);
        if (GetFileAttributesW(hostFile.c_str()) == INVALID_FILE_ATTRIBUTES)
            throw std::runtime_error("host database INI missing; no connection attempted");
        Catalog catalog = Load(Absolute(catalogIni), disable);
        if (disable) catalog.products.clear();
        const std::wstring output = Absolute(outputDirectory);
        temporary = output + L"\\runtime-display." + std::to_wstring(GetCurrentProcessId()) + L".pending";
        // Establish writability before starting a database transaction.
        WriteUnicode(temporary, L"; Preparing shop display configuration\r\n");
        if (!catalog.products.empty()) {
            Database game(hostFile, L"DB Game");
            for (auto& product : catalog.products) Metadata(game, product);
        }
        Database billing(hostFile, L"DB StoreBilling");
        try {
            const StoreItemsLayout layout = StoreItemsMetadata(billing);
            billing.Execute(ApplySql(catalog, layout));
            bool contract = false;
            std::wstring display = L"; Generated from the committed server definitions. UTF-16LE.\r\n[shop]\r\nRevision=" + catalog.revision + L"\r\n";
            unsigned count = 0;
            for (;;) {
                SQLSMALLINT columns = 0;
                if (!Success(SQLNumResultCols(billing.statement, &columns))) throw std::runtime_error("cannot read catalog result");
                if (columns == 7) {
                    if (contract) throw std::runtime_error("duplicate catalog result");
                    contract = true;
                    SQLRETURN fetched;
                    while ((fetched = SQLFetch(billing.statement)) != SQL_NO_DATA) {
                        if (!Success(fetched)) throw std::runtime_error("catalog row fetch failed");
                        const int parent = billing.Number(1);
                        const int option = billing.Number(2), category = billing.Number(3), validity = billing.Number(4);
                        const std::wstring payment = billing.String(5);
                        const int price = billing.Number(6);
                        const std::wstring label = billing.String(7);
                        const auto definition = std::find_if(catalog.products.begin(), catalog.products.end(), [parent](const Product& p) { return p.id == parent; });
                        if (definition == catalog.products.end()) throw std::runtime_error("published parent product identity missing");
                        display += L"\r\n[product:" + std::to_wstring(parent) + L"]\r\nTemplateId=" + std::to_wstring(definition->templateId) +
                            L"\r\nCategory=" + std::to_wstring(category) + L"\r\nValidity=" + std::to_wstring(validity) +
                            L"\r\nPayment=" + payment + L"\r\nPrice=" + std::to_wstring(price) + L"\r\nName=" + definition->name + L"\r\n";
                        display += L"\r\n[option:" + std::to_wstring(option) + L"]\r\nCategory=" + std::to_wstring(category) +
                            L"\r\nValidity=" + std::to_wstring(validity) + L"\r\nPayment=" + payment +
                            L"\r\nPrice=" + std::to_wstring(price) + L"\r\nText=" + label + L"\r\n";
                        ++count;
                    }
                } else if (columns != 0) throw std::runtime_error("unexpected catalog result layout");
                const SQLRETURN next = SQLMoreResults(billing.statement);
                if (next == SQL_NO_DATA) break;
                if (!Success(next)) throw std::runtime_error("catalog transaction rejected");
            }
            if (!contract) throw std::runtime_error("catalog result contract missing");
            WriteUnicode(temporary, display);
            billing.Execute(L"COMMIT TRANSACTION;");
            committed = true;
            const std::wstring destination = output + L"\\runtime-display.ini";
            if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("database applied, but publishing client display profile failed");
            log(false, L"shop catalog applied: " + catalog.id + L", " + std::to_wstring(count) + L" active managed products; native/unowned products preserved");
            return true;
        } catch (...) {
            if (!committed) {
                try { billing.Execute(L"IF @@TRANCOUNT>0 ROLLBACK TRANSACTION;"); } catch (...) {}
            }
            throw;
        }
    } catch (const std::exception& error) {
        // Rejected applications own only this process's staging file. Keep a
        // committed profile on publication failure so the host can recover it.
        if (!committed && !temporary.empty()) DeleteFileW(temporary.c_str());
        wchar_t decoded[2048] = {};
        MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, decoded, 2048);
        const std::wstring text = decoded;
        log(true, text + (committed ? L"; previous client overrides are guarded by exact business fields" : L"; transaction not committed"));
        return false;
    }
}

bool ApplySearchIndex(
    const std::wstring& hostIni, const std::wstring& dictionary,
    const std::wstring& displayProfile, const std::wstring& outputDirectory,
    LogFn log)
{
    std::wstring resultFile;
    try {
        const std::wstring hostFile = Absolute(hostIni);
        if (GetFileAttributesW(hostFile.c_str()) == INVALID_FILE_ATTRIBUTES)
            throw std::runtime_error("host database INI missing; no connection attempted");
        const std::wstring dictionaryFile = Absolute(dictionary);
        const std::wstring output = Absolute(outputDirectory);
        resultFile = output + L"\\search-index-result.json";
        DeleteFileW(resultFile.c_str());
        const auto bytes = ReadBytes(
            dictionaryFile, kMaximumSearchIndexBytes, "shop search dictionary");
        const std::wstring revision = WidenAscii(Sha256(bytes.data(), bytes.size()));
        auto aliases = ParseAliases(Utf8(bytes));
        if (!displayProfile.empty()) {
            // The profile is deliberately not a source of draft aliases. It is
            // only an optional future source after exact server-field readback.
            ReadBytes(
                Absolute(displayProfile), kMaximumSearchIndexBytes,
                "published shop display profile");
        }

        Database billing(hostFile, L"DB StoreBilling");
        billing.Execute(
            L"SELECT OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_StoreItemsList',N'P')),"
            L"CASE WHEN OBJECT_ID(N'dbo.sdgo_ShopSearchState',N'U') IS NULL THEN 0 ELSE 1 END");
        if (!Success(SQLFetch(billing.statement)))
            throw std::runtime_error("native shop search definition unavailable");
        const std::wstring currentDefinition = billing.LongString(1);
        const bool hasState = billing.Number(2) != 0;
        if (SQLFetch(billing.statement) != SQL_NO_DATA)
            throw std::runtime_error("ambiguous native shop search definition");
        if (hasState) {
            billing.Execute(
                L"SELECT InstalledDefinition FROM dbo.sdgo_ShopSearchState "
                L"WHERE ObjectName=N'dbo.sp_StoreItemsList'");
            if (!Success(SQLFetch(billing.statement)))
                throw std::runtime_error("shop search ownership state incomplete");
            const std::wstring installedDefinition = billing.LongString(1);
            if (SQLFetch(billing.statement) != SQL_NO_DATA ||
                installedDefinition != currentDefinition)
                throw std::runtime_error("owned shop search procedure has an external edit");
        } else {
            const std::string originalUtf8 = Utf8(currentDefinition);
            if (Sha256(originalUtf8.data(), originalUtf8.size()) !=
                kOriginalSearchDefinitionHash)
                throw std::runtime_error("unsupported native shop search procedure; no changes made");
        }

        // Only aliases whose exact product/name pair is currently published can
        // affect search. This drops event placeholders and stale dictionary rows
        // without changing products or importing a catalog draft.
        std::set<std::uint32_t> currentProducts;
        billing.Execute(
            L"SELECT DISTINCT f_productid FROM dbo.tbls_StoreItems "
            L"WHERE f_display='1'");
        for (;;) {
            const SQLRETURN fetched = SQLFetch(billing.statement);
            if (fetched == SQL_NO_DATA) break;
            if (!Success(fetched))
                throw std::runtime_error("cannot read current shop product ids");
            const int product = billing.Number(1);
            if (product > 0) currentProducts.insert(static_cast<std::uint32_t>(product));
        }
        aliases.erase(
            std::remove_if(aliases.begin(), aliases.end(), [&](const auto& alias) {
                return currentProducts.find(alias.product) == currentProducts.end();
            }),
            aliases.end());
        if (aliases.empty())
            throw std::runtime_error("no dictionary aliases match current published shop products");

        billing.Execute(
            sdgo::shop_search::BuildInstallSql(
                aliases, currentDefinition, revision));
        billing.Complete();
        billing.Execute(
            L"SELECT COUNT(*) FROM dbo.sdgo_ShopSearchAliases");
        if (!Success(SQLFetch(billing.statement)))
            throw std::runtime_error("shop search alias readback unavailable");
        const int installedAliases = billing.Number(1);
        if (installedAliases <= 0 ||
            SQLFetch(billing.statement) != SQL_NO_DATA)
            throw std::runtime_error("shop search alias readback invalid");

        const std::string result =
            "{\"installed\":true,\"name_count\":" +
            std::to_string(installedAliases) + ",\"revision\":\"" +
            Utf8(revision) + "\"}\n";
        WriteUtf8(resultFile, result);
        log(
            false, L"Chinese shop search installed for " +
                       std::to_wstring(installedAliases) +
                       L" current product aliases");
        return true;
    } catch (const std::exception& error) {
        if (!resultFile.empty()) DeleteFileW(resultFile.c_str());
        wchar_t decoded[2048] = {};
        MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, decoded, 2048);
        log(true, std::wstring(decoded) + L"; search index not confirmed");
        return false;
    }
}
} // namespace sdgo::shop_catalog
