-- OC compatibility enhancement: owned schema in GOnlineGame.
-- The original OC rules stored allocations in tbl_UserUnits columns that the
-- current database no longer has. This Mod keeps the same rules and stores the
-- OC profile in its own table so the current custom-parts data stays untouched.
-- Batches are separated by lines containing only GO.

IF OBJECT_ID(N'dbo.sdgo_mod_oc_Meta', N'U') IS NULL
CREATE TABLE dbo.sdgo_mod_oc_Meta (
    [Key] VARCHAR(32) NOT NULL PRIMARY KEY,
    [Value] NVARCHAR(128) NOT NULL,
    UpdatedAt DATETIME NOT NULL DEFAULT GETDATE()
);
GO
IF OBJECT_ID(N'dbo.sdgo_mod_oc_UnitRev', N'U') IS NULL
CREATE TABLE dbo.sdgo_mod_oc_UnitRev (
    UnitNumber INT NOT NULL PRIMARY KEY,
    CustomNumber INT NOT NULL,      -- tbl_CustomRev key
    Source TINYINT NOT NULL         -- 1 original OC-era catalogue, 2 grade formula, 3 server owner's growth file
);
GO
IF OBJECT_ID(N'dbo.sdgo_mod_oc_UserUnitCustom', N'U') IS NULL
CREATE TABLE dbo.sdgo_mod_oc_UserUnitCustom (
    UserUnitNumber BIGINT NOT NULL PRIMARY KEY,
    AccountNumber INT NOT NULL,
    UnitNumber INT NOT NULL,
    OcLevel TINYINT NOT NULL,
    Normal_MaxLv TINYINT NOT NULL,
    Normal_CustomPnt TINYINT NOT NULL DEFAULT 0,
    Normal_Speed TINYINT NOT NULL DEFAULT 0,
    Normal_HP TINYINT NOT NULL DEFAULT 0,
    Normal_Attack TINYINT NOT NULL DEFAULT 0,
    Normal_Defense TINYINT NOT NULL DEFAULT 0,
    Normal_Agility TINYINT NOT NULL DEFAULT 0,
    Normal_Special TINYINT NOT NULL DEFAULT 0,
    Over_CustomPnt TINYINT NOT NULL DEFAULT 0,
    Over_Speed TINYINT NOT NULL DEFAULT 0,
    Over_HP TINYINT NOT NULL DEFAULT 0,
    Over_Attack TINYINT NOT NULL DEFAULT 0,
    Over_Defense TINYINT NOT NULL DEFAULT 0,
    Over_Agility TINYINT NOT NULL DEFAULT 0,
    Over_Special TINYINT NOT NULL DEFAULT 0,
    ExtraBonus TINYINT NOT NULL DEFAULT 0,
    State TINYINT NOT NULL DEFAULT 0,          -- 0 active, 2 retired (native row gone)
    Revision INT NOT NULL DEFAULT 1,
    CreatedAt DATETIME NOT NULL DEFAULT GETDATE(),
    UpdatedAt DATETIME NOT NULL DEFAULT GETDATE()
);
GO
IF NOT EXISTS (SELECT 1 FROM sys.indexes WHERE name = N'IX_sdgo_mod_oc_UserUnitCustom_Account' AND object_id = OBJECT_ID(N'dbo.sdgo_mod_oc_UserUnitCustom'))
CREATE INDEX IX_sdgo_mod_oc_UserUnitCustom_Account ON dbo.sdgo_mod_oc_UserUnitCustom (AccountNumber);
GO
IF OBJECT_ID(N'dbo.sdgo_mod_oc_CustomLog', N'U') IS NULL
CREATE TABLE dbo.sdgo_mod_oc_CustomLog (
    idx INT IDENTITY(1,1) NOT NULL PRIMARY KEY,
    LogDate DATETIME NOT NULL DEFAULT GETDATE(),
    AccountID INT NOT NULL,
    UserUnitNumber BIGINT NOT NULL,
    UnitNumber INT NOT NULL,
    NowLv TINYINT NOT NULL,
    NextLv TINYINT NOT NULL,
    AlterLv TINYINT NOT NULL,
    LvPrevention TINYINT NOT NULL,
    Success TINYINT NOT NULL
);
GO

-- Resolve the native owned unit and keep the OC profile aligned with it.
-- Return values: 0 ok, 1 unit not owned/not found, 2 catalogue row missing.
IF OBJECT_ID(N'dbo.sdgo_mod_oc_EnsureProfile', N'P') IS NOT NULL DROP PROCEDURE dbo.sdgo_mod_oc_EnsureProfile;
GO
CREATE PROCEDURE dbo.sdgo_mod_oc_EnsureProfile
    @v_AccountNum INT,
    @v_UserUnitNum BIGINT,
    @v_RetVal INT OUTPUT
AS
SET NOCOUNT ON
DECLARE @UnitNumber INT, @UnitLevel TINYINT, @NormalMaxLv TINYINT, @Owner INT
DECLARE @OcLevel TINYINT, @NormalPnt TINYINT, @Grade INT
SET @v_RetVal = 0
SELECT @Owner = AccountNumber, @UnitNumber = UnitNumber, @UnitLevel = UnitLevelNumber, @NormalMaxLv = Normal_MaxLv
FROM dbo.tbl_UserUnits WHERE UserUnitNumber = @v_UserUnitNum
IF @@ROWCOUNT = 0 OR @Owner <> @v_AccountNum
BEGIN
    SET @v_RetVal = 1
    RETURN
END
-- A unit added to tbl_Units while the World runs gets the grade formula row
-- (CustomNumber 1010000 + grade 1..4); the next World start checks it against
-- the catalogue and the owner's growth file.
IF NOT EXISTS (SELECT 1 FROM dbo.sdgo_mod_oc_UnitRev WHERE UnitNumber = @UnitNumber)
BEGIN
    SELECT @Grade = UnitGradeNumber FROM dbo.tbl_Units WHERE UnitNumber = @UnitNumber
    IF @Grade < 1 SET @Grade = 1
    IF @Grade > 4 SET @Grade = 4
    IF @Grade IS NOT NULL AND EXISTS (SELECT 1 FROM dbo.tbl_CustomRev WHERE CustomNumber = 1010000 + @Grade)
        INSERT INTO dbo.sdgo_mod_oc_UnitRev (UnitNumber, CustomNumber, Source)
        SELECT @UnitNumber, 1010000 + @Grade, 2
        WHERE NOT EXISTS (SELECT 1 FROM dbo.sdgo_mod_oc_UnitRev WITH (UPDLOCK, HOLDLOCK) WHERE UnitNumber = @UnitNumber)
