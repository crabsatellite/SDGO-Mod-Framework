#pragma once

#include "gift_policy.h"
#include "unit_payload.h"

#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sdgo::friend_unit_gift {

inline constexpr const wchar_t* kSchema = L"sdgo_friend_unit_gift";
inline constexpr const wchar_t* kOwnerMarker = L"friend_unit_gift_server/v1";
inline constexpr std::int32_t kSchemaVersion = 1;

struct SqlModule {
    std::wstring name;
    std::wstring definition;
};

inline bool ValidIdentifier(std::wstring_view value) noexcept {
    if (value.empty() || value.size() > 128) return false;
    const auto validFirst = [](wchar_t value) {
        return (value >= L'A' && value <= L'Z') ||
               (value >= L'a' && value <= L'z') || value == L'_';
    };
    const auto validRest = [&](wchar_t value) {
        return validFirst(value) ||
               (value >= L'0' && value <= L'9');
    };
    if (!validFirst(value.front())) return false;
    for (const wchar_t character : value.substr(1)) {
        if (!validRest(character)) return false;
    }
    return true;
}

inline std::wstring SqlIdentifier(std::wstring_view value) {
    if (!ValidIdentifier(value))
        throw std::runtime_error("invalid SQL identifier");
    return L"[" + std::wstring(value) + L"]";
}

inline std::wstring SqlLiteral(std::wstring_view value) {
    std::wstring result = L"N'";
    for (const wchar_t character : value) {
        if (character == L'\0')
            throw std::runtime_error("embedded SQL null");
        result += character;
        if (character == L'\'') result += L'\'';
    }
    return result + L"'";
}

inline std::uint64_t StableHash(std::wstring_view value) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    auto appendCodeUnit = [&](std::uint16_t codeUnit) {
        hash ^= static_cast<std::uint8_t>(codeUnit & 0xffu);
        hash *= 1099511628211ull;
        hash ^= static_cast<std::uint8_t>((codeUnit >> 8u) & 0xffu);
        hash *= 1099511628211ull;
    };
    for (const wchar_t character : value) {
        const auto codePoint = static_cast<std::uint32_t>(character);
        if constexpr (sizeof(wchar_t) == 2) {
            appendCodeUnit(static_cast<std::uint16_t>(codePoint));
        } else {
            if (codePoint <= 0xffffu) {
                appendCodeUnit(static_cast<std::uint16_t>(codePoint));
            } else if (codePoint <= 0x10ffffu) {
                const auto adjusted = codePoint - 0x10000u;
                appendCodeUnit(static_cast<std::uint16_t>(
                    0xd800u + (adjusted >> 10u)));
                appendCodeUnit(static_cast<std::uint16_t>(
                    0xdc00u + (adjusted & 0x3ffu)));
            }
        }
    }
    return hash;
}

inline std::wstring StableHashText(std::wstring_view value) {
    std::wostringstream output;
    output << std::hex << std::setfill(L'0') << std::setw(16)
           << StableHash(value);
    return output.str();
}

namespace detail {

inline std::wstring AcquireGiftLock() {
    return LR"SQL(
    DECLARE @GiftLockResult int;
    EXEC @GiftLockResult=sys.sp_getapplock
        @Resource=N'sdgo.friend_unit_gift.transaction.v1',
        @LockMode=N'Exclusive',@LockOwner=N'Transaction',
        @LockTimeout=10000,@DbPrincipal=N'public';
    IF @GiftLockResult<0
        RAISERROR(N'friend unit gift transaction lock unavailable',16,1);
)SQL";
}

inline std::wstring ResultSelect() {
    return LR"SQL(    SELECT @ResultCode ResultCode,@GiftId GiftId,
           @ResultState GiftState,@ResultSlot SlotNumber,
           @ResultUnit UserUnitNumber;
)SQL";
}

inline std::wstring ReceiptReplay(std::uint8_t operation,
                                  std::wstring_view argumentMatch) {
    std::wstring sql = LR"SQL(
    DECLARE @ReceiptOperation tinyint,@ReceiptRecipient int,
            @ReceiptUnit bigint,@ReceiptGift bigint,@ReceiptNote nvarchar(200),
            @ReceiptFound bit=0,@ReceiptResultCode int,
            @ReceiptResultGift bigint,@ReceiptResultState tinyint,
            @ReceiptResultSlot int,@ReceiptResultUnit bigint;
    SELECT @ReceiptFound=1,@ReceiptOperation=Operation,
           @ReceiptRecipient=ArgRecipientAccount,
           @ReceiptUnit=ArgUserUnitNumber,@ReceiptGift=ArgGiftId,
           @ReceiptNote=ArgNote,@ReceiptResultCode=ResultCode,
           @ReceiptResultGift=GiftId,@ReceiptResultState=ResultState,
           @ReceiptResultSlot=ResultSlot,@ReceiptResultUnit=ResultUnitNumber
    FROM [sdgo_friend_unit_gift].[CommandReceipt] WITH(UPDLOCK,HOLDLOCK)
    WHERE ActorAccount=@ActorAccount AND RequestId=@RequestId;
    IF @ReceiptFound=1
    BEGIN
        IF @ReceiptOperation<>)SQL";
    sql += std::to_wstring(operation);
    sql += L" OR NOT(";
    sql += argumentMatch;
    sql += LR"SQL()
        BEGIN
            COMMIT TRANSACTION;
            SELECT CONVERT(int,12) ResultCode,CONVERT(bigint,NULL) GiftId,
                   CONVERT(tinyint,0) GiftState,CONVERT(int,NULL) SlotNumber,
                   CONVERT(bigint,NULL) UserUnitNumber;
            RETURN;
        END;
        SELECT @ResultCode=@ReceiptResultCode,@GiftId=@ReceiptResultGift,
               @ResultState=@ReceiptResultState,@ResultSlot=@ReceiptResultSlot,
               @ResultUnit=@ReceiptResultUnit;
        COMMIT TRANSACTION;
)SQL";
    sql += ResultSelect();
    sql += LR"SQL(        RETURN;
    END;
)SQL";
    return sql;
}

inline std::wstring ReceiptFinish(std::uint8_t operation,
                                  std::wstring_view recipientExpression,
                                  std::wstring_view unitExpression,
                                  std::wstring_view giftExpression,
                                  std::wstring_view noteExpression,
                                  bool identityInsert) {
    std::wstring sql = L"\n gift_finish:\n"
        L"    INSERT [sdgo_friend_unit_gift].[CommandReceipt](\n"
        L"        ActorAccount,RequestId,Operation,ArgRecipientAccount,\n"
        L"        ArgUserUnitNumber,ArgGiftId,ArgNote,GiftId,ResultCode,\n"
        L"        ResultState,ResultSlot,ResultUnitNumber,CreatedAt)\n"
        L"    VALUES(@ActorAccount,@RequestId," +
        std::to_wstring(operation) + L"," +
        std::wstring(recipientExpression) + L"," +
        std::wstring(unitExpression) + L"," +
        std::wstring(giftExpression) + L"," +
        std::wstring(noteExpression) +
        L",@GiftId,@ResultCode,@ResultState,@ResultSlot,@ResultUnit,@Now);\n"
        L"    COMMIT TRANSACTION;\n";
    sql += ResultSelect();
    sql += L"END TRY\nBEGIN CATCH\n"
           L"    DECLARE @Error nvarchar(2048)=ERROR_MESSAGE();\n"
           L"    IF XACT_STATE()<>0 ROLLBACK TRANSACTION;\n";
    if (identityInsert) {
        sql += L"    IF @IdentityOn=1\n"
               L"    BEGIN TRY\n"
               L"        SET IDENTITY_INSERT dbo.tbl_UserUnits OFF;\n"
               L"    END TRY\n"
               L"    BEGIN CATCH\n"
               L"    END CATCH;\n";
    }
    sql += L"    RAISERROR(N'friend unit gift operation failed: %s',16,1,@Error);\n"
           L"END CATCH;\n"
           L"END;\n";
    return sql;
}

inline std::wstring CounterRefresh(std::wstring_view accountExpression) {
    return L"\n    UPDATE native_user\n"
        L"    SET UserUnitKindCnt=counts.KindCount,"
        L"UserUnitTotalCnt=counts.TotalCount\n"
        L"    FROM dbo.tbl_Users native_user\n"
        L"    CROSS APPLY(SELECT CONVERT(real,COUNT(DISTINCT UnitNumber)) KindCount,"
        L"CONVERT(smallint,COUNT(*)) TotalCount FROM dbo.tbl_UserUnits "
        L"WHERE AccountNumber=" + std::wstring(accountExpression) +
        L") counts\n"
        L"    WHERE native_user.AccountNumber=" +
        std::wstring(accountExpression) + L";\n"
        L"    IF @@ROWCOUNT<>1\n"
        L"        RAISERROR(N'gift inventory counter refresh failed',16,1);\n";
}

