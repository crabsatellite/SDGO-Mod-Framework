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
                    AND
                    (
                        S.f_itemNm LIKE N'%' + CONVERT(NVARCHAR(100), @search4) + N'%'
                        OR EXISTS
                        (
                            SELECT 1 FROM dbo.sdgo_ShopSearchAliases AS A
                            WHERE A.ProductId = S.f_productID
                                AND A.OriginalName COLLATE Thai_CI_AS = S.f_itemNm COLLATE Thai_CI_AS
                                AND A.Name LIKE N'%' + CONVERT(NVARCHAR(100), @search4) + N'%'
                        )
                    )
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

inline const std::wstring& SubProcedureDefinition()
{
    static const std::wstring value = LR"SQL(CREATE PROCEDURE dbo.sp_StoreSubItemsList
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

    CREATE TABLE #sdgo_selected_parents
    (
        sdgo_parent_order INT IDENTITY(1, 1) NOT NULL,
        f_productid INT NOT NULL,
        f_itemid VARCHAR(50) NOT NULL,
        f_subitemCnt INT NULL,
        f_depth3 SMALLINT NOT NULL,
        f_price INT NULL,
        f_payment VARCHAR(8) NULL,
        f_limitedAdition VARCHAR(8) NULL,
        f_quantity INT NOT NULL,
        f_package VARCHAR(8) NULL,
        f_limitedGift VARCHAR(8) NULL
    );

    INSERT #sdgo_selected_parents
    (
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
    )
    EXEC dbo.sp_StoreItemsList
        @depth1,
        @PageNm,
        @PageSize,
        @search1,
        @search2,
        @search3,
        @search4,
        @TotalCnt OUTPUT;

    SELECT
        S.f_productid,
        B.f_itemtblidx,
        B.f_price,
        B.f_itemNm,
        B.f_validity,
        B.f_payment,
        B.f_categoryNumber,
        S.f_package
    FROM #sdgo_selected_parents AS P
    JOIN dbo.tbls_StoreItems AS S
        ON S.f_productid = P.f_productid
    JOIN dbo.tbls_SubItems AS B
        ON B.f_itemid = S.f_itemid
    ORDER BY
        P.sdgo_parent_order ASC,
        B.f_index DESC,
        B.f_validity ASC;
END)SQL";
    return value;
}

} // namespace detail

