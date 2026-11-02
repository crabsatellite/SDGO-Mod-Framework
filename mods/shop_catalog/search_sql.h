#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sdgo::shop_search {

struct Alias {
    std::uint32_t product;
    std::wstring original;
    std::wstring name;
};

namespace detail {

inline void ValidateText(
    const std::wstring& value, std::size_t maximum, const char* field)
{
    if (value.empty())
        throw std::invalid_argument(std::string(field) + " is empty");
    if (value.size() > maximum)
        throw std::invalid_argument(std::string(field) + " is too long");
    if (value.find(L'\0') != std::wstring::npos)
        throw std::invalid_argument(std::string(field) + " contains NUL");
}

inline std::wstring Literal(const std::wstring& value)
{
    std::wstring result = L"N'";
    result.reserve(value.size() + 3);
    for (const auto character : value) {
        result.push_back(character);
        if (character == L'\'')
            result.push_back(L'\'');
    }
    result.push_back(L'\'');
    return result;
}

inline std::wstring MaxLiteral(const std::wstring& value)
{
    if (value.empty())
        return L"CAST(N'' AS NVARCHAR(MAX))";
    std::wstring result;
    for (std::size_t offset = 0; offset < value.size();) {
        std::size_t count = std::min<std::size_t>(3900, value.size() - offset);
        if constexpr (sizeof(wchar_t) == 2) {
            const auto last =
                static_cast<std::uint32_t>(value[offset + count - 1]);
            if (last >= 0xD800 && last <= 0xDBFF)
                --count;
        }
        const auto part = Literal(value.substr(offset, count));
        if (offset == 0) {
            result += L"CAST(";
            result += part;
            result += L" AS NVARCHAR(MAX))";
        } else {
            result += L" + ";
            result += part;
        }
        offset += count;
    }
    return result;
}

inline const std::wstring& ProcedureDefinition()
{
    static const std::wstring value = LR"SQL(CREATE PROCEDURE dbo.sp_StoreItemsList
    @depth1 SMALLINT,
    @PageNm INT,
    @PageSize INT,
    @search1 SMALLINT,
    @search2 SMALLINT,
    @search3 SMALLINT,
    @search4 VARCHAR(100),
    @TotalCnt INT OUTPUT
AS
BEGIN
    SET NOCOUNT ON;
    SET XACT_ABORT ON;

    SET @TotalCnt = 0;
    IF @PageNm < 1 SET @PageNm = 1;
    IF @PageSize < 0 SET @PageSize = 0;

    DECLARE @IsChinese BIT = 0;
    DECLARE @InvalidChinese BIT = 0;
    DECLARE @ChineseQuery NVARCHAR(36) = N'';
    DECLARE @Payload VARCHAR(96);
    DECLARE @Decoded VARBINARY(MAX);

    IF LEFT(@search4, 4) = '~CN:'
    BEGIN
        SET @IsChinese = 1;
        SET @Payload = SUBSTRING(@search4, 5, 96);
        IF @Payload = '' OR LEN(@Payload) % 4 <> 0
            SET @InvalidChinese = 1;
        ELSE
        BEGIN TRY
            SET @Decoded = CAST(N'' AS XML).value(
                'xs:base64Binary(sql:variable("@Payload"))',
                'varbinary(max)');
            IF DATALENGTH(@Decoded) = 0
                OR DATALENGTH(@Decoded) % 2 <> 0
                OR DATALENGTH(@Decoded) > 72
                SET @InvalidChinese = 1;
            ELSE
                SET @ChineseQuery = CONVERT(NVARCHAR(36), @Decoded);
        END TRY
        BEGIN CATCH
            SET @InvalidChinese = 1;
            SET @ChineseQuery = N'';
        END CATCH;
    END;

    DECLARE @ProductLow BIGINT =
        (CONVERT(BIGINT, @depth1) + 100) * 10000;
    DECLARE @ProductHigh BIGINT =
        (CONVERT(BIGINT, @depth1) + 101) * 10000;
    DECLARE @FirstRow BIGINT =
        (CONVERT(BIGINT, @PageNm) - 1) * CONVERT(BIGINT, @PageSize) + 1;
    DECLARE @LastRow BIGINT =
        CONVERT(BIGINT, @PageNm) * CONVERT(BIGINT, @PageSize);

    ;WITH Filtered AS
    (
        SELECT
            S.f_productid,
            S.f_itemid,
            S.f_subitemCnt,
            S.f_depth3,
            S.f_price,
            S.f_payment,
            S.f_limitedAdition,
            S.f_quantity,
            S.f_package,
            S.f_limitedGift,
            ROW_NUMBER() OVER
            (
                ORDER BY
                    CASE WHEN @depth1 = 99 THEN S.f_productID END DESC,
                    CASE WHEN @depth1 = 99 THEN S.f_itemNm END ASC,
                    CASE WHEN @depth1 = 99 THEN S.f_regdate END DESC,
                    CASE WHEN @depth1 <> 99 THEN S.f_regdate END DESC,
                    CASE WHEN @depth1 <> 99 THEN S.f_itemNm END ASC,
                    CASE WHEN @depth1 <> 99 THEN S.f_productID END DESC
            ) AS sdgo_row
        FROM dbo.tbls_StoreItems AS S
        WHERE
            @InvalidChinese = 0
            AND S.f_display = 1
            AND
            (
                (
                    @depth1 = 99
                    AND S.f_depth3 IN (2, 3)
                )
                OR
                (
                    @depth1 > 0
                    AND @depth1 <> 99
                    AND CONVERT(BIGINT, S.f_productID) > @ProductLow
                    AND CONVERT(BIGINT, S.f_productID) < @ProductHigh
                    AND S.f_depth0 > 1
                )
                OR
                (
                    @depth1 <= 0
                    AND S.f_depth0 > 1
                    AND S.f_depth1 <> 20
                )
            )
            AND (@search1 < 0 OR S.f_search1 = @search1)
            AND (@search2 < 0 OR S.f_search2 = @search2)
            AND (@search3 < 0 OR S.f_payment = @search3)
            AND
            (
                @search4 = ''
                OR
                (
                    @IsChinese = 0
                    AND S.f_itemNm LIKE
                        N'%' + CONVERT(NVARCHAR(100), @search4) + N'%'
                )
                OR
                (
                    @IsChinese = 1
                    AND
                    (
                        CHARINDEX(
                            @ChineseQuery COLLATE Thai_CI_AS,
                            S.f_itemNm COLLATE Thai_CI_AS) > 0
                        OR EXISTS
                        (
                            SELECT 1
                            FROM dbo.sdgo_ShopSearchAliases AS A
                            WHERE A.ProductId = S.f_productID
                                AND A.OriginalName COLLATE Thai_CI_AS =
                                    S.f_itemNm COLLATE Thai_CI_AS
                                AND CHARINDEX(
                                    @ChineseQuery COLLATE Thai_CI_AS,
                                    A.Name COLLATE Thai_CI_AS) > 0
                        )
                    )
                )
            )
    )
    SELECT
        f_productid,
        f_itemid,
        f_subitemCnt,
        f_depth3,
        f_price,
        f_payment,
        f_limitedAdition,
        f_quantity,
        f_package,
        f_limitedGift,
        sdgo_row
    INTO #sdgo_shop_search_matches
    FROM Filtered;

    SELECT @TotalCnt = COUNT(*) FROM #sdgo_shop_search_matches;

    SELECT
        f_productid,
        f_itemid,
        f_subitemCnt,
        f_depth3,
        f_price,
        f_payment,
        f_limitedAdition,
        f_quantity,
        f_package,
        f_limitedGift
    FROM #sdgo_shop_search_matches
    WHERE sdgo_row BETWEEN @FirstRow AND @LastRow
    ORDER BY sdgo_row ASC;
END)SQL";
    return value;
}

} // namespace detail