END
IF NOT EXISTS (SELECT 1 FROM dbo.sdgo_mod_oc_UnitRev WHERE UnitNumber = @UnitNumber)
BEGIN
    SET @v_RetVal = 2
    RETURN
END
IF @NormalMaxLv < 2 SET @NormalMaxLv = 2
IF @NormalMaxLv > 4 SET @NormalMaxLv = 4
-- Original semantics: normal custom levels end at Normal_MaxLv + 4; over custom
-- levels (9..15) are only earned through the OC challenge.
SET @OcLevel = @UnitLevel
IF @OcLevel > @NormalMaxLv + 4 SET @OcLevel = @NormalMaxLv + 4
IF @OcLevel < 1 SET @OcLevel = 1
SET @NormalPnt = CASE WHEN @OcLevel >= 5 THEN @OcLevel - 4 ELSE 0 END
IF NOT EXISTS (SELECT 1 FROM dbo.sdgo_mod_oc_UserUnitCustom WHERE UserUnitNumber = @v_UserUnitNum)
BEGIN
    INSERT INTO dbo.sdgo_mod_oc_UserUnitCustom (UserUnitNumber, AccountNumber, UnitNumber, OcLevel, Normal_MaxLv, Normal_CustomPnt)
    VALUES (@v_UserUnitNum, @v_AccountNum, @UnitNumber, @OcLevel, @NormalMaxLv, @NormalPnt)
    RETURN
END
-- Template replaced in place (exchange/trade): the old allocation no longer fits; reset.
IF EXISTS (SELECT 1 FROM dbo.sdgo_mod_oc_UserUnitCustom WHERE UserUnitNumber = @v_UserUnitNum AND UnitNumber <> @UnitNumber)
BEGIN
    UPDATE dbo.sdgo_mod_oc_UserUnitCustom
    SET AccountNumber = @v_AccountNum, UnitNumber = @UnitNumber, OcLevel = @OcLevel, Normal_MaxLv = @NormalMaxLv,
        Normal_CustomPnt = @NormalPnt, Normal_Speed = 0, Normal_HP = 0, Normal_Attack = 0, Normal_Defense = 0, Normal_Agility = 0, Normal_Special = 0,
        Over_CustomPnt = 0, Over_Speed = 0, Over_HP = 0, Over_Attack = 0, Over_Defense = 0, Over_Agility = 0, Over_Special = 0,
        ExtraBonus = 0, State = 0, Revision = Revision + 1, UpdatedAt = GETDATE()
    WHERE UserUnitNumber = @v_UserUnitNum
    RETURN
END
-- Ownership moved with the unit (gift): the profile follows the native row.
UPDATE dbo.sdgo_mod_oc_UserUnitCustom
SET AccountNumber = @v_AccountNum, State = 0, Revision = Revision + 1, UpdatedAt = GETDATE()
WHERE UserUnitNumber = @v_UserUnitNum AND (AccountNumber <> @v_AccountNum OR State <> 0)
-- A unit that has grown further in the current system gains the same normal points it would have had.
UPDATE dbo.sdgo_mod_oc_UserUnitCustom
SET OcLevel = @OcLevel,
    Normal_CustomPnt = Normal_CustomPnt + (@OcLevel - OcLevel),
    Revision = Revision + 1, UpdatedAt = GETDATE()
WHERE UserUnitNumber = @v_UserUnitNum AND OcLevel < 9 AND OcLevel < @OcLevel
RETURN
GO

-- One row describing the unit's OC state, rule row, base stats and budgets.
IF OBJECT_ID(N'dbo.sdgo_mod_oc_GetProfile', N'P') IS NOT NULL DROP PROCEDURE dbo.sdgo_mod_oc_GetProfile;
GO
CREATE PROCEDURE dbo.sdgo_mod_oc_GetProfile
    @v_AccountNum INT,
    @v_UserUnitNum BIGINT,
    @v_RetVal INT OUTPUT
AS
SET NOCOUNT ON
EXEC dbo.sdgo_mod_oc_EnsureProfile @v_AccountNum, @v_UserUnitNum, @v_RetVal OUTPUT
IF @v_RetVal <> 0 RETURN
SELECT P.UserUnitNumber, P.AccountNumber, P.UnitNumber, P.OcLevel, P.Normal_MaxLv, P.Revision, P.State, P.ExtraBonus,
    P.Normal_CustomPnt, P.Normal_Speed, P.Normal_HP, P.Normal_Attack, P.Normal_Defense, P.Normal_Agility, P.Normal_Special,
    P.Over_CustomPnt, P.Over_Speed, P.Over_HP, P.Over_Attack, P.Over_Defense, P.Over_Agility, P.Over_Special,
    UU.UnitLevelNumber, UU.LapUnitExp, UU.UnitBattle, TU.UnitGradeNumber, R.CustomNumber,
    TU.DashLevel AS Base_Speed, TU.HP AS Base_HP, TU.StrikingPower AS Base_Attack, TU.DefensivePower AS Base_Defense,
    TU.MotivePower AS Base_Agility, ISNULL(TWR.DamageSize, 0) AS Base_Special,
    TCR.Normal_Speed_Stat, TCR.Normal_HP_Stat, TCR.Normal_Attack_Stat, TCR.Normal_Defense_Stat, TCR.Normal_Agility_Stat, TCR.Normal_Special_Stat,
    TCR.Over_Speed_Stat, TCR.Over_HP_Stat, TCR.Over_Attack_Stat, TCR.Over_Defense_Stat, TCR.Over_Agility_Stat, TCR.Over_Special_Stat,
    CASE WHEN P.OcLevel > 8 THEN P.Normal_MaxLv WHEN P.OcLevel >= 5 THEN P.OcLevel - 4 ELSE 0 END AS NormalBudget,
    ISNULL((SELECT TOP 1 TotalCustomPoint FROM dbo.tbl_CustomDice WHERE CustomLv = P.OcLevel AND UnitGradeNumber = TU.UnitGradeNumber), 0) AS OverBudget,
    CASE WHEN P.OcLevel < 9 THEN 9 ELSE P.OcLevel + 1 END AS NextLevel,
    ISNULL((SELECT TOP 1 Probability FROM dbo.tbl_CustomDice WHERE CustomLv = CASE WHEN P.OcLevel < 9 THEN 9 ELSE P.OcLevel + 1 END AND UnitGradeNumber = TU.UnitGradeNumber), 0) AS NextProbability,
    ISNULL((SELECT TOP 1 GiveCustomPoint FROM dbo.tbl_CustomDice WHERE CustomLv = CASE WHEN P.OcLevel < 9 THEN 9 ELSE P.OcLevel + 1 END AND UnitGradeNumber = TU.UnitGradeNumber), 0) AS NextGivePoint,
    ISNULL((SELECT TOP 1 BattleCnt FROM dbo.tbl_CustomDice WHERE CustomLv = CASE WHEN P.OcLevel < 9 THEN 9 ELSE P.OcLevel + 1 END AND UnitGradeNumber = TU.UnitGradeNumber), 0) AS NextBattleCnt,
    ISNULL((SELECT TOP 1 LapUnitExp FROM dbo.tbl_UnitLevels WHERE UnitLevelIndex = TU.UnitGrowIndex AND UnitLevelNumber = P.OcLevel), 0) AS LevelMaxExp