inline std::wstring OfferModule(const std::wstring& commonDatabase) {
    const auto columns = SqlColumnList();
    const auto deleted = SqlColumnList(L"DELETED");
    const auto capture = SqlColumnList(L"captured");
    const auto common = SqlIdentifier(commonDatabase);
    std::wstring sql = LR"SQL(CREATE PROCEDURE [sdgo_friend_unit_gift].[Offer]
    @ActorAccount int,
    @RequestId uniqueidentifier,
    @RecipientAccount int,
    @UserUnitNumber bigint,
    @Note nvarchar(max)
AS
BEGIN
    SET NOCOUNT ON;
    SET XACT_ABORT ON;
    IF (@@OPTIONS & 2)<>0
    BEGIN
        RAISERROR(N'friend unit gift rejects implicit transactions',16,1);
        RETURN;
    END;
    IF @@TRANCOUNT<>0
    BEGIN
        RAISERROR(N'friend unit gift rejects ambient transactions',16,1);
        RETURN;
    END;
    SET @Note=ISNULL(@Note,N'');
    IF @ActorAccount IS NULL OR @ActorAccount<=0
       OR @RecipientAccount IS NULL OR @RecipientAccount<=0
       OR @ActorAccount=@RecipientAccount OR @RequestId IS NULL
       OR @UserUnitNumber IS NULL OR @UserUnitNumber<=0
       OR DATALENGTH(@Note)>400
    BEGIN
        SELECT CONVERT(int,2) ResultCode,CONVERT(bigint,NULL) GiftId,
               CONVERT(tinyint,0) GiftState,CONVERT(int,NULL) SlotNumber,
               CONVERT(bigint,@UserUnitNumber) UserUnitNumber;
        RETURN;
    END;
    DECLARE @NoteOffset int=1,@NoteCodeUnits int=DATALENGTH(@Note)/2,
            @NoteHasNull bit=0;
    WHILE @NoteOffset<=@NoteCodeUnits
    BEGIN
        IF UNICODE(SUBSTRING(@Note,@NoteOffset,1))=0
        BEGIN
            SET @NoteHasNull=1;
            BREAK;
        END;
        SET @NoteOffset=@NoteOffset+1;
    END;
    IF @NoteHasNull=1
    BEGIN
        SELECT CONVERT(int,2) ResultCode,CONVERT(bigint,NULL) GiftId,
               CONVERT(tinyint,0) GiftState,CONVERT(int,NULL) SlotNumber,
               CONVERT(bigint,@UserUnitNumber) UserUnitNumber;
        RETURN;
    END;
    DECLARE @Now datetime2(3)=SYSUTCDATETIME(),@ResultCode int=0,
            @GiftId bigint=NULL,@ResultState tinyint=0,@ResultSlot int=NULL,
            @ResultUnit bigint=@UserUnitNumber,@Enabled bit,
            @MinimumBattles int,@MinimumKills int,@RequiredTiers tinyint,
            @MinimumDistinct smallint,@Battles int,@Kills int,@TiersMet int,
            @UnitNumber int,@OriginalSlot int,@LockState tinyint,
            @BattleShipCount smallint,@TimeLimit tinyint,@RunOut tinyint,
            @Forbidden tinyint,@DateEnd datetime,@Active bigint,@Assist bigint,
            @SenderPilot nvarchar(20),@LockedAccount int,@NativeUidCount int;
    DECLARE @Captured TABLE(
        )SQL";
    sql += PayloadVariableDefinitions();
    sql += LR"SQL(
    );
    BEGIN TRY
    BEGIN TRANSACTION;
)SQL";
    sql += AcquireGiftLock();
    sql += ReceiptReplay(
        static_cast<std::uint8_t>(Operation::offer),
        L"ISNULL(@ReceiptRecipient,-1)=@RecipientAccount "
        L"AND ISNULL(@ReceiptUnit,-1)=@UserUnitNumber "
        L"AND @ReceiptGift IS NULL "
        L"AND DATALENGTH(@ReceiptNote)=DATALENGTH(@Note) "
        L"AND ISNULL(@ReceiptNote,N'') COLLATE Latin1_General_100_BIN2="
        L"@Note COLLATE Latin1_General_100_BIN2");
    sql += LR"SQL(
    SELECT @Enabled=Enabled,@MinimumBattles=MinBattles,
           @MinimumKills=MinPlayerKills,@RequiredTiers=RequiredTiers,
           @MinimumDistinct=MinDistinctPerTier
    FROM [sdgo_friend_unit_gift].[Policy] WITH(UPDLOCK,HOLDLOCK)
    WHERE Singleton=1;
    IF @@ROWCOUNT<>1 RAISERROR(N'gift policy row unavailable',16,1);
    IF @Enabled=0 BEGIN SET @ResultCode=1; GOTO gift_finish; END;

    DECLARE @FirstAccount int=CASE WHEN @ActorAccount<@RecipientAccount
                                   THEN @ActorAccount ELSE @RecipientAccount END;
    DECLARE @SecondAccount int=CASE WHEN @ActorAccount<@RecipientAccount
                                    THEN @RecipientAccount ELSE @ActorAccount END;
    SELECT @LockedAccount=AccountNumber FROM dbo.tbl_Users WITH(UPDLOCK,HOLDLOCK)
    WHERE AccountNumber=@FirstAccount;
    IF @@ROWCOUNT<>1 BEGIN SET @ResultCode=3; GOTO gift_finish; END;
    SET @LockedAccount=NULL;
    SELECT @LockedAccount=AccountNumber FROM dbo.tbl_Users WITH(UPDLOCK,HOLDLOCK)
    WHERE AccountNumber=@SecondAccount;
    IF @@ROWCOUNT<>1 BEGIN SET @ResultCode=3; GOTO gift_finish; END;

    IF (SELECT COUNT(*) FROM )SQL";
    sql += common;
    sql += LR"SQL(.dbo.tbl_Friend WITH(UPDLOCK,HOLDLOCK)
        WHERE ((f_idx=@ActorAccount AND f_idx2=@RecipientAccount)
            OR (f_idx=@RecipientAccount AND f_idx2=@ActorAccount))
          AND f_relation=7 AND ISNULL(f_cutoff,'0')<>'1')<>2
    BEGIN SET @ResultCode=4; GOTO gift_finish; END;

    SELECT @Battles=UserBattle,@Kills=UserKill,@Active=GoUserUnitNum,
           @Assist=AssistUserUnitNum
    FROM dbo.tbl_Users WITH(UPDLOCK,HOLDLOCK)
    WHERE AccountNumber=@ActorAccount;
    SELECT @TiersMet=COUNT(*) FROM(
        SELECT unit_data.UnitGradeNumber,unit_data.UnitGradeNumber2
        FROM dbo.tbl_UserUnits owned WITH(HOLDLOCK)
        JOIN dbo.tbl_Units unit_data WITH(HOLDLOCK)
          ON unit_data.UnitNumber=owned.UnitNumber
        WHERE owned.AccountNumber=@ActorAccount
          AND unit_data.UnitGradeNumber IN(1,2,3,4)
          AND unit_data.UnitGradeNumber2 IN(2,3,4)
        GROUP BY unit_data.UnitGradeNumber,unit_data.UnitGradeNumber2
        HAVING COUNT(DISTINCT owned.UnitNumber)>=@MinimumDistinct) tiers;
    IF ISNULL(@Battles,0)<@MinimumBattles OR ISNULL(@Kills,0)<@MinimumKills
       OR ISNULL(@TiersMet,0)<@RequiredTiers
    BEGIN SET @ResultCode=5; GOTO gift_finish; END;

    SELECT @UnitNumber=UnitNumber,@OriginalSlot=UserUnitSlotNumber,
           @LockState=LockState,@BattleShipCount=BattleShipCount,
           @TimeLimit=TimeLimit,@RunOut=RunOut,@Forbidden=Forbidden,
           @DateEnd=DateEnd
    FROM dbo.tbl_UserUnits WITH(UPDLOCK,HOLDLOCK)
    WHERE AccountNumber=@ActorAccount AND UserUnitNumber=@UserUnitNumber;
    IF @@ROWCOUNT<>1 BEGIN SET @ResultCode=6; GOTO gift_finish; END;
    SELECT @NativeUidCount=COUNT(*) FROM dbo.tbl_UserUnits WITH(UPDLOCK,HOLDLOCK)
    WHERE UserUnitNumber=@UserUnitNumber;
    IF @NativeUidCount<>1
        RAISERROR(N'native unit uid is not globally unique',16,1);
    IF @LockState<>0 OR @UserUnitNumber=@Active OR @UserUnitNumber=@Assist
       OR @BattleShipCount<>0 OR @TimeLimit<>0 OR @RunOut<>0
       OR @Forbidden<>0 OR @DateEnd IS NOT NULL
       OR EXISTS(SELECT 1 FROM dbo.tbl_UserBattleShips WITH(UPDLOCK,HOLDLOCK)
          WHERE AccountNumber=@ActorAccount AND @UserUnitNumber IN(
              ISNULL(SelectPointUnit,0),ISNULL(UserUnitNumber_1,0),
              ISNULL(UserUnitNumber_2,0),ISNULL(UserUnitNumber_3,0),
              ISNULL(UserUnitNumber_4,0)))
    BEGIN SET @ResultCode=7; GOTO gift_finish; END;

    SELECT @SenderPilot=LEFT(f_pilot,20)
    FROM )SQL";
    sql += common;
    sql += LR"SQL(.dbo.tbl_Account_Pilot WITH(HOLDLOCK)
    WHERE f_idx=@ActorAccount;
    IF NULLIF(LTRIM(RTRIM(@SenderPilot)),N'') IS NULL
    BEGIN SET @ResultCode=3; GOTO gift_finish; END;

    INSERT [sdgo_friend_unit_gift].[GiftOffer](
        OfferRequestId,SenderAccount,RecipientAccount,UserUnitNumber,UnitNumber,
        OriginalSlot,SenderPilotSnapshot,Note,OfferedAt,State)
    VALUES(@RequestId,@ActorAccount,@RecipientAccount,@UserUnitNumber,@UnitNumber,
           @OriginalSlot,@SenderPilot,CONVERT(nvarchar(200),@Note),@Now,0);
    SET @GiftId=CONVERT(bigint,SCOPE_IDENTITY());

    DELETE FROM dbo.tbl_UserUnits
    OUTPUT )SQL";
    sql += deleted;
    sql += L"\n    INTO @Captured(" + columns + L")\n";
    sql += LR"SQL(    WHERE AccountNumber=@ActorAccount
      AND UserUnitNumber=@UserUnitNumber;
    IF @@ROWCOUNT<>1
        RAISERROR(N'gift escrow capture lost its locked unit',16,1);
    INSERT [sdgo_friend_unit_gift].[EscrowUnit](GiftId,)SQL";
    sql += columns;
    sql += L") SELECT @GiftId," + capture + L" FROM @Captured captured;\n";
    sql += LR"SQL(    IF @@ROWCOUNT<>1
        RAISERROR(N'gift escrow payload insert failed',16,1);
)SQL";
    sql += CounterRefresh(L"@ActorAccount");
    sql += L"    SET @ResultState=0; SET @ResultSlot=@OriginalSlot;\n";
    sql += ReceiptFinish(
        static_cast<std::uint8_t>(Operation::offer),
        L"@RecipientAccount", L"@UserUnitNumber", L"NULL",
        L"CONVERT(nvarchar(200),@Note)", false);
    return sql;
}