inline std::wstring BuildInstallSql(
    const std::vector<Alias>& input,
    const std::wstring& expectedCurrentDefinition,
    const std::wstring& revision)
{
    detail::ValidateText(
        expectedCurrentDefinition,
        std::numeric_limits<std::size_t>::max(),
        "expectedCurrentDefinition");
    detail::ValidateText(revision, 128, "revision");

    std::vector<Alias> aliases = input;
    std::sort(
        aliases.begin(), aliases.end(),
        [](const Alias& left, const Alias& right) {
            if (left.product != right.product)
                return left.product < right.product;
            if (left.original != right.original)
                return left.original < right.original;
            return left.name < right.name;
        });
    std::vector<Alias> normalized;
    normalized.reserve(aliases.size());
    for (const auto& alias : aliases) {
        if (alias.product == 0 ||
            alias.product >
                static_cast<std::uint32_t>(
                    std::numeric_limits<std::int32_t>::max()))
            throw std::invalid_argument("alias product is outside SQL int");
        detail::ValidateText(alias.original, 512, "alias original");
        detail::ValidateText(alias.name, 512, "alias name");
        if (!normalized.empty() &&
            normalized.back().product == alias.product &&
            normalized.back().original == alias.original) {
            if (normalized.back().name != alias.name)
                throw std::invalid_argument(
                    "conflicting alias for product and original name");
            continue;
        }
        normalized.push_back(alias);
    }
    aliases = std::move(normalized);

    const auto& procedure = detail::ProcedureDefinition();
    std::wstring sql;
    sql.reserve(
        expectedCurrentDefinition.size() * 2 + procedure.size() * 2 +
        aliases.size() * 160 + 7000);
    sql += L"SET NOCOUNT ON;\nSET XACT_ABORT ON;\n";
    sql += L"DECLARE @ExpectedCurrentDefinition NVARCHAR(MAX) = ";
    sql += detail::MaxLiteral(expectedCurrentDefinition);
    sql += L";\nDECLARE @IntendedInstalledDefinition NVARCHAR(MAX) = ";
    sql += detail::MaxLiteral(procedure);
    sql += L";\nDECLARE @Revision NVARCHAR(128) = ";
    sql += detail::Literal(revision);
    sql += LR"SQL(;
DECLARE @CurrentDefinition NVARCHAR(MAX);
DECLARE @OwnedInstalledDefinition NVARCHAR(MAX);
DECLARE @OriginalDefinition NVARCHAR(MAX);
DECLARE @ActualInstalledDefinition NVARCHAR(MAX);
DECLARE @InstallBatch NVARCHAR(MAX);
DECLARE @Owner NVARCHAR(128);
DECLARE @LockResult INT;
DECLARE @CreatedAliases BIT = 0;
DECLARE @CreatedState BIT = 0;
DECLARE @CreatedBackup BIT = 0;
SET @InstallBatch =
    STUFF(@IntendedInstalledDefinition, 1, 6, N'ALTER');

BEGIN TRY
    BEGIN TRANSACTION;
    EXEC @LockResult = sys.sp_getapplock
        @Resource = N'sdgo.shop_search.install.v1',
        @LockMode = 'Exclusive',
        @LockOwner = 'Transaction',
        @LockTimeout = 5000;
    IF @LockResult < 0
    BEGIN
        ;THROW 51000, 'shop search installation lock unavailable', 1;
    END;

    IF OBJECT_ID(N'dbo.sdgo_ShopSearchAliases', N'U') IS NULL
    BEGIN
        EXEC(N'CREATE TABLE dbo.sdgo_ShopSearchAliases(
            ProductId INT NOT NULL,
            OriginalName NVARCHAR(50) COLLATE Thai_CI_AS NOT NULL,
            Name NVARCHAR(512) COLLATE Thai_CI_AS NOT NULL,
            CONSTRAINT PK_sdgo_ShopSearchAliases
                PRIMARY KEY(ProductId, OriginalName)
        )');
        SET @CreatedAliases = 1;
    END;
    IF OBJECT_ID(N'dbo.sdgo_ShopSearchState', N'U') IS NULL
    BEGIN
        EXEC(N'CREATE TABLE dbo.sdgo_ShopSearchState(
            ObjectName SYSNAME NOT NULL
                CONSTRAINT PK_sdgo_ShopSearchState PRIMARY KEY,
            Revision NVARCHAR(128) NOT NULL,
            InstalledDefinition NVARCHAR(MAX) NOT NULL,
            UpdatedAt DATETIME2(3) NOT NULL
        )');
        SET @CreatedState = 1;
    END;
    IF OBJECT_ID(N'dbo.sdgo_ShopSearchBackup', N'U') IS NULL
    BEGIN
        EXEC(N'CREATE TABLE dbo.sdgo_ShopSearchBackup(
            ObjectName SYSNAME NOT NULL
                CONSTRAINT PK_sdgo_ShopSearchBackup PRIMARY KEY,
            OriginalDefinition NVARCHAR(MAX) NOT NULL,
            CreatedAt DATETIME2(3) NOT NULL
        )');
        SET @CreatedBackup = 1;
    END;

    IF @CreatedAliases = 1
        EXEC sys.sp_addextendedproperty
            @name=N'sdgo_owner', @value=N'shop_search/v1',
            @level0type=N'SCHEMA', @level0name=N'dbo',
            @level1type=N'TABLE',
            @level1name=N'sdgo_ShopSearchAliases';
    IF @CreatedState = 1
        EXEC sys.sp_addextendedproperty
            @name=N'sdgo_owner', @value=N'shop_search/v1',
            @level0type=N'SCHEMA', @level0name=N'dbo',
            @level1type=N'TABLE',
            @level1name=N'sdgo_ShopSearchState';
    IF @CreatedBackup = 1
        EXEC sys.sp_addextendedproperty
            @name=N'sdgo_owner', @value=N'shop_search/v1',
            @level0type=N'SCHEMA', @level0name=N'dbo',
            @level1type=N'TABLE',
            @level1name=N'sdgo_ShopSearchBackup';

    SELECT @Owner = CONVERT(NVARCHAR(128), value)
    FROM sys.extended_properties
    WHERE class = 1
        AND major_id = OBJECT_ID(N'dbo.sdgo_ShopSearchAliases', N'U')
        AND minor_id = 0
        AND name = N'sdgo_owner';
    IF ISNULL(@Owner, N'') <> N'shop_search/v1'
    BEGIN
        ;THROW 51007, 'shop search alias table belongs to another owner', 1;
    END;
    SET @Owner = NULL;
    SELECT @Owner = CONVERT(NVARCHAR(128), value)
    FROM sys.extended_properties
    WHERE class = 1
        AND major_id = OBJECT_ID(N'dbo.sdgo_ShopSearchState', N'U')
        AND minor_id = 0
        AND name = N'sdgo_owner';
    IF ISNULL(@Owner, N'') <> N'shop_search/v1'
    BEGIN
        ;THROW 51008, 'shop search state table belongs to another owner', 1;
    END;
    SET @Owner = NULL;
    SELECT @Owner = CONVERT(NVARCHAR(128), value)
    FROM sys.extended_properties
    WHERE class = 1
        AND major_id = OBJECT_ID(N'dbo.sdgo_ShopSearchBackup', N'U')
        AND minor_id = 0
        AND name = N'sdgo_owner';
    IF ISNULL(@Owner, N'') <> N'shop_search/v1'
    BEGIN
        ;THROW 51009, 'shop search backup table belongs to another owner', 1;
    END;

    IF COL_LENGTH(N'dbo.sdgo_ShopSearchAliases', N'ProductId') <> 4
        OR COL_LENGTH(N'dbo.sdgo_ShopSearchAliases', N'OriginalName') <> 100
        OR COL_LENGTH(N'dbo.sdgo_ShopSearchAliases', N'Name') <> 1024
        OR COL_LENGTH(N'dbo.sdgo_ShopSearchState', N'ObjectName') <> 256
        OR COL_LENGTH(N'dbo.sdgo_ShopSearchState', N'Revision') <> 256
        OR COL_LENGTH(
            N'dbo.sdgo_ShopSearchState', N'InstalledDefinition') <> -1
        OR COL_LENGTH(
            N'dbo.sdgo_ShopSearchBackup', N'OriginalDefinition') <> -1
    BEGIN
        ;THROW 51010, 'shop search owned table layout mismatch', 1;
    END;

    SELECT @CurrentDefinition =
        OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_StoreItemsList', N'P'));
    IF @CurrentDefinition IS NULL
    BEGIN
        ;THROW 51001, 'native shop search procedure unavailable', 1;
    END;
    IF DATALENGTH(@CurrentDefinition) <>
            DATALENGTH(@ExpectedCurrentDefinition)
        OR @CurrentDefinition COLLATE Latin1_General_100_BIN2 <>
            @ExpectedCurrentDefinition COLLATE Latin1_General_100_BIN2
    BEGIN
        ;THROW 51002, 'shop search procedure changed after inspection', 1;
    END;

    SELECT @OwnedInstalledDefinition = InstalledDefinition
    FROM dbo.sdgo_ShopSearchState WITH (UPDLOCK, HOLDLOCK)
    WHERE ObjectName = N'dbo.sp_StoreItemsList';
    SELECT @OriginalDefinition = OriginalDefinition
    FROM dbo.sdgo_ShopSearchBackup WITH (UPDLOCK, HOLDLOCK)
    WHERE ObjectName = N'dbo.sp_StoreItemsList';

    IF @OwnedInstalledDefinition IS NOT NULL
    BEGIN
        IF DATALENGTH(@CurrentDefinition) <>
                DATALENGTH(@OwnedInstalledDefinition)
            OR @CurrentDefinition COLLATE Latin1_General_100_BIN2 <>
                @OwnedInstalledDefinition COLLATE Latin1_General_100_BIN2
        BEGIN
            ;THROW 51003, 'owned shop search procedure has an external edit', 1;
        END;
        IF @OriginalDefinition IS NULL
        BEGIN
            ;THROW 51004, 'owned shop search backup is missing', 1;
        END;
    END
    ELSE
    BEGIN
        IF @OriginalDefinition IS NOT NULL
        BEGIN
            ;THROW 51005, 'shop search ownership state is incomplete', 1;
        END;
        INSERT dbo.sdgo_ShopSearchBackup(
            ObjectName, OriginalDefinition, CreatedAt)
        VALUES(
            N'dbo.sp_StoreItemsList', @CurrentDefinition, SYSUTCDATETIME());
    END;

    DELETE FROM dbo.sdgo_ShopSearchAliases;
    DECLARE @AliasInput TABLE(
        ProductId INT NOT NULL,
        OriginalName NVARCHAR(512) COLLATE Thai_CI_AS NOT NULL,
        Name NVARCHAR(512) COLLATE Thai_CI_AS NOT NULL
    );
)SQL";

    for (std::size_t first = 0; first < aliases.size(); first += 1000) {
        const auto last = std::min(first + 1000, aliases.size());
        sql += L"    INSERT @AliasInput("
               L"ProductId, OriginalName, Name) VALUES\n";
        for (std::size_t index = first; index < last; ++index) {
            const auto& alias = aliases[index];
            sql += L"        (";
            sql += std::to_wstring(alias.product);
            sql += L", ";
            sql += detail::Literal(alias.original);
            sql += L", ";
            sql += detail::Literal(alias.name);
            sql += index + 1 == last ? L");\n" : L"),\n";
        }
    }

    sql += LR"SQL(    DECLARE @CanonicalAlias TABLE(
        ProductId INT NOT NULL,
        OriginalName NVARCHAR(50) COLLATE Thai_CI_AS NOT NULL,
        Name NVARCHAR(512) COLLATE Thai_CI_AS NOT NULL
    );
    INSERT @CanonicalAlias(ProductId, OriginalName, Name)
    SELECT
        I.ProductId,
        CONVERT(NVARCHAR(50), S.f_itemNm),
        I.Name
    FROM @AliasInput AS I
    JOIN dbo.tbls_StoreItems AS S
        ON S.f_productID = I.ProductId
        AND S.f_display = 1
        AND LEFT(I.OriginalName, 50) COLLATE Thai_CI_AS =
            S.f_itemNm COLLATE Thai_CI_AS;

    IF NOT EXISTS(SELECT 1 FROM @CanonicalAlias)
    BEGIN
        ;THROW 51012, 'no current visible shop products matched search aliases', 1;
    END;

    IF EXISTS(
        SELECT 1
        FROM @CanonicalAlias
        GROUP BY ProductId, OriginalName
        HAVING COUNT(DISTINCT CONVERT(
            VARCHAR(64),
            HASHBYTES(
                'SHA2_256',
                CONVERT(VARBINARY(1024), Name)),
            2)) > 1)
    BEGIN
        ;THROW 51011, 'conflicting translation for shop product source name', 1;
    END;

    INSERT dbo.sdgo_ShopSearchAliases(ProductId, OriginalName, Name)
    SELECT
        I.ProductId,
        MIN(I.OriginalName),
        MIN(I.Name)
    FROM @CanonicalAlias AS I
    GROUP BY I.ProductId, I.OriginalName;

    EXEC(@InstallBatch);

    SELECT @ActualInstalledDefinition =
        OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_StoreItemsList', N'P'));
    IF @ActualInstalledDefinition IS NULL
        OR DATALENGTH(@ActualInstalledDefinition) <>
            DATALENGTH(@IntendedInstalledDefinition)
        OR @ActualInstalledDefinition COLLATE Latin1_General_100_BIN2 <>
            @IntendedInstalledDefinition COLLATE Latin1_General_100_BIN2
    BEGIN
        ;THROW 51006, 'shop search procedure readback mismatch', 1;
    END;

    IF EXISTS(
        SELECT 1 FROM dbo.sdgo_ShopSearchState
        WHERE ObjectName = N'dbo.sp_StoreItemsList')
        UPDATE dbo.sdgo_ShopSearchState
        SET Revision = @Revision,
            InstalledDefinition = @ActualInstalledDefinition,
            UpdatedAt = SYSUTCDATETIME()
        WHERE ObjectName = N'dbo.sp_StoreItemsList';
    ELSE
        INSERT dbo.sdgo_ShopSearchState(
            ObjectName, Revision, InstalledDefinition, UpdatedAt)
        VALUES(
            N'dbo.sp_StoreItemsList', @Revision,
            @ActualInstalledDefinition, SYSUTCDATETIME());

    COMMIT TRANSACTION;
END TRY
BEGIN CATCH
    IF @@TRANCOUNT > 0 ROLLBACK TRANSACTION;
    THROW;
END CATCH;
)SQL";
    return sql;
}

} // namespace sdgo::shop_search