FROM dbo.sdgo_mod_oc_UserUnitCustom P
INNER JOIN dbo.tbl_UserUnits UU ON UU.UserUnitNumber = P.UserUnitNumber
INNER JOIN dbo.tbl_Units TU ON TU.UnitNumber = UU.UnitNumber
INNER JOIN dbo.sdgo_mod_oc_UnitRev R ON R.UnitNumber = TU.UnitNumber
INNER JOIN dbo.tbl_CustomRev TCR ON TCR.CustomNumber = R.CustomNumber
LEFT JOIN dbo.tbl_WeaponsRev TWR ON TWR.WeaponNo = TU.WeaponNumber_FatalAttack
WHERE P.UserUnitNumber = @v_UserUnitNum AND P.AccountNumber = @v_AccountNum
IF @@ROWCOUNT = 0 SET @v_RetVal = 2
RETURN
GO

-- Port of sp_SetCustomUserUnit_Normal (2009-03-20). Return values as the original:
-- 1 unit data missing, 2 allocation exceeds budget, 3 stat cap exceeded, 4 no points,
-- plus 5 stale revision.
IF OBJECT_ID(N'dbo.sdgo_mod_oc_SetNormal', N'P') IS NOT NULL DROP PROCEDURE dbo.sdgo_mod_oc_SetNormal;
GO
CREATE PROCEDURE dbo.sdgo_mod_oc_SetNormal
    @v_AccountNum INT,
    @v_UserUnitNum BIGINT,
    @v_Speed TINYINT, @v_HP TINYINT, @v_Attack TINYINT, @v_Defense TINYINT, @v_Agility TINYINT, @v_Special TINYINT,
    @v_Revision INT,
    @v_rCustomPnt TINYINT OUTPUT,
    @v_RetVal INT OUTPUT
AS
SET NOCOUNT ON
DECLARE @OcLevel TINYINT, @CustomMaxLv TINYINT, @NowPnt TINYINT, @Revision INT
DECLARE @SpeedData INT, @HPData INT, @AttackData INT, @DefenseData INT, @AgilityData INT, @SpecialData INT
DECLARE @S INT, @H INT, @A INT, @D INT, @G INT, @X INT
DECLARE @Sum INT
SET @v_RetVal = 0
SET @v_rCustomPnt = 0
EXEC dbo.sdgo_mod_oc_EnsureProfile @v_AccountNum, @v_UserUnitNum, @v_RetVal OUTPUT
IF @v_RetVal <> 0 BEGIN SET @v_RetVal = 1 RETURN END
SELECT @OcLevel = P.OcLevel, @CustomMaxLv = P.Normal_MaxLv, @NowPnt = P.Normal_CustomPnt, @Revision = P.Revision,
    @SpeedData = TU.DashLevel, @HPData = TU.HP, @AttackData = TU.StrikingPower, @DefenseData = TU.DefensivePower,
    @AgilityData = TU.MotivePower, @SpecialData = ISNULL(TWR.DamageSize, 0),
    @S = TCR.Normal_Speed_Stat, @H = TCR.Normal_HP_Stat, @A = TCR.Normal_Attack_Stat,
    @D = TCR.Normal_Defense_Stat, @G = TCR.Normal_Agility_Stat, @X = TCR.Normal_Special_Stat
FROM dbo.sdgo_mod_oc_UserUnitCustom P
INNER JOIN dbo.tbl_UserUnits UU ON UU.UserUnitNumber = P.UserUnitNumber
INNER JOIN dbo.tbl_Units TU ON TU.UnitNumber = UU.UnitNumber
INNER JOIN dbo.sdgo_mod_oc_UnitRev R ON R.UnitNumber = TU.UnitNumber
INNER JOIN dbo.tbl_CustomRev TCR ON TCR.CustomNumber = R.CustomNumber
LEFT JOIN dbo.tbl_WeaponsRev TWR ON TWR.WeaponNo = TU.WeaponNumber_FatalAttack
WHERE P.UserUnitNumber = @v_UserUnitNum AND P.AccountNumber = @v_AccountNum
IF @@ROWCOUNT = 0 BEGIN SET @v_RetVal = 1 RETURN END
IF @v_Revision <> 0 AND @v_Revision <> @Revision BEGIN SET @v_RetVal = 5 RETURN END
IF @OcLevel < 5 OR @NowPnt < 1 BEGIN SET @v_RetVal = 4 RETURN END
SET @Sum = @v_Speed + @v_HP + @v_Attack + @v_Defense + @v_Agility + @v_Special
IF 8 < @OcLevel
BEGIN
    IF @CustomMaxLv < @Sum BEGIN SET @v_RetVal = 2 RETURN END
    SET @v_rCustomPnt = @CustomMaxLv - @Sum
END
ELSE
BEGIN
    IF (@OcLevel - 4) < @Sum BEGIN SET @v_RetVal = 2 RETURN END
    SET @v_rCustomPnt = (@OcLevel - 4) - @Sum