inline std::wstring AcceptModule() {
    const auto columns = SqlColumnList();
    const auto restore =
        RestoreProjection(L"escrow", L"@ActorAccount", L"@FreeSlot");
    std::wstring sql = LR"SQL(CREATE PROCEDURE [sdgo_friend_unit_gift].[Accept]
    @ActorAccount int,
    @RequestId uniqueidentifier,
    @GiftId bigint
AS
BEGIN
    SET NOCOUNT ON;
    SET XACT_ABORT ON;
    IF (@@OPTIONS & 2)<>0
    BEGIN
        RAISERROR(N'friend unit gift rejects implicit transactions',16,1);
        RETURN;
    END;
    IF @@TRANCOUNT<>0
    BEGIN
        RAISERROR(N'friend unit gift rejects ambient transactions',16,1);
        RETURN;
    END;
    IF @ActorAccount IS NULL OR @ActorAccount<=0 OR @RequestId IS NULL
       OR @GiftId IS NULL OR @GiftId<=0
    BEGIN
        SELECT CONVERT(int,2) ResultCode,@GiftId GiftId,
               CONVERT(tinyint,0) GiftState,CONVERT(int,NULL) SlotNumber,
               CONVERT(bigint,NULL) UserUnitNumber;
        RETURN;
    END;
    DECLARE @Now datetime2(3)=SYSUTCDATETIME(),@ResultCode int=0,
            @ResultState tinyint=0,@ResultSlot int=NULL,@ResultUnit bigint=NULL,
            @Recipient int,@State tinyint,@Expand int,@Capacity int,
            @UnitCount int,@FreeSlot int,@IdentityOn bit=0,@LockedAccount int;
    BEGIN TRY
    BEGIN TRANSACTION;
)SQL";
    sql += AcquireGiftLock();
    sql += ReceiptReplay(
        static_cast<std::uint8_t>(Operation::accept),
        L"@ReceiptRecipient IS NULL AND @ReceiptUnit IS NULL "
        L"AND ISNULL(@ReceiptGift,-1)=@GiftId AND @ReceiptNote IS NULL");
    sql += LR"SQL(
    SELECT @Recipient=RecipientAccount,@State=State,@ResultUnit=UserUnitNumber
    FROM [sdgo_friend_unit_gift].[GiftOffer] WITH(UPDLOCK,HOLDLOCK)
    WHERE GiftId=@GiftId;
    IF @@ROWCOUNT<>1 OR @ActorAccount<>@Recipient
    BEGIN SET @ResultCode=9; GOTO gift_finish; END;
    IF @State<>0
    BEGIN SET @ResultCode=10; SET @ResultState=@State; GOTO gift_finish; END;

    SELECT @LockedAccount=AccountNumber,@Expand=ExpandSlotCount
    FROM dbo.tbl_Users WITH(UPDLOCK,HOLDLOCK)
    WHERE AccountNumber=@ActorAccount;
    IF @@ROWCOUNT<>1 OR @Expand<0 OR @Expand>1176
    BEGIN SET @ResultCode=3; GOTO gift_finish; END;
    SET @Capacity=24+@Expand;
    SELECT @UnitCount=COUNT(*) FROM dbo.tbl_UserUnits WITH(UPDLOCK,HOLDLOCK)
    WHERE AccountNumber=@ActorAccount;
    IF EXISTS(SELECT 1 FROM dbo.tbl_UserUnits WITH(UPDLOCK,HOLDLOCK)
              WHERE UserUnitNumber=@ResultUnit)
        RAISERROR(N'gift unit uid already exists in native inventory',16,1);
    SELECT TOP 1 @FreeSlot=slots.UserUnitSlotNumber
    FROM dbo.tbl_UserUnitSlotNumberList slots WITH(UPDLOCK,HOLDLOCK)
    WHERE slots.UserUnitSlotNumber BETWEEN 1 AND @Capacity
      AND NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnits owned WITH(UPDLOCK,HOLDLOCK)
                     WHERE owned.AccountNumber=@ActorAccount
                       AND owned.UserUnitSlotNumber=slots.UserUnitSlotNumber)
    ORDER BY slots.UserUnitSlotNumber;
    IF @UnitCount>=@Capacity OR @FreeSlot IS NULL
    BEGIN SET @ResultCode=8; GOTO gift_finish; END;
    IF NOT EXISTS(SELECT 1 FROM [sdgo_friend_unit_gift].[EscrowUnit]
                  WITH(UPDLOCK,HOLDLOCK) WHERE GiftId=@GiftId
                    AND UserUnitNumber=@ResultUnit)
        RAISERROR(N'gift escrow payload unavailable',16,1);

    SET IDENTITY_INSERT dbo.tbl_UserUnits ON;
    SET @IdentityOn=1;
    INSERT dbo.tbl_UserUnits()SQL";
    sql += columns + L") SELECT " + restore +
           L" FROM [sdgo_friend_unit_gift].[EscrowUnit] escrow "
           L"WHERE escrow.GiftId=@GiftId;\n";
    sql += LR"SQL(    IF @@ROWCOUNT<>1
        RAISERROR(N'gift accept restore failed',16,1);
    SET IDENTITY_INSERT dbo.tbl_UserUnits OFF;
    SET @IdentityOn=0;
    DELETE [sdgo_friend_unit_gift].[EscrowUnit] WHERE GiftId=@GiftId;
    IF @@ROWCOUNT<>1
        RAISERROR(N'gift accept escrow cleanup failed',16,1);
    UPDATE [sdgo_friend_unit_gift].[GiftOffer]
    SET State=1,AcceptedAt=@Now,ResolvedAt=@Now
    WHERE GiftId=@GiftId AND State=0;
    IF @@ROWCOUNT<>1
        RAISERROR(N'gift accept state transition failed',16,1);
)SQL";
    sql += CounterRefresh(L"@ActorAccount");
    sql += L"    SET @ResultState=1; SET @ResultSlot=@FreeSlot;\n";
    sql += ReceiptFinish(
        static_cast<std::uint8_t>(Operation::accept),
        L"NULL", L"NULL", L"@GiftId", L"NULL", true);
    return sql;
}

