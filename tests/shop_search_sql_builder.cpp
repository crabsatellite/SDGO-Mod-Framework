#include "mods/shop_catalog/search_sql.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

std::string ReadBytes(const std::string& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("cannot open input file");
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
}

void WriteBytes(const std::string& path, const std::string& value)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        throw std::runtime_error("cannot open output file");
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!stream)
        throw std::runtime_error("cannot write output file");
}

std::wstring DecodeUtf8(const std::string& input)
{
    std::wstring output;
    for (std::size_t index = 0; index < input.size();) {
        const auto first = static_cast<std::uint8_t>(input[index++]);
        std::uint32_t value = 0;
        std::size_t continuation = 0;
        if (first < 0x80) {
            value = first;
        } else if ((first & 0xE0) == 0xC0) {
            value = first & 0x1F;
            continuation = 1;
        } else if ((first & 0xF0) == 0xE0) {
            value = first & 0x0F;
            continuation = 2;
        } else if ((first & 0xF8) == 0xF0) {
            value = first & 0x07;
            continuation = 3;
        } else {
            throw std::runtime_error("invalid UTF-8 lead byte");
        }
        if (index + continuation > input.size())
            throw std::runtime_error("truncated UTF-8");
        for (std::size_t part = 0; part < continuation; ++part) {
            const auto next = static_cast<std::uint8_t>(input[index++]);
            if ((next & 0xC0) != 0x80)
                throw std::runtime_error("invalid UTF-8 continuation");
            value = (value << 6) | (next & 0x3F);
        }
        if ((continuation == 1 && value < 0x80) ||
            (continuation == 2 && value < 0x800) ||
            (continuation == 3 && value < 0x10000) ||
            value > 0x10FFFF ||
            (value >= 0xD800 && value <= 0xDFFF))
            throw std::runtime_error("invalid UTF-8 scalar");
        if constexpr (sizeof(wchar_t) == 2) {
            if (value < 0x10000) {
                output.push_back(static_cast<wchar_t>(value));
            } else {
                value -= 0x10000;
                output.push_back(
                    static_cast<wchar_t>(0xD800 + (value >> 10)));
                output.push_back(
                    static_cast<wchar_t>(0xDC00 + (value & 0x3FF)));
            }
        } else {
            output.push_back(static_cast<wchar_t>(value));
        }
    }
    return output;
}