END
IF 0 < @v_Speed AND 100 < @SpeedData + @v_Speed * @S BEGIN SET @v_RetVal = 3 RETURN END
IF 0 < @v_HP AND 23800 < @HPData + @v_HP * @H BEGIN SET @v_RetVal = 3 RETURN END
IF 0 < @v_Attack AND 169 < @AttackData + @v_Attack * @A BEGIN SET @v_RetVal = 3 RETURN END
IF 0 < @v_Defense AND 169 < @DefenseData + @v_Defense * @D BEGIN SET @v_RetVal = 3 RETURN END
IF 0 < @v_Agility AND 100 < @AgilityData + @v_Agility * @G BEGIN SET @v_RetVal = 3 RETURN END
IF 0 < @v_Special AND 7950 < @SpecialData + @v_Special * @X BEGIN SET @v_RetVal = 3 RETURN END
UPDATE dbo.sdgo_mod_oc_UserUnitCustom
SET Normal_CustomPnt = @v_rCustomPnt, Normal_Speed = @v_Speed, Normal_HP = @v_HP, Normal_Attack = @v_Attack,
    Normal_Defense = @v_Defense, Normal_Agility = @v_Agility, Normal_Special = @v_Special,
    Revision = Revision + 1, UpdatedAt = GETDATE()
WHERE UserUnitNumber = @v_UserUnitNum AND AccountNumber = @v_AccountNum AND Revision = @Revision
IF @@ROWCOUNT = 0 SET @v_RetVal = 5
RETURN
GO

-- Port of sp_SetCustomUserUnit_Over (2009-03-22). Return values as the original:
-- 1 unit data missing, 3 budget or stat cap exceeded, 4 not an over-custom unit, plus 5 stale revision.
IF OBJECT_ID(N'dbo.sdgo_mod_oc_SetOver', N'P') IS NOT NULL DROP PROCEDURE dbo.sdgo_mod_oc_SetOver;
GO
CREATE PROCEDURE dbo.sdgo_mod_oc_SetOver
    @v_AccountNum INT,
    @v_UserUnitNum BIGINT,
    @v_Speed TINYINT, @v_HP TINYINT, @v_Attack TINYINT, @v_Defense TINYINT, @v_Agility TINYINT, @v_Special TINYINT,
    @v_Revision INT,
    @v_rCustomPnt TINYINT OUTPUT,
    @v_RetVal INT OUTPUT
AS
SET NOCOUNT ON
DECLARE @OcLevel TINYINT, @NowNormalPnt TINYINT, @NowOverPnt TINYINT, @Revision INT, @Total INT
DECLARE @NS INT, @NH INT, @NA INT, @ND INT, @NG INT, @NX INT
DECLARE @SpeedData INT, @HPData INT, @AttackData INT, @DefenseData INT, @AgilityData INT, @SpecialData INT
DECLARE @S INT, @H INT, @A INT, @D INT, @G INT, @X INT
DECLARE @OS INT, @OH INT, @OA INT, @OD INT, @OG INT, @OX INT
DECLARE @Sum INT
SET @v_RetVal = 0
SET @v_rCustomPnt = 0
EXEC dbo.sdgo_mod_oc_EnsureProfile @v_AccountNum, @v_UserUnitNum, @v_RetVal OUTPUT
IF @v_RetVal <> 0 BEGIN SET @v_RetVal = 1 RETURN END
SELECT @OcLevel = P.OcLevel, @NowNormalPnt = P.Normal_CustomPnt, @NowOverPnt = P.Over_CustomPnt, @Revision = P.Revision,
    @NS = P.Normal_Speed, @NH = P.Normal_HP, @NA = P.Normal_Attack, @ND = P.Normal_Defense, @NG = P.Normal_Agility, @NX = P.Normal_Special,
    @SpeedData = TU.DashLevel, @HPData = TU.HP, @AttackData = TU.StrikingPower, @DefenseData = TU.DefensivePower,
    @AgilityData = TU.MotivePower, @SpecialData = ISNULL(TWR.DamageSize, 0),
    @Total = ISNULL(TCD.TotalCustomPoint, 0),
    @S = TCR.Normal_Speed_Stat, @H = TCR.Normal_HP_Stat, @A = TCR.Normal_Attack_Stat,
    @D = TCR.Normal_Defense_Stat, @G = TCR.Normal_Agility_Stat, @X = TCR.Normal_Special_Stat,
    @OS = TCR.Over_Speed_Stat, @OH = TCR.Over_HP_Stat, @OA = TCR.Over_Attack_Stat,
    @OD = TCR.Over_Defense_Stat, @OG = TCR.Over_Agility_Stat, @OX = TCR.Over_Special_Stat
FROM dbo.sdgo_mod_oc_UserUnitCustom P
INNER JOIN dbo.tbl_UserUnits UU ON UU.UserUnitNumber = P.UserUnitNumber
INNER JOIN dbo.tbl_Units TU ON TU.UnitNumber = UU.UnitNumber
INNER JOIN dbo.sdgo_mod_oc_UnitRev R ON R.UnitNumber = TU.UnitNumber
INNER JOIN dbo.tbl_CustomRev TCR ON TCR.CustomNumber = R.CustomNumber
LEFT JOIN dbo.tbl_WeaponsRev TWR ON TWR.WeaponNo = TU.WeaponNumber_FatalAttack
LEFT JOIN dbo.tbl_CustomDice TCD ON TCD.UnitGradeNumber = TU.UnitGradeNumber AND TCD.CustomLv = P.OcLevel
WHERE P.UserUnitNumber = @v_UserUnitNum AND P.AccountNumber = @v_AccountNum
IF @@ROWCOUNT = 0 BEGIN SET @v_RetVal = 1 RETURN END
IF @v_Revision <> 0 AND @v_Revision <> @Revision BEGIN SET @v_RetVal = 5 RETURN END
IF @OcLevel < 9 OR @NowNormalPnt > 0 OR @NowOverPnt = 0 BEGIN SET @v_RetVal = 4 RETURN END
SET @Sum = @v_Speed + @v_HP + @v_Attack + @v_Defense + @v_Agility + @v_Special
IF @Total < @Sum BEGIN SET @v_RetVal = 3 RETURN END
IF 0 < @v_Speed AND 100 < @SpeedData + @NS * @S + @v_Speed * @OS BEGIN SET @v_RetVal = 3 RETURN END
IF 0 < @v_HP AND 23800 < @HPData + @NH * @H + @v_HP * @OH BEGIN SET @v_RetVal = 3 RETURN END
IF 0 < @v_Attack AND 169 < @AttackData + @NA * @A + @v_Attack * @OA BEGIN SET @v_RetVal = 3 RETURN END
IF 0 < @v_Defense AND 169 < @DefenseData + @ND * @D + @v_Defense * @OD BEGIN SET @v_RetVal = 3 RETURN END
IF 0 < @v_Agility AND 100 < @AgilityData + @NG * @G + @v_Agility * @OG BEGIN SET @v_RetVal = 3 RETURN END
IF 0 < @v_Special AND 7950 < @SpecialData + @NX * @X + @v_Special * @OX BEGIN SET @v_RetVal = 3 RETURN END
SET @v_rCustomPnt = @Total - @Sum
UPDATE dbo.sdgo_mod_oc_UserUnitCustom
SET Over_CustomPnt = @v_rCustomPnt, Over_Speed = @v_Speed, Over_HP = @v_HP, Over_Attack = @v_Attack,
    Over_Defense = @v_Defense, Over_Agility = @v_Agility, Over_Special = @v_Special,
    Revision = Revision + 1, UpdatedAt = GETDATE()