inline std::wstring ReturnModule(bool claim) {
    const auto columns = SqlColumnList();
    const auto restore =
        RestoreProjection(L"escrow", L"@Sender", L"@FreeSlot");
    const auto name = claim ? L"ClaimReturned" : L"Return";
    const auto operation = claim ? Operation::claim_returned
                                 : Operation::return_to_sender;
    std::wstring sql = L"CREATE PROCEDURE [sdgo_friend_unit_gift].[" +
        std::wstring(name) + LR"SQL(]
    @ActorAccount int,
    @RequestId uniqueidentifier,
    @GiftId bigint
AS
BEGIN
    SET NOCOUNT ON;
    SET XACT_ABORT ON;
    IF (@@OPTIONS & 2)<>0
    BEGIN
        RAISERROR(N'friend unit gift rejects implicit transactions',16,1);
        RETURN;
    END;
    IF @@TRANCOUNT<>0
    BEGIN
        RAISERROR(N'friend unit gift rejects ambient transactions',16,1);
        RETURN;
    END;
    IF @ActorAccount IS NULL OR @ActorAccount<=0 OR @RequestId IS NULL
       OR @GiftId IS NULL OR @GiftId<=0
    BEGIN
        SELECT CONVERT(int,2) ResultCode,@GiftId GiftId,
               CONVERT(tinyint,0) GiftState,CONVERT(int,NULL) SlotNumber,
               CONVERT(bigint,NULL) UserUnitNumber;
        RETURN;
    END;
    DECLARE @Now datetime2(3)=SYSUTCDATETIME(),@ResultCode int=0,
            @ResultState tinyint=0,@ResultSlot int=NULL,@ResultUnit bigint=NULL,
            @Sender int,@Recipient int,@State tinyint,@OriginalSlot int,
            @Expand int,@Capacity int,@UnitCount int,@FreeSlot int,
            @IdentityOn bit=0,@LockedAccount int;
    BEGIN TRY
    BEGIN TRANSACTION;
)SQL";
    sql += AcquireGiftLock();
    sql += ReceiptReplay(
        static_cast<std::uint8_t>(operation),
        L"@ReceiptRecipient IS NULL AND @ReceiptUnit IS NULL "
        L"AND ISNULL(@ReceiptGift,-1)=@GiftId AND @ReceiptNote IS NULL");
    sql += LR"SQL(
    SELECT @Sender=SenderAccount,@Recipient=RecipientAccount,@State=State,
           @OriginalSlot=OriginalSlot,@ResultUnit=UserUnitNumber
    FROM [sdgo_friend_unit_gift].[GiftOffer] WITH(UPDLOCK,HOLDLOCK)
    WHERE GiftId=@GiftId;
    IF @@ROWCOUNT<>1)SQL";
    sql += claim
        ? L" OR @ActorAccount<>@Sender\n"
        : L" OR @ActorAccount<>@Recipient\n";
    sql += L"    BEGIN SET @ResultCode=9; GOTO gift_finish; END;\n";
    if (claim) {
        sql += LR"SQL(    IF @State<>2
    BEGIN SET @ResultCode=10; SET @ResultState=@State; GOTO gift_finish; END;
)SQL";
    } else {
        sql += LR"SQL(    IF @State<>0
    BEGIN SET @ResultCode=10; SET @ResultState=@State; GOTO gift_finish; END;
    UPDATE [sdgo_friend_unit_gift].[GiftOffer]
    SET State=2,ResolutionKind=1
    WHERE GiftId=@GiftId AND State=0;
    IF @@ROWCOUNT<>1
        RAISERROR(N'gift return state transition failed',16,1);
    SET @State=2;
)SQL";
    }
    sql += LR"SQL(
    SELECT @LockedAccount=AccountNumber,@Expand=ExpandSlotCount
    FROM dbo.tbl_Users WITH(UPDLOCK,HOLDLOCK)
    WHERE AccountNumber=@Sender;
    IF @@ROWCOUNT<>1 OR @Expand<0 OR @Expand>1176
    BEGIN SET @ResultCode=3; SET @ResultState=2; GOTO gift_finish; END;
    SET @Capacity=24+@Expand;
    SELECT @UnitCount=COUNT(*) FROM dbo.tbl_UserUnits WITH(UPDLOCK,HOLDLOCK)
    WHERE AccountNumber=@Sender;
    IF EXISTS(SELECT 1 FROM dbo.tbl_UserUnits WITH(UPDLOCK,HOLDLOCK)
              WHERE UserUnitNumber=@ResultUnit)
        RAISERROR(N'returned gift unit uid already exists in native inventory',16,1);
    IF @OriginalSlot BETWEEN 1 AND @Capacity
       AND NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnits WITH(UPDLOCK,HOLDLOCK)
                      WHERE AccountNumber=@Sender
                        AND UserUnitSlotNumber=@OriginalSlot)
        SET @FreeSlot=@OriginalSlot;
    ELSE
        SELECT TOP 1 @FreeSlot=slots.UserUnitSlotNumber
        FROM dbo.tbl_UserUnitSlotNumberList slots WITH(UPDLOCK,HOLDLOCK)
        WHERE slots.UserUnitSlotNumber BETWEEN 1 AND @Capacity
          AND NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnits owned WITH(UPDLOCK,HOLDLOCK)
                         WHERE owned.AccountNumber=@Sender
                           AND owned.UserUnitSlotNumber=slots.UserUnitSlotNumber)
        ORDER BY slots.UserUnitSlotNumber;
    IF @UnitCount>=@Capacity OR @FreeSlot IS NULL
    BEGIN SET @ResultCode=11; SET @ResultState=2; GOTO gift_finish; END;
    IF NOT EXISTS(SELECT 1 FROM [sdgo_friend_unit_gift].[EscrowUnit]
                  WITH(UPDLOCK,HOLDLOCK) WHERE GiftId=@GiftId
                    AND UserUnitNumber=@ResultUnit)
        RAISERROR(N'gift return escrow payload unavailable',16,1);

    SET IDENTITY_INSERT dbo.tbl_UserUnits ON;
    SET @IdentityOn=1;
    INSERT dbo.tbl_UserUnits()SQL";
    sql += columns + L") SELECT " + restore +
           L" FROM [sdgo_friend_unit_gift].[EscrowUnit] escrow "
           L"WHERE escrow.GiftId=@GiftId;\n";
    sql += LR"SQL(    IF @@ROWCOUNT<>1
        RAISERROR(N'gift return restore failed',16,1);
    SET IDENTITY_INSERT dbo.tbl_UserUnits OFF;
    SET @IdentityOn=0;
    DELETE [sdgo_friend_unit_gift].[EscrowUnit] WHERE GiftId=@GiftId;
    IF @@ROWCOUNT<>1
        RAISERROR(N'gift return escrow cleanup failed',16,1);
    UPDATE [sdgo_friend_unit_gift].[GiftOffer]
    SET State=3,ResolvedAt=@Now WHERE GiftId=@GiftId AND State=2;
    IF @@ROWCOUNT<>1
        RAISERROR(N'gift returned state transition failed',16,1);
)SQL";
    sql += CounterRefresh(L"@Sender");
    sql += L"    SET @ResultState=3; SET @ResultSlot=@FreeSlot;\n";
    sql += ReceiptFinish(
        static_cast<std::uint8_t>(operation),
        L"NULL", L"NULL", L"@GiftId", L"NULL", true);
    return sql;
}

inline std::wstring SetPolicyModule() {
    return LR"SQL(CREATE PROCEDURE [sdgo_friend_unit_gift].[SetPolicy]
    @Enabled bit,
    @MinBattles int,
    @MinPlayerKills int,
    @RequiredTiers tinyint,
    @MinDistinctPerTier smallint
AS
BEGIN
    SET NOCOUNT ON;
    SET XACT_ABORT ON;
    IF (@@OPTIONS & 2)<>0
    BEGIN
        RAISERROR(N'friend unit gift policy rejects implicit transactions',16,1);
        RETURN;
    END;
    IF @@TRANCOUNT<>0
    BEGIN
        RAISERROR(N'friend unit gift policy rejects ambient transactions',16,1);
        RETURN;
    END;
    IF @Enabled IS NULL OR @MinBattles IS NULL OR @MinPlayerKills IS NULL
       OR @RequiredTiers IS NULL OR @MinDistinctPerTier IS NULL
       OR @MinBattles<0 OR @MinPlayerKills<0 OR @RequiredTiers>12
       OR @MinDistinctPerTier<1 OR @MinDistinctPerTier>1200
    BEGIN
        RAISERROR(N'invalid friend unit gift policy',16,1);
        RETURN;
    END;
    BEGIN TRANSACTION;
    UPDATE [sdgo_friend_unit_gift].[Policy] WITH(UPDLOCK,HOLDLOCK)
    SET Enabled=@Enabled,MinBattles=@MinBattles,
        MinPlayerKills=@MinPlayerKills,RequiredTiers=@RequiredTiers,
        MinDistinctPerTier=@MinDistinctPerTier,UpdatedAt=SYSUTCDATETIME()
    WHERE Singleton=1;
    IF @@ROWCOUNT<>1
    BEGIN
        ROLLBACK TRANSACTION;
        RAISERROR(N'friend unit gift policy row unavailable',16,1);
        RETURN;
    END;
    COMMIT TRANSACTION;
END;
)SQL";
}

inline std::wstring QueryInboxModule() {
    return LR"SQL(CREATE PROCEDURE [sdgo_friend_unit_gift].[QueryInbox]
    @ActorAccount int
AS
BEGIN
    SET NOCOUNT ON;
    IF @ActorAccount IS NULL OR @ActorAccount<=0 RETURN;
    SELECT offer.GiftId,offer.UserUnitNumber,offer.UnitNumber,offer.State,
           offer.SenderAccount PeerAccount,
           offer.SenderPilotSnapshot PeerName,offer.Note,
           CONVERT(bigint,DATEDIFF(second,CONVERT(datetime,'19700101',112),
               offer.OfferedAt))*1000+DATEPART(millisecond,offer.OfferedAt)
               OfferedUnixMillis,
           escrow.UnitLevelNumber,escrow.UnitClass,escrow.UnitClassMark
    FROM [sdgo_friend_unit_gift].[GiftOffer] offer
    JOIN [sdgo_friend_unit_gift].[EscrowUnit] escrow
      ON escrow.GiftId=offer.GiftId
    WHERE offer.RecipientAccount=@ActorAccount AND offer.State=0
    ORDER BY offer.GiftId;
END;
)SQL";
}

inline std::wstring QueryReturnedModule(const std::wstring& commonDatabase) {
    const auto common = SqlIdentifier(commonDatabase);
    std::wstring sql =
        LR"SQL(CREATE PROCEDURE [sdgo_friend_unit_gift].[QueryReturned]
    @ActorAccount int
AS
BEGIN
    SET NOCOUNT ON;
    IF @ActorAccount IS NULL OR @ActorAccount<=0 RETURN;
    SELECT offer.GiftId,offer.UserUnitNumber,offer.UnitNumber,offer.State,
           offer.RecipientAccount PeerAccount,
           ISNULL(LEFT(peer.f_pilot,20),N'') PeerName,offer.Note,
           CONVERT(bigint,DATEDIFF(second,CONVERT(datetime,'19700101',112),
               offer.OfferedAt))*1000+DATEPART(millisecond,offer.OfferedAt)
               OfferedUnixMillis,
           escrow.UnitLevelNumber,
           escrow.UnitClass,escrow.UnitClassMark
    FROM [sdgo_friend_unit_gift].[GiftOffer] offer
    JOIN [sdgo_friend_unit_gift].[EscrowUnit] escrow
      ON escrow.GiftId=offer.GiftId
    LEFT JOIN )SQL";
    sql += common;
    sql += LR"SQL(.dbo.tbl_Account_Pilot peer
      ON peer.f_idx=offer.RecipientAccount
    WHERE offer.SenderAccount=@ActorAccount AND offer.State=2
    ORDER BY offer.GiftId;