std::string EncodeUtf8(const std::wstring& input)
{
    std::string output;
    for (std::size_t index = 0; index < input.size(); ++index) {
        std::uint32_t value = static_cast<std::uint32_t>(input[index]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (value >= 0xD800 && value <= 0xDBFF) {
                if (++index >= input.size())
                    throw std::runtime_error("truncated UTF-16");
                const auto low =
                    static_cast<std::uint32_t>(input[index]);
                if (low < 0xDC00 || low > 0xDFFF)
                    throw std::runtime_error("invalid UTF-16 pair");
                value = 0x10000 +
                    ((value - 0xD800) << 10) + (low - 0xDC00);
            } else if (value >= 0xDC00 && value <= 0xDFFF) {
                throw std::runtime_error("invalid UTF-16 low surrogate");
            }
        }
        if (value < 0x80) {
            output.push_back(static_cast<char>(value));
        } else if (value < 0x800) {
            output.push_back(static_cast<char>(0xC0 | (value >> 6)));
            output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        } else if (value < 0x10000) {
            output.push_back(static_cast<char>(0xE0 | (value >> 12)));
            output.push_back(
                static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        } else {
            output.push_back(static_cast<char>(0xF0 | (value >> 18)));
            output.push_back(
                static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
            output.push_back(
                static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
        }
    }
    return output;
}

std::vector<sdgo::shop_search::Alias> FixtureAliases()
{
    return {
        {1010101, L"alpha gundam", L"高达阿尔法"},
        {1010102, L"Beta O'Hara", L"测试机乙"},
        {
            1010103,
            L"Gamma Quote '; DROP TABLE dbo.tbls_StoreItems;--",
            L"恶意安全测试"},
        {1010104, L"Wrong Original", L"不应命中"},
        {1010104, L"Delta Unit", L"德尔塔机"},
    };
}

void Offline()
{
    using sdgo::shop_search::Alias;
    using sdgo::shop_search::BuildItemsInstallSql;
    const std::wstring expected =
        L"CREATE PROCEDURE dbo.sp_StoreItemsList AS SELECT N'O''Hara';";
    const auto sql = BuildItemsInstallSql(
        {
            {1010102, L"Beta O'Hara", L"测试机乙"},
            {1010101, L"Alpha", L"高达"},
        },
        expected, L"revision'O");

    Check(
        sql.find(L"@search4 VARCHAR(100)") != std::wstring::npos,
        "search parameter must hold the 100-byte bridge");
    Check(
        sql.find(L"DATALENGTH(@Decoded) > 72") != std::wstring::npos,
        "decoder must enforce 36 UTF-16 units");
    Check(
        sql.find(L"xs:base64Binary") != std::wstring::npos,
        "decoder must use SQL Server 2014 XML base64");
    Check(
        sql.find(L"ROW_NUMBER() OVER") != std::wstring::npos &&
            sql.find(L"@PageNm < 1") != std::wstring::npos,
        "pagination must use a stable one-based row window");
    Check(
        sql.find(L"S.f_search1 = @search1") != std::wstring::npos &&
            sql.find(L"S.f_search2 = @search2") != std::wstring::npos &&
            sql.find(L"S.f_payment = @search3") != std::wstring::npos,
        "rank, subfilter and payment must share one branch-independent filter");
    Check(
        sql.find(L"S.f_depth3 IN (2, 3)") != std::wstring::npos &&
            sql.find(L"S.f_depth1 <> 20") != std::wstring::npos &&
            sql.find(L"S.f_productID) > @ProductLow") != std::wstring::npos,
        "native hot, global and normal membership rules must remain present");
    Check(
        sql.find(L"A.OriginalName COLLATE Thai_CI_AS") !=
                std::wstring::npos &&
            sql.find(L"A.ProductId = S.f_productID") != std::wstring::npos,
        "aliases must bind product and case-insensitive original name");
    Check(
        sql.find(L"OriginalName NVARCHAR(512) COLLATE Thai_CI_AS") !=
                std::wstring::npos &&
            sql.find(L"LEFT(I.OriginalName, 50) COLLATE Thai_CI_AS") !=
                std::wstring::npos &&
            sql.find(L"S.f_display = 1") != std::wstring::npos &&
            sql.find(L"CONVERT(NVARCHAR(50), S.f_itemNm)") !=
                std::wstring::npos,
        "long dictionary originals must bind to visible canonical SQL names");
    Check(
        sql.find(
            L"ProductId INT NOT NULL,\n"
            L"            OriginalName NVARCHAR(50)") !=
            std::wstring::npos,
        "alias table columns must remain comma-separated SQL");
    Check(
        sql.find(L"Name NVARCHAR(512)") != std::wstring::npos &&
            sql.find(L"PRIMARY KEY(ProductId, OriginalName)") !=
                std::wstring::npos,
        "aliases must preserve long names and multiple source names per product");
    Check(
        sql.find(L"S.f_itemNm LIKE") != std::wstring::npos &&
            sql.find(L"CONVERT(NVARCHAR(100), @search4)") !=
                std::wstring::npos,
        "plain search must remain a parameterized LIKE expression");
    Check(
        sql.find(L"EXEC(@InstallBatch)") !=
                std::wstring::npos &&
            sql.find(
                L"STUFF(@IntendedInstalledDefinition, 1, 6, N'ALTER')") !=
                std::wstring::npos &&
            sql.find(L"CREATE PROCEDURE dbo.sp_StoreItemsList") !=
                std::wstring::npos &&
            sql.find(L"CREATE OR ALTER") == std::wstring::npos,
        "SQL Server 2014 installation must execute ALTER and store CREATE text");
    Check(
        sql.find(L"CREATE PROCEDURE dbo.sp_StoreItemsList AS SELECT "
                     L"N''O''''Hara'';") != std::wstring::npos,
        "expected definition must be embedded as a quoted Unicode literal");
    Check(
        sql.find(L"(1010101, N'Alpha', N'高达'),") <
            sql.find(L"(1010102, N'Beta O''Hara', N'测试机乙')"),
        "aliases must be deterministic and quote-safe");
    Check(
        sql.find(L"INSERT dbo.tbls_StoreItems") == std::wstring::npos &&
            sql.find(L"UPDATE dbo.tbls_StoreItems") == std::wstring::npos &&
            sql.find(L"DELETE FROM dbo.tbls_StoreItems") ==
                std::wstring::npos,
        "installer must not mutate product rows");
    Check(
        sql.find(L"BEGIN TRANSACTION") != std::wstring::npos &&
            sql.find(L"ROLLBACK TRANSACTION") != std::wstring::npos &&
            sql.find(L"COMMIT TRANSACTION") != std::wstring::npos,
        "installer must own one fail-closed transaction");
    Check(
        sql.find(L"IF NOT EXISTS(SELECT 1 FROM @CanonicalAlias)") !=
            std::wstring::npos,
        "installer must refuse an empty or fully stale alias index");

    bool rejected = false;
    try {
        BuildItemsInstallSql(
            {{1, L"A", L"甲"}, {1, L"A", L"乙"}},
            expected, L"r");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    Check(
        rejected,
        "conflicting translations for one product/source pair must be rejected");

    const auto duplicate = BuildItemsInstallSql(
        {{1, L"A", L"甲"}, {1, L"A", L"甲"}, {1, L"B", L"乙"}},
        expected, L"r");
    const auto firstDuplicate =
        duplicate.find(L"(1, N'A', N'甲')");
    Check(
        firstDuplicate != std::wstring::npos &&
            duplicate.find(L"(1, N'A', N'甲')", firstDuplicate + 1) ==
                std::wstring::npos &&
            duplicate.find(L"(1, N'B', N'乙')") != std::wstring::npos,
        "identical duplicates must collapse while alternate originals remain");

    rejected = false;
    try {
        BuildItemsInstallSql(
            {{1, std::wstring(513, L'a'), L"甲"}}, expected, L"r");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    Check(rejected, "dictionary original names beyond nvarchar(512) must be rejected");

    rejected = false;
    try {
        BuildItemsInstallSql(
            {{1, L"A", std::wstring(513, L'甲')}}, expected, L"r");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    Check(rejected, "localized names beyond nvarchar(512) must be rejected");

    rejected = false;
    try {
        BuildItemsInstallSql({}, L"", L"r");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    Check(rejected, "empty expected definitions must be rejected");
    const auto paired=sdgo::shop_search::BuildInstallSql(
        {{1010101,L"Alpha",L"高达"}},expected,L"paired",
        L"CREATE PROCEDURE dbo.sp_StoreSubItemsList AS SELECT 1;");
    Check(paired.find(L"EXEC dbo.sp_StoreItemsList\n")!=std::wstring::npos &&
          paired.find(L"ORDER BY P.sdgo_ordinal,S.f_index DESC,S.f_validity ASC")!=std::wstring::npos,
          "sales options must consume the same ordered parent result and original option order");
    Check(paired.find(L"shop sales-option procedure changed after inspection")<
              paired.find(L"DELETE FROM dbo.sdgo_ShopSearchAliases") &&
          paired.find(L"EXEC(@SubItemsInstallBatch)")<paired.find(L"COMMIT TRANSACTION"),
          "both native procedures are guarded and installed before the shared transaction commits");
    Check(paired.find(L"owned shop sales-option procedure has an external edit")!=std::wstring::npos &&
          paired.find(L"shop sales-option procedure readback mismatch")!=std::wstring::npos &&
          paired.find(L"VALUES(N'dbo.sp_StoreSubItemsList',@Revision")!=std::wstring::npos,
          "option procedure ownership, readback and revision remain separate from the parent");
    rejected=false;
    try {sdgo::shop_search::BuildInstallSql({},expected,L"paired",L"");}
    catch(const std::invalid_argument&){rejected=true;}
    Check(rejected,"paired installation refuses a missing inspected option definition");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 1) {
            Offline();
            std::cout << "shop search SQL builder checks passed\n";
            return 0;
        }
        if(argc==6 && std::string(argv[1])=="--emit-pair") {
            const auto sql=sdgo::shop_search::BuildInstallSql(FixtureAliases(),
                DecodeUtf8(ReadBytes(argv[2])),DecodeUtf8(argv[5]),DecodeUtf8(ReadBytes(argv[3])));
            WriteBytes(argv[4],EncodeUtf8(sql));return 0;
        }
        if (argc != 5 || std::string(argv[1]) != "--emit") {
            std::cerr
                << "usage: shop_search_sql_builder "
                   "--emit EXPECTED_UTF8 OUTPUT_UTF8 REVISION\n";
            return 2;
        }
        const auto expected = DecodeUtf8(ReadBytes(argv[2]));
        const auto revision = DecodeUtf8(argv[4]);
        const auto sql = sdgo::shop_search::BuildItemsInstallSql(
            FixtureAliases(), expected, revision);
        WriteBytes(argv[3], EncodeUtf8(sql));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