WHERE UserUnitNumber = @v_UserUnitNum AND AccountNumber = @v_AccountNum AND Revision = @Revision
IF @@ROWCOUNT = 0 SET @v_RetVal = 5
RETURN
GO

-- Port of sp_SetUserUnitCustomUpgrade (2009-04-09). The OC level lives in the
-- profile, so a failed attempt never rewrites the native level or experience.
-- Return values as the original: 1 unit missing, 2 not ready (level/exp gate),
-- 3 item missing, 4 item level range, 5 item count, 6 dice row missing,
-- 7 item use failed, 8 update failed, 9 reward item failed.
IF OBJECT_ID(N'dbo.sdgo_mod_oc_Upgrade', N'P') IS NOT NULL DROP PROCEDURE dbo.sdgo_mod_oc_Upgrade;
GO
CREATE PROCEDURE dbo.sdgo_mod_oc_Upgrade
    @v_AccountID INT,
    @v_UserUnitNum BIGINT,
    @v_EtcItemNum_1 INT, @v_EtcItemNum_2 INT, @v_EtcItemNum_3 INT,
    @v_EtcItemCnt_1 SMALLINT, @v_EtcItemCnt_2 SMALLINT, @v_EtcItemCnt_3 SMALLINT,
    @v_RewardItem INT,
    @v_UnitNumber INT OUTPUT,
    @v_OcLevel TINYINT OUTPUT,
    @v_Over_CustomPnt TINYINT OUTPUT,
    @v_ItemCnt_1 SMALLINT OUTPUT, @v_ItemCnt_2 SMALLINT OUTPUT, @v_ItemCnt_3 SMALLINT OUTPUT,
    @v_CustomSuccess TINYINT OUTPUT,
    @v_LvPrevention TINYINT OUTPUT,
    @v_NowLv TINYINT OUTPUT,
    @v_NextLv TINYINT OUTPUT,
    @v_RetVal INT OUTPUT
AS
SET NOCOUNT ON
DECLARE @NowUnitLv TINYINT, @NativeLv TINYINT, @NowLapUnitExp INT, @UnitMaxLv TINYINT, @UnitGradeNumber TINYINT, @MaxUnitExp INT
DECLARE @NowOverCustomPnt TINYINT, @rSpeed TINYINT, @rHP TINYINT, @rAttack TINYINT, @rDefense TINYINT, @rAgility TINYINT, @rSpecial TINYINT
DECLARE @RevisionProbability INT, @BattleCnt INT, @RC INT, @Revision INT
DECLARE @CustomMaxLevel TINYINT
SET @CustomMaxLevel = 15
SET @v_ItemCnt_1 = 0 SET @v_ItemCnt_2 = 0 SET @v_ItemCnt_3 = 0
SET @RevisionProbability = 0 SET @v_LvPrevention = 0 SET @v_CustomSuccess = 0 SET @v_NowLv = 0 SET @v_NextLv = 0
SET @v_RetVal = 0
EXEC dbo.sdgo_mod_oc_EnsureProfile @v_AccountID, @v_UserUnitNum, @v_RetVal OUTPUT
IF @v_RetVal <> 0 BEGIN SET @v_RetVal = 1 RETURN END
SELECT TOP 1
    @v_UnitNumber = TUU.UnitNumber, @NowUnitLv = P.OcLevel, @NativeLv = TUU.UnitLevelNumber, @BattleCnt = TUU.UnitBattle,
    @NowLapUnitExp = TUU.LapUnitExp, @UnitMaxLv = P.Normal_MaxLv, @NowOverCustomPnt = P.Over_CustomPnt,
    @rSpeed = P.Over_Speed, @rHP = P.Over_HP, @rAttack = P.Over_Attack, @rDefense = P.Over_Defense, @rAgility = P.Over_Agility, @rSpecial = P.Over_Special,
    @UnitGradeNumber = TU.UnitGradeNumber, @Revision = P.Revision,
    @MaxUnitExp = ISNULL(TUL.LapUnitExp, 0)
FROM dbo.sdgo_mod_oc_UserUnitCustom P
INNER JOIN dbo.tbl_UserUnits TUU ON TUU.UserUnitNumber = P.UserUnitNumber
INNER JOIN dbo.tbl_Units TU ON TUU.UnitNumber = TU.UnitNumber
LEFT JOIN dbo.tbl_UnitLevels TUL ON P.OcLevel = TUL.UnitLevelNumber AND TU.UnitGrowIndex = TUL.UnitLevelIndex
WHERE P.AccountNumber = @v_AccountID AND P.UserUnitNumber = @v_UserUnitNum
IF @@ROWCOUNT = 0 BEGIN SET @v_RetVal = 1 RETURN END
-- Original gate: normal max level reached, below the cap, and the current level's experience bar full.
-- A unit that already grew past this level in the current system satisfies the experience gate.
IF (@NowUnitLv < (@UnitMaxLv + 4)) OR (@NowUnitLv >= @CustomMaxLevel) OR (@NativeLv <= @NowUnitLv AND @MaxUnitExp > @NowLapUnitExp)
BEGIN SET @v_RetVal = 2 RETURN END
DECLARE @ItemRemainCnt_1 SMALLINT, @ProbabilityPlus_1 INT, @LvPrevention_1 TINYINT, @UseLvMax_1 TINYINT, @UseLvMin_1 TINYINT
DECLARE @ItemRemainCnt_2 SMALLINT, @ProbabilityPlus_2 INT, @LvPrevention_2 TINYINT, @UseLvMax_2 TINYINT, @UseLvMin_2 TINYINT
DECLARE @ItemRemainCnt_3 SMALLINT, @ProbabilityPlus_3 INT, @LvPrevention_3 TINYINT, @UseLvMax_3 TINYINT, @UseLvMin_3 TINYINT
SET @ItemRemainCnt_1 = 0 SET @ProbabilityPlus_1 = 0 SET @LvPrevention_1 = 0
SET @ItemRemainCnt_2 = 0 SET @ProbabilityPlus_2 = 0 SET @LvPrevention_2 = 0
SET @ItemRemainCnt_3 = 0 SET @ProbabilityPlus_3 = 0 SET @LvPrevention_3 = 0
IF @v_EtcItemNum_1 > 0 AND @v_EtcItemCnt_1 > 0
BEGIN
    SELECT TOP 1 @ItemRemainCnt_1 = TUEV.RemainCount, @ProbabilityPlus_1 = TE.ProbabilityPlus, @LvPrevention_1 = TE.LvPrevention, @UseLvMin_1 = TE.UseMinLv, @UseLvMax_1 = TE.UseMaxLv
    FROM dbo.tbl_UserEtcItemView TUEV INNER JOIN dbo.tbl_EtcItems TE ON TUEV.EtcItemNumber = TE.EtcItemNumber
    WHERE TUEV.AccountNumber = @v_AccountID AND TUEV.EtcItemNumber = @v_EtcItemNum_1 AND TUEV.ItemType = 3
    IF @@ROWCOUNT = 0 BEGIN SET @v_RetVal = 3 RETURN END
    IF @NowUnitLv < @UseLvMin_1 OR @NowUnitLv > @UseLvMax_1 BEGIN SET @v_RetVal = 4 RETURN END
    IF @v_EtcItemCnt_1 > @ItemRemainCnt_1 BEGIN SET @v_RetVal = 5 RETURN END
    IF 1 = @LvPrevention_1 SET @v_LvPrevention = 1