END;
)SQL";
    return sql;
}

inline std::wstring QueryProvenanceModule() {
    return LR"SQL(CREATE PROCEDURE [sdgo_friend_unit_gift].[QueryProvenance]
    @ActorAccount int,
    @UserUnitNumber bigint
AS
BEGIN
    SET NOCOUNT ON;
    IF @ActorAccount IS NULL OR @ActorAccount<=0
       OR @UserUnitNumber IS NULL OR @UserUnitNumber<=0 RETURN;
    SELECT TOP 1 offer.GiftId,offer.UserUnitNumber,offer.UnitNumber,offer.State,
           offer.SenderAccount PeerAccount,
           offer.SenderPilotSnapshot PeerName,offer.Note,
           CONVERT(bigint,DATEDIFF(second,CONVERT(datetime,'19700101',112),
               offer.OfferedAt))*1000+DATEPART(millisecond,offer.OfferedAt)
               OfferedUnixMillis,
           CONVERT(bigint,DATEDIFF(second,CONVERT(datetime,'19700101',112),
               offer.AcceptedAt))*1000+DATEPART(millisecond,offer.AcceptedAt)
               AcceptedUnixMillis,
           owned.UnitLevelNumber,owned.UnitClass,owned.UnitClassMark
    FROM [sdgo_friend_unit_gift].[GiftOffer] offer
    JOIN dbo.tbl_UserUnits owned
      ON owned.AccountNumber=@ActorAccount
     AND owned.UserUnitNumber=offer.UserUnitNumber
    WHERE offer.RecipientAccount=@ActorAccount
      AND offer.UserUnitNumber=@UserUnitNumber AND offer.State=1
    ORDER BY offer.AcceptedAt DESC,offer.GiftId DESC;
END;
)SQL";
}

inline std::wstring PreviewOfferModule(const std::wstring& commonDatabase) {
    const auto common = SqlIdentifier(commonDatabase);
    std::wstring sql =
        LR"SQL(CREATE PROCEDURE [sdgo_friend_unit_gift].[PreviewOffer]
    @ActorAccount int,
    @RecipientAccount int,
    @UserUnitNumber bigint
AS
BEGIN
    SET NOCOUNT ON;
    DECLARE @ResultCode int=0,@Enabled bit,@MinimumBattles int,
            @MinimumKills int,@RequiredTiers tinyint,@MinimumDistinct smallint,
            @Battles int,@Kills int,@TiersMet int,@UnitNumber int,
            @OriginalSlot int,@LockState tinyint,@BattleShipCount smallint,
            @TimeLimit tinyint,@RunOut tinyint,@Forbidden tinyint,
            @DateEnd datetime,@Active bigint,@Assist bigint,
            @UnitLevel tinyint,@UnitClass tinyint,@ClassMark tinyint,
            @RecipientPilot nvarchar(20),@SenderPilot nvarchar(20),
            @NativeUidCount int;
    IF @ActorAccount IS NULL OR @ActorAccount<=0
       OR @RecipientAccount IS NULL OR @RecipientAccount<=0
       OR @ActorAccount=@RecipientAccount OR @UserUnitNumber IS NULL
       OR @UserUnitNumber<=0
    BEGIN
        SET @ResultCode=2;
        GOTO preview_finish;
    END;
    SELECT @Enabled=Enabled,@MinimumBattles=MinBattles,
           @MinimumKills=MinPlayerKills,@RequiredTiers=RequiredTiers,
           @MinimumDistinct=MinDistinctPerTier
    FROM [sdgo_friend_unit_gift].[Policy] WHERE Singleton=1;
    IF @@ROWCOUNT<>1 BEGIN SET @ResultCode=3; GOTO preview_finish; END;
    IF @Enabled=0 BEGIN SET @ResultCode=1; GOTO preview_finish; END;
    IF (SELECT COUNT(*) FROM dbo.tbl_Users
        WHERE AccountNumber IN(@ActorAccount,@RecipientAccount))<>2
    BEGIN SET @ResultCode=3; GOTO preview_finish; END;
    IF (SELECT COUNT(*) FROM )SQL";
    sql += common;
    sql += LR"SQL(.dbo.tbl_Friend
        WHERE ((f_idx=@ActorAccount AND f_idx2=@RecipientAccount)
            OR (f_idx=@RecipientAccount AND f_idx2=@ActorAccount))
          AND f_relation=7 AND ISNULL(f_cutoff,'0')<>'1')<>2
    BEGIN SET @ResultCode=4; GOTO preview_finish; END;
    SELECT @Battles=UserBattle,@Kills=UserKill,@Active=GoUserUnitNum,
           @Assist=AssistUserUnitNum
    FROM dbo.tbl_Users WHERE AccountNumber=@ActorAccount;
    SELECT @TiersMet=COUNT(*) FROM(
        SELECT unit_data.UnitGradeNumber,unit_data.UnitGradeNumber2
        FROM dbo.tbl_UserUnits owned
        JOIN dbo.tbl_Units unit_data ON unit_data.UnitNumber=owned.UnitNumber
        WHERE owned.AccountNumber=@ActorAccount
          AND unit_data.UnitGradeNumber IN(1,2,3,4)
          AND unit_data.UnitGradeNumber2 IN(2,3,4)
        GROUP BY unit_data.UnitGradeNumber,unit_data.UnitGradeNumber2
        HAVING COUNT(DISTINCT owned.UnitNumber)>=@MinimumDistinct) tiers;
    IF ISNULL(@Battles,0)<@MinimumBattles OR ISNULL(@Kills,0)<@MinimumKills
       OR ISNULL(@TiersMet,0)<@RequiredTiers
    BEGIN SET @ResultCode=5; GOTO preview_finish; END;
    SELECT @UnitNumber=UnitNumber,@OriginalSlot=UserUnitSlotNumber,
           @LockState=LockState,@BattleShipCount=BattleShipCount,
           @TimeLimit=TimeLimit,@RunOut=RunOut,@Forbidden=Forbidden,
           @DateEnd=DateEnd,@UnitLevel=UnitLevelNumber,@UnitClass=UnitClass,
           @ClassMark=UnitClassMark
    FROM dbo.tbl_UserUnits
    WHERE AccountNumber=@ActorAccount AND UserUnitNumber=@UserUnitNumber;
    IF @@ROWCOUNT<>1 BEGIN SET @ResultCode=6; GOTO preview_finish; END;
    SELECT @NativeUidCount=COUNT(*) FROM dbo.tbl_UserUnits
    WHERE UserUnitNumber=@UserUnitNumber;
    IF @NativeUidCount<>1 BEGIN SET @ResultCode=6; GOTO preview_finish; END;
    IF @LockState<>0 OR @UserUnitNumber=@Active OR @UserUnitNumber=@Assist
       OR @BattleShipCount<>0 OR @TimeLimit<>0 OR @RunOut<>0
       OR @Forbidden<>0 OR @DateEnd IS NOT NULL
       OR EXISTS(SELECT 1 FROM dbo.tbl_UserBattleShips
          WHERE AccountNumber=@ActorAccount AND @UserUnitNumber IN(
              ISNULL(SelectPointUnit,0),ISNULL(UserUnitNumber_1,0),
              ISNULL(UserUnitNumber_2,0),ISNULL(UserUnitNumber_3,0),
              ISNULL(UserUnitNumber_4,0)))
    BEGIN SET @ResultCode=7; GOTO preview_finish; END;
    SELECT @SenderPilot=LEFT(f_pilot,20) FROM )SQL";
    sql += common;
    sql += LR"SQL(.dbo.tbl_Account_Pilot WHERE f_idx=@ActorAccount;
    SELECT @RecipientPilot=LEFT(f_pilot,20) FROM )SQL";
    sql += common;
    sql += LR"SQL(.dbo.tbl_Account_Pilot WHERE f_idx=@RecipientAccount;
    IF NULLIF(LTRIM(RTRIM(@SenderPilot)),N'') IS NULL
       OR NULLIF(LTRIM(RTRIM(@RecipientPilot)),N'') IS NULL
    BEGIN SET @ResultCode=3; GOTO preview_finish; END;
 preview_finish:
    SELECT @ResultCode ResultCode,CONVERT(bigint,NULL) GiftId,
           CONVERT(tinyint,0) GiftState,@OriginalSlot SlotNumber,
           @UserUnitNumber UserUnitNumber,@UnitNumber UnitNumber,
           @UnitLevel UnitLevelNumber,@UnitClass UnitClass,
           @ClassMark UnitClassMark,@RecipientAccount PeerAccount,
           ISNULL(@RecipientPilot,N'') PeerName;
END;
)SQL";
    return sql;
}