inline std::wstring BuildInstallSql(
    const std::vector<Alias>& input,
    const std::wstring& expectedCurrentMainDefinition,
    const std::wstring& expectedCurrentSubDefinition,
    const std::wstring& revision)
{
    detail::ValidateText(
        expectedCurrentMainDefinition,
        std::numeric_limits<std::size_t>::max(),
        "expectedCurrentMainDefinition");
    detail::ValidateText(
        expectedCurrentSubDefinition,
        std::numeric_limits<std::size_t>::max(),
        "expectedCurrentSubDefinition");
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

    const auto& mainProcedure = detail::ProcedureDefinition();
    const auto& subProcedure = detail::SubProcedureDefinition();
    std::wstring sql;
    sql.reserve(
        expectedCurrentMainDefinition.size() * 2 +
        expectedCurrentSubDefinition.size() * 2 +
        mainProcedure.size() * 2 + subProcedure.size() * 2 +
        aliases.size() * 160 + 12000);
    sql += L"SET NOCOUNT ON;\nSET XACT_ABORT ON;\n";
    sql += L"DECLARE @ExpectedCurrentMainDefinition NVARCHAR(MAX) = ";
    sql += detail::MaxLiteral(expectedCurrentMainDefinition);
    sql += L";\nDECLARE @ExpectedCurrentSubDefinition NVARCHAR(MAX) = ";
    sql += detail::MaxLiteral(expectedCurrentSubDefinition);
    sql += L";\nDECLARE @IntendedInstalledMainDefinition NVARCHAR(MAX) = ";
    sql += detail::MaxLiteral(mainProcedure);
    sql += L";\nDECLARE @IntendedInstalledSubDefinition NVARCHAR(MAX) = ";
    sql += detail::MaxLiteral(subProcedure);
    sql += L";\nDECLARE @Revision NVARCHAR(128) = ";
    sql += detail::Literal(revision);
    sql += LR"SQL(;
DECLARE @CurrentMainDefinition NVARCHAR(MAX);
DECLARE @CurrentSubDefinition NVARCHAR(MAX);
DECLARE @OwnedInstalledMainDefinition NVARCHAR(MAX);
DECLARE @OwnedInstalledSubDefinition NVARCHAR(MAX);
DECLARE @OriginalMainDefinition NVARCHAR(MAX);
DECLARE @OriginalSubDefinition NVARCHAR(MAX);
DECLARE @ActualInstalledMainDefinition NVARCHAR(MAX);
DECLARE @ActualInstalledSubDefinition NVARCHAR(MAX);
DECLARE @MainInstallBatch NVARCHAR(MAX);
DECLARE @SubInstallBatch NVARCHAR(MAX);
DECLARE @AliasesOwner NVARCHAR(128);
DECLARE @StateOwner NVARCHAR(128);
DECLARE @BackupOwner NVARCHAR(128);
DECLARE @InitialOwner NVARCHAR(128);
DECLARE @LockResult INT;
DECLARE @CreatedAliases BIT = 0;
DECLARE @CreatedState BIT = 0;
DECLARE @CreatedBackup BIT = 0;
SET @MainInstallBatch =
    STUFF(@IntendedInstalledMainDefinition, 1, 6, N'ALTER');
SET @SubInstallBatch =
    STUFF(@IntendedInstalledSubDefinition, 1, 6, N'ALTER');

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
            @name=N'sdgo_owner', @value=N'shop_search/v2',
            @level0type=N'SCHEMA', @level0name=N'dbo',
            @level1type=N'TABLE',
            @level1name=N'sdgo_ShopSearchAliases';
    IF @CreatedState = 1
        EXEC sys.sp_addextendedproperty
            @name=N'sdgo_owner', @value=N'shop_search/v2',
            @level0type=N'SCHEMA', @level0name=N'dbo',
            @level1type=N'TABLE',
            @level1name=N'sdgo_ShopSearchState';
    IF @CreatedBackup = 1
        EXEC sys.sp_addextendedproperty
            @name=N'sdgo_owner', @value=N'shop_search/v2',
            @level0type=N'SCHEMA', @level0name=N'dbo',
            @level1type=N'TABLE',
            @level1name=N'sdgo_ShopSearchBackup';

    SELECT @AliasesOwner = CONVERT(NVARCHAR(128), value)
    FROM sys.extended_properties
    WHERE class = 1
        AND major_id = OBJECT_ID(N'dbo.sdgo_ShopSearchAliases', N'U')
        AND minor_id = 0
        AND name = N'sdgo_owner';
    IF ISNULL(@AliasesOwner, N'') NOT IN
            (N'shop_search/v1', N'shop_search/v2')
    BEGIN
        ;THROW 51007, 'shop search alias table belongs to another owner', 1;
    END;
    SELECT @StateOwner = CONVERT(NVARCHAR(128), value)
    FROM sys.extended_properties
    WHERE class = 1
        AND major_id = OBJECT_ID(N'dbo.sdgo_ShopSearchState', N'U')
        AND minor_id = 0
        AND name = N'sdgo_owner';
    IF ISNULL(@StateOwner, N'') NOT IN
            (N'shop_search/v1', N'shop_search/v2')
    BEGIN
        ;THROW 51008, 'shop search state table belongs to another owner', 1;
    END;
    SELECT @BackupOwner = CONVERT(NVARCHAR(128), value)
    FROM sys.extended_properties
    WHERE class = 1
        AND major_id = OBJECT_ID(N'dbo.sdgo_ShopSearchBackup', N'U')
        AND minor_id = 0
        AND name = N'sdgo_owner';
    IF ISNULL(@BackupOwner, N'') NOT IN
            (N'shop_search/v1', N'shop_search/v2')
    BEGIN
        ;THROW 51009, 'shop search backup table belongs to another owner', 1;
    END;
    IF @AliasesOwner <> @StateOwner OR @AliasesOwner <> @BackupOwner
    BEGIN
        ;THROW 51013, 'shop search owned table versions disagree', 1;
    END;
    SET @InitialOwner = @AliasesOwner;

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

)SQL";
    sql += LR"SQL(
    SELECT @CurrentMainDefinition =
        OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_StoreItemsList', N'P'));
    SELECT @CurrentSubDefinition =
        OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_StoreSubItemsList', N'P'));
    IF @CurrentMainDefinition IS NULL OR @CurrentSubDefinition IS NULL
    BEGIN
        ;THROW 51001, 'native shop search procedures unavailable', 1;
    END;
    IF DATALENGTH(@CurrentMainDefinition) <>
            DATALENGTH(@ExpectedCurrentMainDefinition)
        OR @CurrentMainDefinition COLLATE Latin1_General_100_BIN2 <>
            @ExpectedCurrentMainDefinition COLLATE Latin1_General_100_BIN2
    BEGIN
        ;THROW 51002, 'main shop search procedure changed after inspection', 1;
    END;
    IF DATALENGTH(@CurrentSubDefinition) <>
            DATALENGTH(@ExpectedCurrentSubDefinition)
        OR @CurrentSubDefinition COLLATE Latin1_General_100_BIN2 <>
            @ExpectedCurrentSubDefinition COLLATE Latin1_General_100_BIN2
    BEGIN
        ;THROW 51014, 'sub shop search procedure changed after inspection', 1;
    END;

    SELECT @OwnedInstalledMainDefinition = InstalledDefinition
    FROM dbo.sdgo_ShopSearchState WITH (UPDLOCK, HOLDLOCK)
    WHERE ObjectName = N'dbo.sp_StoreItemsList';
    SELECT @OwnedInstalledSubDefinition = InstalledDefinition
    FROM dbo.sdgo_ShopSearchState WITH (UPDLOCK, HOLDLOCK)
    WHERE ObjectName = N'dbo.sp_StoreSubItemsList';
    SELECT @OriginalMainDefinition = OriginalDefinition
    FROM dbo.sdgo_ShopSearchBackup WITH (UPDLOCK, HOLDLOCK)
    WHERE ObjectName = N'dbo.sp_StoreItemsList';
    SELECT @OriginalSubDefinition = OriginalDefinition
    FROM dbo.sdgo_ShopSearchBackup WITH (UPDLOCK, HOLDLOCK)
    WHERE ObjectName = N'dbo.sp_StoreSubItemsList';

    IF @CreatedState = 0 AND @InitialOwner = N'shop_search/v2'
        AND
        (
            @OwnedInstalledMainDefinition IS NULL
            OR @OwnedInstalledSubDefinition IS NULL
            OR @OriginalMainDefinition IS NULL
            OR @OriginalSubDefinition IS NULL
        )
    BEGIN
        ;THROW 51015, 'shop search v2 ownership state is incomplete', 1;
    END;
    IF @CreatedState = 0 AND @InitialOwner = N'shop_search/v1'
        AND @OwnedInstalledMainDefinition IS NULL
    BEGIN
        ;THROW 51005, 'shop search v1 ownership state is incomplete', 1;
    END;

    IF @OwnedInstalledMainDefinition IS NOT NULL
    BEGIN
        IF DATALENGTH(@CurrentMainDefinition) <>
                DATALENGTH(@OwnedInstalledMainDefinition)
            OR @CurrentMainDefinition COLLATE Latin1_General_100_BIN2 <>
                @OwnedInstalledMainDefinition COLLATE Latin1_General_100_BIN2
        BEGIN
            ;THROW 51003, 'owned main shop search procedure has an external edit', 1;
        END;
        IF @OriginalMainDefinition IS NULL
        BEGIN
            ;THROW 51004, 'owned main shop search backup is missing', 1;
        END;
    END
    ELSE
    BEGIN
        IF @OriginalMainDefinition IS NOT NULL
        BEGIN
            ;THROW 51005, 'main shop search ownership state is incomplete', 1;
        END;
        INSERT dbo.sdgo_ShopSearchBackup(
            ObjectName, OriginalDefinition, CreatedAt)
        VALUES(
            N'dbo.sp_StoreItemsList', @CurrentMainDefinition,
            SYSUTCDATETIME());
    END;

    IF @OwnedInstalledSubDefinition IS NOT NULL
    BEGIN
        IF DATALENGTH(@CurrentSubDefinition) <>
                DATALENGTH(@OwnedInstalledSubDefinition)
            OR @CurrentSubDefinition COLLATE Latin1_General_100_BIN2 <>
                @OwnedInstalledSubDefinition COLLATE Latin1_General_100_BIN2
        BEGIN
            ;THROW 51016, 'owned sub shop search procedure has an external edit', 1;
        END;
        IF @OriginalSubDefinition IS NULL
        BEGIN
            ;THROW 51017, 'owned sub shop search backup is missing', 1;
        END;
    END
    ELSE
    BEGIN
        IF @OriginalSubDefinition IS NOT NULL
        BEGIN
            ;THROW 51018, 'sub shop search ownership state is incomplete', 1;
        END;
        INSERT dbo.sdgo_ShopSearchBackup(
            ObjectName, OriginalDefinition, CreatedAt)
        VALUES(
            N'dbo.sp_StoreSubItemsList', @CurrentSubDefinition,
            SYSUTCDATETIME());
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

    EXEC(@MainInstallBatch);
    EXEC(@SubInstallBatch);

    SELECT @ActualInstalledMainDefinition =
        OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_StoreItemsList', N'P'));
    SELECT @ActualInstalledSubDefinition =
        OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_StoreSubItemsList', N'P'));
    IF @ActualInstalledMainDefinition IS NULL
        OR DATALENGTH(@ActualInstalledMainDefinition) <>
            DATALENGTH(@IntendedInstalledMainDefinition)
        OR @ActualInstalledMainDefinition COLLATE Latin1_General_100_BIN2 <>
            @IntendedInstalledMainDefinition COLLATE Latin1_General_100_BIN2
    BEGIN
        ;THROW 51006, 'main shop search procedure readback mismatch', 1;
    END;
    IF @ActualInstalledSubDefinition IS NULL
        OR DATALENGTH(@ActualInstalledSubDefinition) <>
            DATALENGTH(@IntendedInstalledSubDefinition)
        OR @ActualInstalledSubDefinition COLLATE Latin1_General_100_BIN2 <>
            @IntendedInstalledSubDefinition COLLATE Latin1_General_100_BIN2
    BEGIN
        ;THROW 51019, 'sub shop search procedure readback mismatch', 1;
    END;

    IF EXISTS(
        SELECT 1 FROM dbo.sdgo_ShopSearchState
        WHERE ObjectName = N'dbo.sp_StoreItemsList')
        UPDATE dbo.sdgo_ShopSearchState
        SET Revision = @Revision,
            InstalledDefinition = @ActualInstalledMainDefinition,
            UpdatedAt = SYSUTCDATETIME()
        WHERE ObjectName = N'dbo.sp_StoreItemsList';
    ELSE
        INSERT dbo.sdgo_ShopSearchState(
            ObjectName, Revision, InstalledDefinition, UpdatedAt)
        VALUES(
            N'dbo.sp_StoreItemsList', @Revision,
            @ActualInstalledMainDefinition, SYSUTCDATETIME());

    IF EXISTS(
        SELECT 1 FROM dbo.sdgo_ShopSearchState
        WHERE ObjectName = N'dbo.sp_StoreSubItemsList')
        UPDATE dbo.sdgo_ShopSearchState
        SET Revision = @Revision,
            InstalledDefinition = @ActualInstalledSubDefinition,
            UpdatedAt = SYSUTCDATETIME()
        WHERE ObjectName = N'dbo.sp_StoreSubItemsList';
    ELSE
        INSERT dbo.sdgo_ShopSearchState(
            ObjectName, Revision, InstalledDefinition, UpdatedAt)
        VALUES(
            N'dbo.sp_StoreSubItemsList', @Revision,
            @ActualInstalledSubDefinition, SYSUTCDATETIME());

    IF @InitialOwner = N'shop_search/v1'
    BEGIN
        EXEC sys.sp_updateextendedproperty
            @name=N'sdgo_owner', @value=N'shop_search/v2',
            @level0type=N'SCHEMA', @level0name=N'dbo',
            @level1type=N'TABLE',
            @level1name=N'sdgo_ShopSearchAliases';
        EXEC sys.sp_updateextendedproperty
            @name=N'sdgo_owner', @value=N'shop_search/v2',
            @level0type=N'SCHEMA', @level0name=N'dbo',
            @level1type=N'TABLE',
            @level1name=N'sdgo_ShopSearchState';
        EXEC sys.sp_updateextendedproperty
            @name=N'sdgo_owner', @value=N'shop_search/v2',
            @level0type=N'SCHEMA', @level0name=N'dbo',
            @level1type=N'TABLE',
            @level1name=N'sdgo_ShopSearchBackup';
    END;

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