END
IF @v_EtcItemNum_2 > 0 AND @v_EtcItemCnt_2 > 0
BEGIN
    SELECT TOP 1 @ItemRemainCnt_2 = TUEV.RemainCount, @ProbabilityPlus_2 = TE.ProbabilityPlus, @LvPrevention_2 = TE.LvPrevention, @UseLvMin_2 = TE.UseMinLv, @UseLvMax_2 = TE.UseMaxLv
    FROM dbo.tbl_UserEtcItemView TUEV INNER JOIN dbo.tbl_EtcItems TE ON TUEV.EtcItemNumber = TE.EtcItemNumber
    WHERE TUEV.AccountNumber = @v_AccountID AND TUEV.EtcItemNumber = @v_EtcItemNum_2 AND TUEV.ItemType = 3
    IF @@ROWCOUNT = 0 BEGIN SET @v_RetVal = 3 RETURN END
    IF @NowUnitLv < @UseLvMin_2 OR @NowUnitLv > @UseLvMax_2 BEGIN SET @v_RetVal = 4 RETURN END
    IF @v_EtcItemCnt_2 > @ItemRemainCnt_2 BEGIN SET @v_RetVal = 5 RETURN END
    IF 1 = @LvPrevention_2 SET @v_LvPrevention = 1
END
IF @v_EtcItemNum_3 > 0 AND @v_EtcItemCnt_3 > 0
BEGIN
    SELECT TOP 1 @ItemRemainCnt_3 = TUEV.RemainCount, @ProbabilityPlus_3 = TE.ProbabilityPlus, @LvPrevention_3 = TE.LvPrevention, @UseLvMin_3 = TE.UseMinLv, @UseLvMax_3 = TE.UseMaxLv
    FROM dbo.tbl_UserEtcItemView TUEV INNER JOIN dbo.tbl_EtcItems TE ON TUEV.EtcItemNumber = TE.EtcItemNumber
    WHERE TUEV.AccountNumber = @v_AccountID AND TUEV.EtcItemNumber = @v_EtcItemNum_3 AND TUEV.ItemType = 3
    IF @@ROWCOUNT = 0 BEGIN SET @v_RetVal = 3 RETURN END
    IF @NowUnitLv < @UseLvMin_3 OR @NowUnitLv > @UseLvMax_3 BEGIN SET @v_RetVal = 4 RETURN END
    IF @v_EtcItemCnt_3 > @ItemRemainCnt_3 BEGIN SET @v_RetVal = 5 RETURN END
    IF 1 = @LvPrevention_3 SET @v_LvPrevention = 1
END
DECLARE @UserProbability INT, @NextUnitLevel TINYINT, @NowProbability INT, @GiveCustomPoint TINYINT, @TotalCustomPoint TINYINT, @MinBattleCnt INT
IF @NowUnitLv < 9 SET @NextUnitLevel = 9 ELSE SET @NextUnitLevel = @NowUnitLv + 1
EXEC dbo.sp_GetCustomProbability @NextUnitLevel, @UserProbability OUTPUT
SELECT TOP 1 @NowProbability = Probability, @GiveCustomPoint = GiveCustomPoint, @TotalCustomPoint = TotalCustomPoint, @MinBattleCnt = BattleCnt
FROM dbo.tbl_CustomDice
WHERE CustomLv = @NextUnitLevel AND UnitGradeNumber = @UnitGradeNumber AND BattleCnt <= @BattleCnt
IF @@ROWCOUNT = 0 BEGIN SET @v_RetVal = 6 RETURN END
SET @RevisionProbability = @NowProbability + ((@ProbabilityPlus_1 * @v_EtcItemCnt_1) + (@ProbabilityPlus_2 * @v_EtcItemCnt_2) + (@ProbabilityPlus_3 * @v_EtcItemCnt_3))
IF @RevisionProbability > 10001 SET @RevisionProbability = 10001
IF @RevisionProbability > @UserProbability SET @v_CustomSuccess = 1 ELSE SET @v_CustomSuccess = 0
IF @v_EtcItemNum_1 > 0
BEGIN
    EXEC @RC = dbo.sp_UseEtcItem @v_AccountID, @v_EtcItemNum_1, @v_EtcItemCnt_1, @v_ItemCnt_1 OUTPUT
    IF @RC <> 0 BEGIN SET @v_RetVal = 7 RETURN END
END
IF @v_EtcItemNum_2 > 0
BEGIN
    EXEC @RC = dbo.sp_UseEtcItem @v_AccountID, @v_EtcItemNum_2, @v_EtcItemCnt_2, @v_ItemCnt_2 OUTPUT
    IF @RC <> 0 BEGIN SET @v_RetVal = 7 RETURN END