inline std::wstring QueryGiftModule(const std::wstring& commonDatabase) {
    const auto common = SqlIdentifier(commonDatabase);
    std::wstring sql =
        LR"SQL(CREATE PROCEDURE [sdgo_friend_unit_gift].[QueryGift]
    @ActorAccount int,
    @GiftId bigint
AS
BEGIN
    SET NOCOUNT ON;
    IF @ActorAccount IS NULL OR @ActorAccount<=0
       OR @GiftId IS NULL OR @GiftId<=0 RETURN;
    SELECT offer.GiftId,offer.UserUnitNumber,offer.UnitNumber,offer.State,
           CASE WHEN @ActorAccount=offer.RecipientAccount
                THEN offer.SenderAccount ELSE offer.RecipientAccount END
                PeerAccount,
           CASE WHEN @ActorAccount=offer.RecipientAccount
                THEN offer.SenderPilotSnapshot COLLATE DATABASE_DEFAULT
                ELSE ISNULL(LEFT(peer.f_pilot,20),N'')
                     COLLATE DATABASE_DEFAULT END PeerName,
           offer.Note,
           CONVERT(bigint,DATEDIFF(second,CONVERT(datetime,'19700101',112),
               offer.OfferedAt))*1000+DATEPART(millisecond,offer.OfferedAt)
               OfferedUnixMillis,
           CASE WHEN offer.AcceptedAt IS NULL THEN CONVERT(bigint,0)
                ELSE CONVERT(bigint,DATEDIFF(second,
                    CONVERT(datetime,'19700101',112),offer.AcceptedAt))*1000+
                    DATEPART(millisecond,offer.AcceptedAt) END
                AcceptedUnixMillis,
           ISNULL(ISNULL(escrow.UnitLevelNumber,native_unit.UnitLevelNumber),0)
                UnitLevelNumber,
           ISNULL(ISNULL(escrow.UnitClass,native_unit.UnitClass),0) UnitClass,
           ISNULL(ISNULL(escrow.UnitClassMark,native_unit.UnitClassMark),0)
                UnitClassMark,
           CONVERT(tinyint,CASE WHEN @ActorAccount=offer.RecipientAccount
                               THEN 0 ELSE 1 END) Direction
    FROM [sdgo_friend_unit_gift].[GiftOffer] offer
    LEFT JOIN [sdgo_friend_unit_gift].[EscrowUnit] escrow
      ON escrow.GiftId=offer.GiftId
    LEFT JOIN dbo.tbl_UserUnits native_unit
      ON native_unit.UserUnitNumber=offer.UserUnitNumber
     AND native_unit.AccountNumber=CASE WHEN offer.State=1
                                       THEN offer.RecipientAccount
                                       WHEN offer.State=3
                                       THEN offer.SenderAccount ELSE -1 END
    LEFT JOIN )SQL";
    sql += common;
    sql += LR"SQL(.dbo.tbl_Account_Pilot peer
      ON peer.f_idx=offer.RecipientAccount
    WHERE offer.GiftId=@GiftId
      AND @ActorAccount IN(offer.SenderAccount,offer.RecipientAccount);
END;
)SQL";
    return sql;
}

inline std::wstring QueryReceiptModule() {
    return LR"SQL(CREATE PROCEDURE [sdgo_friend_unit_gift].[QueryReceipt]
    @ActorAccount int,
    @RequestId uniqueidentifier
AS
BEGIN
    SET NOCOUNT ON;
    IF @ActorAccount IS NULL OR @ActorAccount<=0
       OR @RequestId IS NULL RETURN;
    SELECT ResultCode,GiftId,ResultState GiftState,
           ResultSlot SlotNumber,ResultUnitNumber UserUnitNumber
    FROM [sdgo_friend_unit_gift].[CommandReceipt]
    WHERE ActorAccount=@ActorAccount AND RequestId=@RequestId;
END;
)SQL";
}

inline std::wstring ExpectedObjectGuard(
    const std::vector<SqlModule>& modules) {
    std::wstring allowed =
        L"(N'State',N'U'),(N'Policy',N'U'),(N'GiftOffer',N'U'),"
        L"(N'EscrowUnit',N'U'),(N'CommandReceipt',N'U'),"
        L"(N'ModuleState',N'U')";
    for (const auto& module : modules) {
        allowed += L",(N'" + module.name + L"',N'P')";
    }
    return LR"SQL(
 IF EXISTS(
    SELECT 1 FROM sys.objects object_data
    WHERE object_data.schema_id=SCHEMA_ID(N'sdgo_friend_unit_gift')
      AND object_data.parent_object_id=0
      AND object_data.is_ms_shipped=0
      AND NOT EXISTS(
        SELECT 1 FROM (VALUES )SQL" + allowed +
        LR"SQL() expected(Name,ObjectType)
        WHERE expected.Name=object_data.name
          AND expected.ObjectType=object_data.type))
  RAISERROR(N'friend unit gift schema contains an unowned object',16,1);
 IF EXISTS(
    SELECT 1 FROM sys.triggers trigger_data
    JOIN sys.objects parent_data
      ON parent_data.object_id=trigger_data.parent_id
    WHERE parent_data.schema_id=SCHEMA_ID(N'sdgo_friend_unit_gift')
      AND trigger_data.is_ms_shipped=0)
  RAISERROR(N'friend unit gift owned tables contain an unowned trigger',16,1);
)SQL";
}

} // namespace detail

inline std::vector<SqlModule> Modules(std::wstring_view commonDatabase) {
    if (!ValidIdentifier(commonDatabase))
        throw std::runtime_error("invalid member database identifier");
    std::vector<SqlModule> modules;
    modules.push_back({L"SetPolicy", detail::SetPolicyModule()});
    modules.push_back(
        {L"Offer", detail::OfferModule(std::wstring(commonDatabase))});
    modules.push_back({L"Accept", detail::AcceptModule()});
    modules.push_back({L"Return", detail::ReturnModule(false)});
    modules.push_back({L"ClaimReturned", detail::ReturnModule(true)});
    modules.push_back({L"QueryInbox", detail::QueryInboxModule()});
    modules.push_back(
        {L"QueryReturned",
         detail::QueryReturnedModule(std::wstring(commonDatabase))});
    modules.push_back({L"QueryProvenance", detail::QueryProvenanceModule()});
    modules.push_back(
        {L"PreviewOffer",
         detail::PreviewOfferModule(std::wstring(commonDatabase))});
    modules.push_back(
        {L"QueryGift",
         detail::QueryGiftModule(std::wstring(commonDatabase))});
    modules.push_back(
        {L"QueryReceipt", detail::QueryReceiptModule()});
    return modules;
}

inline std::wstring DefinitionContract(std::wstring_view commonDatabase) {
    std::wstring contract =
        L"friend-unit-gift-schema-v1\n" + EscrowColumnDefinitions();
    for (const auto& module : Modules(commonDatabase)) {
        contract += L"\n-- ";
        contract += module.name;
        contract += L"\n";
        contract += module.definition;
    }
    return contract;
}

inline std::wstring DefinitionHash(std::wstring_view commonDatabase) {
    return StableHashText(DefinitionContract(commonDatabase));
}

inline std::optional<std::wstring> MakeInstallSql(
    std::wstring_view expectedGameDatabase,
    std::wstring_view commonDatabase,
    const EligibilityPolicy& policy) {
    if (!ValidIdentifier(expectedGameDatabase) ||
        !ValidIdentifier(commonDatabase) || !ValidPolicy(policy) ||
        !policy.enabled) {
        return std::nullopt;
    }
    const auto modules = Modules(commonDatabase);
    const auto gameLiteral = SqlLiteral(expectedGameDatabase);
    const auto commonLiteral = SqlLiteral(commonDatabase);
    const auto owner = SqlLiteral(kOwnerMarker);
    const auto definitionHash = DefinitionHash(commonDatabase);
    std::wstring sql = LR"SQL(DECLARE @gift_saved_options int=@@OPTIONS,
        @gift_new_schema bit=0;
IF @@TRANCOUNT<>0
BEGIN
 RAISERROR(N'friend unit gift install requires no ambient transaction',16,1);
 RETURN;
END;
IF (@gift_saved_options & 2)<>0
BEGIN
 RAISERROR(N'friend unit gift install rejects implicit transactions',16,1);
 RETURN;
END;
IF DB_ID()<=4 OR DB_ID()<>)SQL";
    sql += L"DB_ID(" + gameLiteral + L")";
    sql += LR"SQL(
BEGIN
 RAISERROR(N'friend unit gift database identity mismatch',16,1);
 RETURN;
END;
IF DB_ID()SQL";
    sql += commonLiteral;
    sql += LR"SQL() IS NULL
BEGIN
 RAISERROR(N'friend unit gift common database unavailable',16,1);
 RETURN;
END;
BEGIN TRY
SET NOCOUNT ON;
SET XACT_ABORT ON;
BEGIN TRANSACTION;
DECLARE @gift_lock int;
EXEC @gift_lock=sys.sp_getapplock
 @Resource=N'sdgo.friend_unit_gift.install.v1',
 @LockMode=N'Exclusive',@LockOwner=N'Transaction',
 @LockTimeout=10000,@DbPrincipal=N'public';
IF @gift_lock<0
 RAISERROR(N'friend unit gift install lock unavailable',16,1);

IF NOT()SQL";
    sql += NativeShapePredicate(L"OBJECT_ID(N'dbo.tbl_UserUnits',N'U')");
    sql += LR"SQL()
 RAISERROR(N'native unit payload shape differs',16,1);
IF NOT EXISTS(
 SELECT 1 FROM sys.indexes index_data
 WHERE index_data.object_id=OBJECT_ID(N'dbo.tbl_UserUnits',N'U')
   AND index_data.name=N'UK_tbl_UserUnits_AccountNumber_UserUnitSlotNumber'
   AND index_data.is_unique=1
   AND (SELECT COUNT(*) FROM sys.index_columns key_data
        WHERE key_data.object_id=index_data.object_id
          AND key_data.index_id=index_data.index_id
          AND key_data.key_ordinal>0)=2
   AND EXISTS(SELECT 1 FROM sys.index_columns key_data
        JOIN sys.columns column_data
          ON column_data.object_id=key_data.object_id
         AND column_data.column_id=key_data.column_id
        WHERE key_data.object_id=index_data.object_id
          AND key_data.index_id=index_data.index_id
          AND key_data.key_ordinal=1
          AND column_data.name=N'AccountNumber')
   AND EXISTS(SELECT 1 FROM sys.index_columns key_data
        JOIN sys.columns column_data
          ON column_data.object_id=key_data.object_id
         AND column_data.column_id=key_data.column_id
        WHERE key_data.object_id=index_data.object_id
          AND key_data.index_id=index_data.index_id
          AND key_data.key_ordinal=2
          AND column_data.name=N'UserUnitSlotNumber'))
 RAISERROR(N'native unit slot uniqueness differs',16,1);