END
IF @v_EtcItemNum_3 > 0
BEGIN
    EXEC @RC = dbo.sp_UseEtcItem @v_AccountID, @v_EtcItemNum_3, @v_EtcItemCnt_3, @v_ItemCnt_3 OUTPUT
    IF @RC <> 0 BEGIN SET @v_RetVal = 7 RETURN END
END
IF 0 = @v_CustomSuccess
BEGIN
    IF @v_LvPrevention = 1
    BEGIN
        SET @v_OcLevel = @NowUnitLv
        SET @v_Over_CustomPnt = @NowOverCustomPnt
    END
    ELSE
    BEGIN
        SET @v_OcLevel = @UnitMaxLv + 4
        SET @v_Over_CustomPnt = 0
        SET @rSpeed = 0 SET @rHP = 0 SET @rAttack = 0 SET @rDefense = 0 SET @rAgility = 0 SET @rSpecial = 0
    END
END
ELSE
BEGIN
    SET @v_OcLevel = @NextUnitLevel
    SET @v_Over_CustomPnt = @NowOverCustomPnt + @GiveCustomPoint
    IF @TotalCustomPoint <> (@v_Over_CustomPnt + @rSpeed + @rHP + @rAttack + @rDefense + @rAgility + @rSpecial)
    BEGIN
        SET @v_Over_CustomPnt = @TotalCustomPoint
        SET @rSpeed = 0 SET @rHP = 0 SET @rAttack = 0 SET @rDefense = 0 SET @rAgility = 0 SET @rSpecial = 0
    END
END
UPDATE dbo.sdgo_mod_oc_UserUnitCustom
SET OcLevel = @v_OcLevel, Over_CustomPnt = @v_Over_CustomPnt,
    Over_Speed = @rSpeed, Over_HP = @rHP, Over_Attack = @rAttack, Over_Defense = @rDefense, Over_Agility = @rAgility, Over_Special = @rSpecial,
    Revision = Revision + 1, UpdatedAt = GETDATE()
WHERE AccountNumber = @v_AccountID AND UserUnitNumber = @v_UserUnitNum
IF @@ROWCOUNT = 0 BEGIN SET @v_RetVal = 8 RETURN END
SET @v_NowLv = @NowUnitLv
SET @v_NextLv = @NextUnitLevel
INSERT INTO dbo.sdgo_mod_oc_CustomLog (AccountID, UserUnitNumber, UnitNumber, NowLv, NextLv, AlterLv, LvPrevention, Success)
VALUES (@v_AccountID, @v_UserUnitNum, @v_UnitNumber, @NowUnitLv, @NextUnitLevel, @v_OcLevel, @v_LvPrevention, @v_CustomSuccess)
DECLARE @ExtraItemUserNum BIGINT, @ExtraItemCount SMALLINT
IF @v_OcLevel = 15 AND @v_RewardItem > 0
BEGIN
    EXEC @RC = dbo.sp_CreateEtcItem @v_AccountID, @v_RewardItem, 1, @ExtraItemUserNum OUTPUT, @ExtraItemCount OUTPUT
    IF @RC > 0 BEGIN SET @v_RetVal = 9 RETURN END
END
SET @v_RetVal = 0
RETURN
GO

-- Port of sp_GetCustomResetGP (2009-03-19). @v_Work 1 normal, 2 over.
-- Return values: 1 unit missing, 2 over points differ from the level total,
-- 3 nothing to reset (points unused), 4 not an over-custom level, 5 bad work type.
IF OBJECT_ID(N'dbo.sdgo_mod_oc_GetResetGP', N'P') IS NOT NULL DROP PROCEDURE dbo.sdgo_mod_oc_GetResetGP;
GO
CREATE PROCEDURE dbo.sdgo_mod_oc_GetResetGP
    @v_AccountNum INT,
    @v_UserUnitNumber BIGINT,
    @v_Work TINYINT,
    @v_UnitRank TINYINT OUTPUT,
    @v_UnitResetGP INT OUTPUT,
    @v_RetVal INT OUTPUT
AS
SET NOCOUNT ON
DECLARE @NowTotalGivePoint TINYINT, @UnitLevel TINYINT, @Normal_MaxLv TINYINT, @Normal_CustomPnt TINYINT, @Over_CustomPnt TINYINT, @NowUnitResetGP INT
SET @v_RetVal = 0
SET @v_UnitResetGP = 0
IF @v_Work <> 1 AND @v_Work <> 2 BEGIN SET @v_RetVal = 5 RETURN END
EXEC dbo.sdgo_mod_oc_EnsureProfile @v_AccountNum, @v_UserUnitNumber, @v_RetVal OUTPUT
IF @v_RetVal <> 0 BEGIN SET @v_RetVal = 1 RETURN END
SELECT @v_UnitRank = U.UnitGradeNumber, @UnitLevel = P.OcLevel, @Normal_CustomPnt = P.Normal_CustomPnt, @Normal_MaxLv = P.Normal_MaxLv,
    @Over_CustomPnt = P.Over_CustomPnt, @NowUnitResetGP = CD.ResetGP, @NowTotalGivePoint = CD.TotalCustomPoint
FROM dbo.sdgo_mod_oc_UserUnitCustom P
INNER JOIN dbo.tbl_UserUnits UU ON UU.UserUnitNumber = P.UserUnitNumber
INNER JOIN dbo.tbl_Units U ON UU.UnitNumber = U.UnitNumber
INNER JOIN dbo.tbl_CustomDice CD ON U.UnitGradeNumber = CD.UnitGradeNumber AND P.OcLevel = CD.CustomLv
WHERE P.AccountNumber = @v_AccountNum AND P.UserUnitNumber = @v_UserUnitNumber
IF @@ROWCOUNT = 0 BEGIN SET @v_RetVal = 1 RETURN END
IF @v_Work = 1
BEGIN
    IF @UnitLevel > 8
    BEGIN
        IF @Over_CustomPnt <> @NowTotalGivePoint BEGIN SET @v_RetVal = 2 RETURN END
        SELECT @v_UnitResetGP = ResetGP FROM dbo.tbl_CustomDice WHERE CustomLv = @Normal_MaxLv + 4 AND UnitGradeNumber = @v_UnitRank
    END
    ELSE
    BEGIN
        IF @Normal_CustomPnt + 4 = @UnitLevel BEGIN SET @v_RetVal = 3 RETURN END
        SET @v_UnitResetGP = @NowUnitResetGP
    END
END
ELSE
BEGIN
    IF @UnitLevel < 9 BEGIN SET @v_RetVal = 4 RETURN END
    IF @NowTotalGivePoint = @Over_CustomPnt BEGIN SET @v_RetVal = 3 RETURN END
    SET @v_UnitResetGP = @NowUnitResetGP
END
RETURN
GO

-- Port of sp_SetCustomPointReset (2009-03-19). @v_Work 1 normal, 2 over.
-- Return values: 1 bad work type, 2 unit missing, 3 not at a resettable level, 4 update failed.
IF OBJECT_ID(N'dbo.sdgo_mod_oc_Reset', N'P') IS NOT NULL DROP PROCEDURE dbo.sdgo_mod_oc_Reset;
GO
CREATE PROCEDURE dbo.sdgo_mod_oc_Reset
    @v_AccountID INT,
    @v_UserUnitNumber BIGINT,
    @v_Work TINYINT,
    @v_Normal_CustomPnt TINYINT OUTPUT,
    @v_Over_CustomPnt TINYINT OUTPUT,
    @v_RetVal INT OUTPUT
AS
SET NOCOUNT ON
DECLARE @UnitLevel TINYINT, @Normal_MaxLv TINYINT, @TotalGivePoint INT
SET @v_RetVal = 0
SET @v_Normal_CustomPnt = 0
SET @v_Over_CustomPnt = 0
IF @v_Work <> 1 AND @v_Work <> 2 BEGIN SET @v_RetVal = 1 RETURN END
EXEC dbo.sdgo_mod_oc_EnsureProfile @v_AccountID, @v_UserUnitNumber, @v_RetVal OUTPUT
IF @v_RetVal <> 0 BEGIN SET @v_RetVal = 2 RETURN END
SELECT @UnitLevel = P.OcLevel, @v_Normal_CustomPnt = P.Normal_CustomPnt, @Normal_MaxLv = P.Normal_MaxLv, @v_Over_CustomPnt = P.Over_CustomPnt,
    @TotalGivePoint = ISNULL(CD.TotalCustomPoint, 0)
FROM dbo.sdgo_mod_oc_UserUnitCustom P
INNER JOIN dbo.tbl_UserUnits UU ON UU.UserUnitNumber = P.UserUnitNumber
INNER JOIN dbo.tbl_Units U ON UU.UnitNumber = U.UnitNumber
LEFT JOIN dbo.tbl_CustomDice CD ON U.UnitGradeNumber = CD.UnitGradeNumber AND P.OcLevel = CD.CustomLv
WHERE P.AccountNumber = @v_AccountID AND P.UserUnitNumber = @v_UserUnitNumber
IF @@ROWCOUNT = 0 BEGIN SET @v_RetVal = 2 RETURN END
IF @v_Work = 1
BEGIN
    IF @UnitLevel < 5 BEGIN SET @v_RetVal = 3 RETURN END
    IF @UnitLevel > 8 SET @v_Normal_CustomPnt = @Normal_MaxLv ELSE SET @v_Normal_CustomPnt = @UnitLevel - 4
    UPDATE dbo.sdgo_mod_oc_UserUnitCustom
    SET Normal_CustomPnt = @v_Normal_CustomPnt, Normal_Speed = 0, Normal_HP = 0, Normal_Attack = 0, Normal_Defense = 0, Normal_Agility = 0, Normal_Special = 0,
        Revision = Revision + 1, UpdatedAt = GETDATE()
    WHERE UserUnitNumber = @v_UserUnitNumber AND AccountNumber = @v_AccountID
    IF @@ROWCOUNT = 0 BEGIN SET @v_RetVal = 4 RETURN END
END
ELSE
BEGIN
    IF @UnitLevel < 9 BEGIN SET @v_RetVal = 3 RETURN END
    SET @v_Over_CustomPnt = @TotalGivePoint
    UPDATE dbo.sdgo_mod_oc_UserUnitCustom
    SET Over_CustomPnt = @v_Over_CustomPnt, Over_Speed = 0, Over_HP = 0, Over_Attack = 0, Over_Defense = 0, Over_Agility = 0, Over_Special = 0,
        Revision = Revision + 1, UpdatedAt = GETDATE()
    WHERE UserUnitNumber = @v_UserUnitNumber AND AccountNumber = @v_AccountID
    IF @@ROWCOUNT = 0 BEGIN SET @v_RetVal = 4 RETURN END
END
RETURN
GO

-- Battle/display payload for one owned unit: the stat deltas the allocation
-- yields with the unit's rule row. Zero rows when the unit has no profile.
IF OBJECT_ID(N'dbo.sdgo_mod_oc_Carrier', N'P') IS NOT NULL DROP PROCEDURE dbo.sdgo_mod_oc_Carrier;
GO
CREATE PROCEDURE dbo.sdgo_mod_oc_Carrier
    @v_UserUnitNum BIGINT
AS
SET NOCOUNT ON
SELECT P.OcLevel, P.Normal_CustomPnt, P.Over_CustomPnt, P.Revision,
    P.Normal_Speed * TCR.Normal_Speed_Stat + P.Over_Speed * TCR.Over_Speed_Stat AS Delta_Speed,
    P.Normal_HP * TCR.Normal_HP_Stat + P.Over_HP * TCR.Over_HP_Stat AS Delta_HP,
    P.Normal_Attack * TCR.Normal_Attack_Stat + P.Over_Attack * TCR.Over_Attack_Stat AS Delta_Attack,
    P.Normal_Defense * TCR.Normal_Defense_Stat + P.Over_Defense * TCR.Over_Defense_Stat AS Delta_Defense,
    P.Normal_Agility * TCR.Normal_Agility_Stat + P.Over_Agility * TCR.Over_Agility_Stat AS Delta_Agility,
    P.Normal_Special * TCR.Normal_Special_Stat + P.Over_Special * TCR.Over_Special_Stat AS Delta_Special
FROM dbo.sdgo_mod_oc_UserUnitCustom P
INNER JOIN dbo.tbl_UserUnits UU ON UU.UserUnitNumber = P.UserUnitNumber AND UU.UnitNumber = P.UnitNumber AND UU.AccountNumber = P.AccountNumber
INNER JOIN dbo.sdgo_mod_oc_UnitRev R ON R.UnitNumber = P.UnitNumber
INNER JOIN dbo.tbl_CustomRev TCR ON TCR.CustomNumber = R.CustomNumber
WHERE P.UserUnitNumber = @v_UserUnitNum AND P.State = 0
RETURN
GO