IF OBJECT_ID(N'dbo.tbl_Users',N'U') IS NULL
 OR COL_LENGTH(N'dbo.tbl_Users',N'AccountNumber')<>4
 OR COL_LENGTH(N'dbo.tbl_Users',N'UserBattle')<>4
 OR COL_LENGTH(N'dbo.tbl_Users',N'UserKill')<>4
 OR COL_LENGTH(N'dbo.tbl_Users',N'ExpandSlotCount')<>4
 OR COL_LENGTH(N'dbo.tbl_Users',N'GoUserUnitNum')<>8
 OR COL_LENGTH(N'dbo.tbl_Users',N'AssistUserUnitNum')<>8
 OR COL_LENGTH(N'dbo.tbl_Users',N'UserUnitKindCnt')<>4
 OR COL_LENGTH(N'dbo.tbl_Users',N'UserUnitTotalCnt')<>2
 RAISERROR(N'native user inventory shape differs',16,1);
IF OBJECT_ID(N'dbo.tbl_Units',N'U') IS NULL
 OR COL_LENGTH(N'dbo.tbl_Units',N'UnitNumber')<>4
 OR COL_LENGTH(N'dbo.tbl_Units',N'UnitGradeNumber')<>1
 OR COL_LENGTH(N'dbo.tbl_Units',N'UnitGradeNumber2')<>1
 RAISERROR(N'native unit catalog shape differs',16,1);
IF OBJECT_ID(N'dbo.tbl_UserBattleShips',N'U') IS NULL
 OR COL_LENGTH(N'dbo.tbl_UserBattleShips',N'AccountNumber')<>4
 OR COL_LENGTH(N'dbo.tbl_UserBattleShips',N'SelectPointUnit')<>8
 OR COL_LENGTH(N'dbo.tbl_UserBattleShips',N'UserUnitNumber_1')<>8
 OR COL_LENGTH(N'dbo.tbl_UserBattleShips',N'UserUnitNumber_2')<>8
 OR COL_LENGTH(N'dbo.tbl_UserBattleShips',N'UserUnitNumber_3')<>8
 OR COL_LENGTH(N'dbo.tbl_UserBattleShips',N'UserUnitNumber_4')<>8
 RAISERROR(N'native saved ship shape differs',16,1);
IF OBJECT_ID(N'dbo.tbl_UserUnitSlotNumberList',N'U') IS NULL
 OR COL_LENGTH(N'dbo.tbl_UserUnitSlotNumberList',N'UserUnitSlotNumber')<>4
 RAISERROR(N'native unit slot pool differs',16,1);
IF OBJECT_ID()SQL";
    const auto friendTable =
        SqlLiteral(std::wstring(commonDatabase) + L".dbo.tbl_Friend");
    const auto pilotTable =
        SqlLiteral(std::wstring(commonDatabase) + L".dbo.tbl_Account_Pilot");
    sql += friendTable;
    sql += LR"SQL(,N'U') IS NULL
 OR COL_LENGTH()SQL";
    sql += friendTable;
    sql += LR"SQL(,N'f_idx')<>4
 OR COL_LENGTH()SQL";
    sql += friendTable;
    sql += LR"SQL(,N'f_idx2')<>4
 OR COL_LENGTH()SQL";
    sql += friendTable;
    sql += LR"SQL(,N'f_relation')<>4
 OR COL_LENGTH()SQL";
    sql += friendTable;
    sql += LR"SQL(,N'f_cutoff')<>1
 RAISERROR(N'native friendship shape differs',16,1);
IF OBJECT_ID()SQL";
    sql += pilotTable;
    sql += LR"SQL(,N'U') IS NULL
 OR COL_LENGTH()SQL";
    sql += pilotTable;
    sql += LR"SQL(,N'f_idx')<>4
 OR COL_LENGTH()SQL";
    sql += pilotTable;
    sql += LR"SQL(,N'f_pilot')<>40
 RAISERROR(N'native pilot shape differs',16,1);

IF SCHEMA_ID(N'sdgo_friend_unit_gift') IS NULL
BEGIN
 EXEC(N'CREATE SCHEMA [sdgo_friend_unit_gift] AUTHORIZATION dbo');
 EXEC sys.sp_addextendedproperty @name=N'sdgo_owner',@value=)SQL";
    sql += owner;
    sql += LR"SQL(,@level0type=N'SCHEMA',
      @level0name=N'sdgo_friend_unit_gift';
 SET @gift_new_schema=1;
END;
IF ISNULL(CONVERT(nvarchar(128),(SELECT value FROM sys.extended_properties
 WHERE class=3 AND major_id=SCHEMA_ID(N'sdgo_friend_unit_gift')
 AND name=N'sdgo_owner')),N'')<>)SQL";
    sql += owner;
    sql += LR"SQL(
 RAISERROR(N'friend unit gift schema belongs to another owner',16,1);

IF @gift_new_schema=1
BEGIN
 CREATE TABLE [sdgo_friend_unit_gift].[State](
  Singleton tinyint NOT NULL CONSTRAINT PK_gift_State PRIMARY KEY,
  OwnerMarker nvarchar(64) NOT NULL,SchemaVersion int NOT NULL,
  DefinitionHash char(16) NOT NULL,InstalledAt datetime2(3) NOT NULL,
  UpdatedAt datetime2(3) NOT NULL,
  CONSTRAINT CK_gift_State_singleton CHECK(Singleton=1));
 CREATE TABLE [sdgo_friend_unit_gift].[Policy](
  Singleton tinyint NOT NULL CONSTRAINT PK_gift_Policy PRIMARY KEY,
  Enabled bit NOT NULL,MinBattles int NOT NULL,MinPlayerKills int NOT NULL,
  RequiredTiers tinyint NOT NULL,MinDistinctPerTier smallint NOT NULL,
  UpdatedAt datetime2(3) NOT NULL,
  CONSTRAINT CK_gift_Policy_singleton CHECK(Singleton=1),
  CONSTRAINT CK_gift_Policy_values CHECK(MinBattles>=0 AND MinPlayerKills>=0
   AND RequiredTiers<=12 AND MinDistinctPerTier BETWEEN 1 AND 1200));
 CREATE TABLE [sdgo_friend_unit_gift].[GiftOffer](
  GiftId bigint IDENTITY(1,1) NOT NULL CONSTRAINT PK_gift_Offer PRIMARY KEY,
  OfferRequestId uniqueidentifier NOT NULL,SenderAccount int NOT NULL,
  RecipientAccount int NOT NULL,UserUnitNumber bigint NOT NULL,
  UnitNumber int NOT NULL,OriginalSlot int NOT NULL,
  SenderPilotSnapshot nvarchar(20) NOT NULL,Note nvarchar(200) NOT NULL,
  OfferedAt datetime2(3) NOT NULL,State tinyint NOT NULL,
  ResolutionKind tinyint NULL,AcceptedAt datetime2(3) NULL,
  ResolvedAt datetime2(3) NULL,Version rowversion NOT NULL,
  CONSTRAINT CK_gift_Offer_accounts CHECK(SenderAccount>0 AND RecipientAccount>0
   AND SenderAccount<>RecipientAccount),
  CONSTRAINT CK_gift_Offer_state CHECK(State BETWEEN 0 AND 3),
  CONSTRAINT CK_gift_Offer_resolution CHECK(
   (State IN(0,1) AND ResolutionKind IS NULL)
   OR (State IN(2,3) AND ResolutionKind=1)),
  CONSTRAINT UQ_gift_Offer_request UNIQUE(SenderAccount,OfferRequestId));
 CREATE INDEX IX_gift_Offer_recipient ON [sdgo_friend_unit_gift].[GiftOffer]
  (RecipientAccount,State,GiftId);
 CREATE INDEX IX_gift_Offer_sender ON [sdgo_friend_unit_gift].[GiftOffer]
  (SenderAccount,State,GiftId);
 CREATE INDEX IX_gift_Offer_provenance ON [sdgo_friend_unit_gift].[GiftOffer]
  (RecipientAccount,UserUnitNumber,State,AcceptedAt);
 CREATE TABLE [sdgo_friend_unit_gift].[EscrowUnit](
  GiftId bigint NOT NULL CONSTRAINT PK_gift_Escrow PRIMARY KEY)SQL";
    sql += EscrowColumnDefinitions();
    sql += LR"SQL(,
  CONSTRAINT UQ_gift_Escrow_uid UNIQUE(UserUnitNumber),
  CONSTRAINT FK_gift_Escrow_offer FOREIGN KEY(GiftId)
   REFERENCES [sdgo_friend_unit_gift].[GiftOffer](GiftId));
 CREATE TABLE [sdgo_friend_unit_gift].[CommandReceipt](
  ActorAccount int NOT NULL,RequestId uniqueidentifier NOT NULL,
  Operation tinyint NOT NULL,ArgRecipientAccount int NULL,
  ArgUserUnitNumber bigint NULL,ArgGiftId bigint NULL,
  ArgNote nvarchar(200) NULL,GiftId bigint NULL,ResultCode int NOT NULL,
  ResultState tinyint NOT NULL,ResultSlot int NULL,ResultUnitNumber bigint NULL,
  CreatedAt datetime2(3) NOT NULL,
  CONSTRAINT PK_gift_Receipt PRIMARY KEY(ActorAccount,RequestId),
  CONSTRAINT CK_gift_Receipt_actor CHECK(ActorAccount>0),
  CONSTRAINT CK_gift_Receipt_operation CHECK(Operation BETWEEN 1 AND 4));
 CREATE TABLE [sdgo_friend_unit_gift].[ModuleState](
  ObjectName sysname NOT NULL CONSTRAINT PK_gift_ModuleState PRIMARY KEY,
  DefinitionHash char(16) NOT NULL,Definition nvarchar(max) NOT NULL);
END
ELSE
BEGIN
)SQL";
    sql += detail::ExpectedObjectGuard(modules);
    sql += LR"SQL(
 IF OBJECT_ID(N'[sdgo_friend_unit_gift].[State]',N'U') IS NULL
  OR (SELECT COUNT(*) FROM sys.columns
      WHERE object_id=OBJECT_ID(N'[sdgo_friend_unit_gift].[State]'))<>6
  OR COL_LENGTH(N'[sdgo_friend_unit_gift].[State]',N'OwnerMarker')<>128
  OR COL_LENGTH(N'[sdgo_friend_unit_gift].[State]',N'DefinitionHash')<>16
  RAISERROR(N'friend unit gift state shape differs',16,1);
 IF OBJECT_ID(N'[sdgo_friend_unit_gift].[Policy]',N'U') IS NULL
  OR (SELECT COUNT(*) FROM sys.columns
      WHERE object_id=OBJECT_ID(N'[sdgo_friend_unit_gift].[Policy]'))<>7
  RAISERROR(N'friend unit gift policy shape differs',16,1);
 IF OBJECT_ID(N'[sdgo_friend_unit_gift].[GiftOffer]',N'U') IS NULL
  OR (SELECT COUNT(*) FROM sys.columns
      WHERE object_id=OBJECT_ID(N'[sdgo_friend_unit_gift].[GiftOffer]'))<>15
  RAISERROR(N'friend unit gift offer shape differs',16,1);
 IF OBJECT_ID(N'[sdgo_friend_unit_gift].[CommandReceipt]',N'U') IS NULL
  OR (SELECT COUNT(*) FROM sys.columns
      WHERE object_id=OBJECT_ID(N'[sdgo_friend_unit_gift].[CommandReceipt]'))<>13
  RAISERROR(N'friend unit gift receipt shape differs',16,1);
 IF OBJECT_ID(N'[sdgo_friend_unit_gift].[ModuleState]',N'U') IS NULL
  OR (SELECT COUNT(*) FROM sys.columns
      WHERE object_id=OBJECT_ID(N'[sdgo_friend_unit_gift].[ModuleState]'))<>3
  RAISERROR(N'friend unit gift module state shape differs',16,1);
 IF OBJECT_ID(N'[sdgo_friend_unit_gift].[EscrowUnit]',N'U') IS NULL
  OR NOT()SQL";
    sql += EscrowShapePredicate(
        L"OBJECT_ID(N'[sdgo_friend_unit_gift].[EscrowUnit]',N'U')");
    sql += LR"SQL()
  RAISERROR(N'friend unit gift escrow payload shape differs',16,1);
 IF NOT EXISTS(SELECT 1 FROM [sdgo_friend_unit_gift].[State]
  WHERE Singleton=1 AND OwnerMarker=N'friend_unit_gift_server/v1'
    AND SchemaVersion=1 AND DefinitionHash=')SQL";
    sql += definitionHash;
    sql += LR"SQL(')
  RAISERROR(N'friend unit gift owned state differs',16,1);
)SQL";
    for (const auto& module : modules) {
        const auto expected = SqlLiteral(module.definition);
        sql += L" IF OBJECT_ID(N'[sdgo_friend_unit_gift].[" + module.name +
               L"]',N'P') IS NULL\n"
               L"  RAISERROR(N'friend unit gift module missing',16,1);\n"
               L" IF NOT EXISTS(SELECT 1 FROM "
               L"[sdgo_friend_unit_gift].[ModuleState]\n"
               L"  WHERE ObjectName=N'" + module.name +
               L"' AND DefinitionHash='" + StableHashText(module.definition) +
               L"' AND DATALENGTH(Definition)=DATALENGTH(" + expected +
               L") AND Definition COLLATE Latin1_General_100_BIN2=" + expected +
               L" COLLATE Latin1_General_100_BIN2)\n"
               L"  RAISERROR(N'friend unit gift stored module differs',16,1);\n"
               L" IF DATALENGTH(OBJECT_DEFINITION(OBJECT_ID("
               L"N'[sdgo_friend_unit_gift].[" + module.name +
               L"]',N'P')))<>DATALENGTH(" + expected + L")\n"
               L"  OR OBJECT_DEFINITION(OBJECT_ID("
               L"N'[sdgo_friend_unit_gift].[" + module.name +
               L"]',N'P')) COLLATE Latin1_General_100_BIN2<>" + expected +
               L" COLLATE Latin1_General_100_BIN2\n"
               L"  RAISERROR(N'friend unit gift installed module was edited',16,1);\n";
    }
    sql += L" IF (SELECT COUNT(*) FROM [sdgo_friend_unit_gift].[ModuleState])<>" +
           std::to_wstring(modules.size()) +
           L"\n  RAISERROR(N'friend unit gift module inventory differs',16,1);\n"
           L"END;\n";
    for (const auto& module : modules) {
        sql += L"\nIF @gift_new_schema=1\nBEGIN\n"
               L" EXEC(" + SqlLiteral(module.definition) + L");\n"
               L" INSERT [sdgo_friend_unit_gift].[ModuleState]"
               L"(ObjectName,DefinitionHash,Definition)\n"
               L" VALUES(N'" + module.name + L"','" +
               StableHashText(module.definition) + L"'," +
               SqlLiteral(module.definition) + L");\n"
               L"END;\n";
    }
    sql += L"\nIF @gift_new_schema=1\n"
        L"BEGIN\n"
        L" INSERT [sdgo_friend_unit_gift].[State]"
        L"(Singleton,OwnerMarker,SchemaVersion,DefinitionHash,InstalledAt,UpdatedAt)"
        L" VALUES(1,N'friend_unit_gift_server/v1',1,'" + definitionHash +
        L"',SYSUTCDATETIME(),SYSUTCDATETIME());\n"
        L" INSERT [sdgo_friend_unit_gift].[Policy]"
        L"(Singleton,Enabled,MinBattles,MinPlayerKills,RequiredTiers,"
        L"MinDistinctPerTier,UpdatedAt) VALUES(1,1," +
        std::to_wstring(policy.minimum_battles) + L"," +
        std::to_wstring(policy.minimum_player_kills) + L"," +
        std::to_wstring(policy.required_tiers) + L"," +
        std::to_wstring(policy.minimum_distinct_per_tier) +
        L",SYSUTCDATETIME());\n"
        L"END\n"
        L"ELSE\n"
        L"BEGIN\n"
        L" UPDATE [sdgo_friend_unit_gift].[Policy]\n"
        L" SET Enabled=1,MinBattles=" +
        std::to_wstring(policy.minimum_battles) +
        L",MinPlayerKills=" + std::to_wstring(policy.minimum_player_kills) +
        L",RequiredTiers=" + std::to_wstring(policy.required_tiers) +
        L",MinDistinctPerTier=" +
        std::to_wstring(policy.minimum_distinct_per_tier) +
        L",UpdatedAt=SYSUTCDATETIME() WHERE Singleton=1;\n"
        L" IF @@ROWCOUNT<>1\n"
        L"  RAISERROR(N'friend unit gift policy update failed',16,1);\n"
        L"END;\n"
        L"COMMIT TRANSACTION;\n"
        L"IF (@gift_saved_options & 16384)=0 SET XACT_ABORT OFF;\n"
        L"IF (@gift_saved_options & 512)=0 SET NOCOUNT OFF;\n"
        L"END TRY\nBEGIN CATCH\n"
        L" DECLARE @gift_error nvarchar(2048)=ERROR_MESSAGE();\n"
        L" IF XACT_STATE()<>0 ROLLBACK TRANSACTION;\n"
        L" IF (@gift_saved_options & 16384)=0 SET XACT_ABORT OFF;\n"
        L" IF (@gift_saved_options & 512)=0 SET NOCOUNT OFF;\n"
        L" RAISERROR(N'friend unit gift install failed: %s',16,1,@gift_error);\n"
        L"END CATCH;\n";
    return sql;
}

inline std::optional<std::wstring> MakeSetPolicySql(
    const EligibilityPolicy& policy,
    bool schemaExists) {
    if (!schemaExists || !ValidPolicy(policy)) return std::nullopt;
    return L"EXEC [sdgo_friend_unit_gift].[SetPolicy] @Enabled=" +
        std::to_wstring(policy.enabled ? 1 : 0) +
        L",@MinBattles=" + std::to_wstring(policy.minimum_battles) +
        L",@MinPlayerKills=" + std::to_wstring(policy.minimum_player_kills) +
        L",@RequiredTiers=" + std::to_wstring(policy.required_tiers) +
        L",@MinDistinctPerTier=" +
        std::to_wstring(policy.minimum_distinct_per_tier) + L";";
}

} // namespace sdgo::friend_unit_gift
