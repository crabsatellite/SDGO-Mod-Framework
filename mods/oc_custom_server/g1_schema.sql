-- G改 1.0 attribute loadouts: owned schema in GOnlineGame.
-- The current custom-skill attributes (G改 2.0) stay in tbl_UserUnits
-- UnitSkill_1..4. A G改 1.0 loadout is a second set of four attribute skills per
-- unit, kept in this Mod's own table and checked against the server owner's
-- G改 1.0 rule set (sdgo_mod_g1_Rule, the shape of tbl_UnitCustomSkillRev) with
-- the native custom-skill rules. Rooms with the !g1 command build their battle
-- units with it. Batches are separated by lines containing only GO.

IF OBJECT_ID(N'dbo.sdgo_mod_g1_Rule', N'U') IS NULL
CREATE TABLE dbo.sdgo_mod_g1_Rule (
    UnitSkillID SMALLINT NOT NULL PRIMARY KEY,
    Category TINYINT NOT NULL,
    SkillType TINYINT NOT NULL,
    UseUnitLv TINYINT NOT NULL,
    UseUserLv TINYINT NOT NULL,
    Premium TINYINT NOT NULL,
    ResetCost SMALLINT NOT NULL
);
GO
IF OBJECT_ID(N'dbo.sdgo_mod_g1_UserUnitSkill', N'U') IS NULL
CREATE TABLE dbo.sdgo_mod_g1_UserUnitSkill (
    UserUnitNumber BIGINT NOT NULL PRIMARY KEY,
    AccountNumber INT NOT NULL,
    UnitNumber INT NOT NULL,
    Skill_1 SMALLINT NOT NULL DEFAULT 0,
    Skill_2 SMALLINT NOT NULL DEFAULT 0,
    Skill_3 SMALLINT NOT NULL DEFAULT 0,
    Skill_4 SMALLINT NOT NULL DEFAULT 0,
    ResetCnt SMALLINT NOT NULL DEFAULT 0,
    Revision INT NOT NULL DEFAULT 1,
    UpdatedAt DATETIME NOT NULL DEFAULT GETDATE()
);
GO
IF NOT EXISTS (SELECT 1 FROM sys.indexes WHERE name = N'IX_sdgo_mod_g1_UserUnitSkill_Account' AND object_id = OBJECT_ID(N'dbo.sdgo_mod_g1_UserUnitSkill'))
CREATE INDEX IX_sdgo_mod_g1_UserUnitSkill_Account ON dbo.sdgo_mod_g1_UserUnitSkill (AccountNumber);
GO

-- The unit's loadout with what the client shows: levels, slot count, the
-- category of the equipped skills, the reset count and the next reset's GP.
-- A unit without a row has an empty loadout. Return values: 0 ok, 2002 unit
-- not owned.
IF OBJECT_ID(N'dbo.sdgo_mod_g1_Get', N'P') IS NOT NULL DROP PROCEDURE dbo.sdgo_mod_g1_Get;
GO
CREATE PROCEDURE dbo.sdgo_mod_g1_Get
    @v_AccountNum INT,
    @v_UserUnitNum BIGINT,
    @v_RetVal INT OUTPUT
AS
SET NOCOUNT ON
DECLARE @UnitNumber INT, @UnitLv TINYINT, @NormalMaxLv TINYINT, @UserLv TINYINT
DECLARE @S1 SMALLINT, @S2 SMALLINT, @S3 SMALLINT, @S4 SMALLINT, @ResetCnt SMALLINT, @Revision INT, @Cost INT, @r INT
SET @v_RetVal = 0
SELECT TOP 1 @UnitNumber = UnitNumber, @UnitLv = UnitLevelNumber, @NormalMaxLv = Normal_MaxLv
FROM dbo.tbl_UserUnits WHERE AccountNumber = @v_AccountNum AND UserUnitNumber = @v_UserUnitNum
IF @@ROWCOUNT = 0 BEGIN SET @v_RetVal = 2002; RETURN; END
SELECT TOP 1 @UserLv = UserLevelNumber FROM dbo.tbl_Users WHERE AccountNumber = @v_AccountNum
SELECT TOP 1 @S1 = Skill_1, @S2 = Skill_2, @S3 = Skill_3, @S4 = Skill_4, @ResetCnt = ResetCnt, @Revision = Revision
FROM dbo.sdgo_mod_g1_UserUnitSkill
WHERE UserUnitNumber = @v_UserUnitNum AND AccountNumber = @v_AccountNum AND UnitNumber = @UnitNumber
EXEC dbo.sdgo_mod_g1_GetResetCost @v_AccountNum, @v_UserUnitNum, @Cost OUTPUT, @r OUTPUT
SELECT @v_UserUnitNum, ISNULL(@S1, 0), ISNULL(@S2, 0), ISNULL(@S3, 0), ISNULL(@S4, 0), ISNULL(@ResetCnt, 0),
       ISNULL(@NormalMaxLv, 0), ISNULL(@UnitLv, 0), ISNULL(@UserLv, 0),
       ISNULL((SELECT TOP 1 R.Category FROM dbo.sdgo_mod_g1_Rule R
               WHERE R.UnitSkillID IN (ISNULL(@S1, 0), ISNULL(@S2, 0), ISNULL(@S3, 0), ISNULL(@S4, 0))), 255),
       ISNULL(@Revision, 0), CASE WHEN @r = 0 THEN ISNULL(@Cost, 0) ELSE -1 END
GO

-- GP of the next reset, the native sp_GetResetCustomSkillCost formula with the
-- loadout's own reset count and the G改 1.0 skills' reset costs: the first reset
-- is free, an operator with the custom-reset skill (SkillType 3) makes it free,
-- and the unit's ComposeSkillReset percentage discounts it. Return values: 0 ok,
-- 101 unit missing or below level 3, 102 nothing equipped.
IF OBJECT_ID(N'dbo.sdgo_mod_g1_GetResetCost', N'P') IS NOT NULL DROP PROCEDURE dbo.sdgo_mod_g1_GetResetCost;
GO
CREATE PROCEDURE dbo.sdgo_mod_g1_GetResetCost
    @v_AccountNum INT,
    @v_UserUnitNum BIGINT,
    @v_Cost INT OUTPUT,
    @v_RetVal INT OUTPUT
AS
SET NOCOUNT ON
DECLARE @UnitNumber INT, @ComposeSkillReset TINYINT, @BaseCost SMALLINT, @ResetW SMALLINT, @MaxReset SMALLINT
DECLARE @S1 SMALLINT, @S2 SMALLINT, @S3 SMALLINT, @S4 SMALLINT, @ResetCnt SMALLINT, @SkillW INT, @Count SMALLINT
SET @v_Cost = 0
SET @v_RetVal = 0
SELECT TOP 1 @UnitNumber = UU.UnitNumber, @ComposeSkillReset = UU.ComposeSkillReset,
       @BaseCost = UGI.BaseCost, @ResetW = UGI.ResetWeight, @MaxReset = UGI.MaxReset
FROM dbo.tbl_UserUnits AS UU
INNER JOIN dbo.tbl_Units AS U ON UU.UnitNumber = U.UnitNumber
INNER JOIN dbo.tbl_UnitGradeInfo AS UGI ON U.UnitGradeNumber = UGI.UnitGradeNumber AND U.UnitGradeNumber2 = UGI.UnitGradeNumber2
WHERE UU.AccountNumber = @v_AccountNum AND UU.UserUnitNumber = @v_UserUnitNum AND UU.UnitLevelNumber > 2
IF @@ROWCOUNT = 0 BEGIN SET @v_RetVal = 101; RETURN; END
SELECT TOP 1 @S1 = Skill_1, @S2 = Skill_2, @S3 = Skill_3, @S4 = Skill_4, @ResetCnt = ResetCnt
FROM dbo.sdgo_mod_g1_UserUnitSkill
WHERE UserUnitNumber = @v_UserUnitNum AND AccountNumber = @v_AccountNum AND UnitNumber = @UnitNumber
IF ISNULL(@S1, 0) = 0 AND ISNULL(@S2, 0) = 0 AND ISNULL(@S3, 0) = 0 AND ISNULL(@S4, 0) = 0
BEGIN SET @v_RetVal = 102; RETURN; END
IF EXISTS (SELECT 1 FROM dbo.tbl_Users AS TU
           INNER JOIN dbo.tbl_UserOperators AS UO ON TU.GoUserOperatorNum = UO.UserOperatorNumber AND TU.AccountNumber = UO.AccountNumber
           INNER JOIN dbo.tbl_OperatorData AS OD ON UO.OperatorNumber = OD.OperatorID
           INNER JOIN dbo.tbl_OperatorSkill AS OS ON OD.ServiceType = OS.SkillNo
           WHERE TU.AccountNumber = @v_AccountNum AND OS.SkillType = 3)
    RETURN
IF ISNULL(@ResetCnt, 0) = 0 RETURN
SELECT @SkillW = ISNULL(SUM(ResetCost), 0) FROM dbo.sdgo_mod_g1_Rule WHERE UnitSkillID IN (@S1, @S2, @S3, @S4)
SET @Count = @ResetCnt
IF @MaxReset < @Count SET @Count = @MaxReset
SET @v_Cost = ISNULL(@BaseCost, 0) + ISNULL(@ResetW, 0) * @Count + ISNULL(@SkillW, 0)
IF ISNULL(@ComposeSkillReset, 0) > 99 SET @v_Cost = 0
ELSE IF ISNULL(@ComposeSkillReset, 0) > 0 SET @v_Cost = CONVERT(INT, @v_Cost * (CONVERT(REAL, 100 - @ComposeSkillReset) / 100))
IF @v_Cost < 0 SET @v_Cost = 0
GO

-- Equips skills, the native sp_SetUserUnitCustomSkill rules on the G改 1.0 rule
-- set: a filled slot keeps its skill until a reset, no skill twice, every skill
-- in the rule set within the unit and user levels, one category for all, and
-- slots 3 and 4 only with Normal_MaxLv 3 and 4. @v_Revision 0 skips the stale
-- check. Return values: see G1Code in oc_custom_protocol.h.
IF OBJECT_ID(N'dbo.sdgo_mod_g1_Set', N'P') IS NOT NULL DROP PROCEDURE dbo.sdgo_mod_g1_Set;
GO
CREATE PROCEDURE dbo.sdgo_mod_g1_Set
    @v_AccountNum INT,
    @v_UserUnitNum BIGINT,
    @v_Skill_1 SMALLINT,
    @v_Skill_2 SMALLINT,
    @v_Skill_3 SMALLINT,
    @v_Skill_4 SMALLINT,
    @v_Revision INT,
    @v_RetVal INT OUTPUT
AS
SET NOCOUNT ON
DECLARE @UnitNumber INT, @UnitLv TINYINT, @NormalMaxLv TINYINT, @UserLv TINYINT
DECLARE @S1 SMALLINT, @S2 SMALLINT, @S3 SMALLINT, @S4 SMALLINT, @Revision INT, @Exists BIT
DECLARE @Categories INT, @Known INT, @Equipped INT
SET @v_RetVal = 0
SELECT TOP 1 @UnitNumber = UnitNumber, @UnitLv = UnitLevelNumber, @NormalMaxLv = Normal_MaxLv
FROM dbo.tbl_UserUnits WHERE AccountNumber = @v_AccountNum AND UserUnitNumber = @v_UserUnitNum
IF @@ROWCOUNT = 0 BEGIN SET @v_RetVal = 2002; RETURN; END
SELECT TOP 1 @UserLv = UserLevelNumber FROM dbo.tbl_Users WHERE AccountNumber = @v_AccountNum
IF @@ROWCOUNT = 0 BEGIN SET @v_RetVal = 2002; RETURN; END
SET @Exists = 0
SELECT TOP 1 @S1 = Skill_1, @S2 = Skill_2, @S3 = Skill_3, @S4 = Skill_4, @Revision = Revision, @Exists = 1
FROM dbo.sdgo_mod_g1_UserUnitSkill
WHERE UserUnitNumber = @v_UserUnitNum AND AccountNumber = @v_AccountNum AND UnitNumber = @UnitNumber
SET @S1 = ISNULL(@S1, 0) SET @S2 = ISNULL(@S2, 0) SET @S3 = ISNULL(@S3, 0) SET @S4 = ISNULL(@S4, 0)
SET @v_Skill_1 = ISNULL(@v_Skill_1, 0) SET @v_Skill_2 = ISNULL(@v_Skill_2, 0)
SET @v_Skill_3 = ISNULL(@v_Skill_3, 0) SET @v_Skill_4 = ISNULL(@v_Skill_4, 0)
IF ISNULL(@v_Revision, 0) <> 0 AND ISNULL(@Revision, 0) <> @v_Revision BEGIN SET @v_RetVal = 2010; RETURN; END
IF (@S1 > 0 AND @v_Skill_1 <> @S1) OR (@S2 > 0 AND @v_Skill_2 <> @S2) OR
   (@S3 > 0 AND @v_Skill_3 <> @S3) OR (@S4 > 0 AND @v_Skill_4 <> @S4)
BEGIN SET @v_RetVal = 2003; RETURN; END
IF (@v_Skill_1 > 0 AND @v_Skill_1 IN (@v_Skill_2, @v_Skill_3, @v_Skill_4)) OR
   (@v_Skill_2 > 0 AND @v_Skill_2 IN (@v_Skill_3, @v_Skill_4)) OR
   (@v_Skill_3 > 0 AND @v_Skill_3 = @v_Skill_4)
BEGIN SET @v_RetVal = 2004; RETURN; END
IF (@v_Skill_3 > 0 AND ISNULL(@NormalMaxLv, 0) < 3) OR (@v_Skill_4 > 0 AND ISNULL(@NormalMaxLv, 0) < 4)
BEGIN SET @v_RetVal = 2007; RETURN; END
SELECT @Equipped = COUNT(*) FROM (VALUES (@v_Skill_1), (@v_Skill_2), (@v_Skill_3), (@v_Skill_4)) AS V(Skill) WHERE V.Skill > 0
SELECT @Known = COUNT(*), @Categories = COUNT(DISTINCT R.Category)
FROM (VALUES (@v_Skill_1), (@v_Skill_2), (@v_Skill_3), (@v_Skill_4)) AS V(Skill)
INNER JOIN dbo.sdgo_mod_g1_Rule AS R ON R.UnitSkillID = V.Skill
WHERE V.Skill > 0 AND R.UseUnitLv <= @UnitLv AND R.UseUserLv <= ISNULL(@UserLv, 0)
IF @Known <> @Equipped BEGIN SET @v_RetVal = 2005; RETURN; END
IF @Categories > 1 BEGIN SET @v_RetVal = 2006; RETURN; END
IF @Exists = 1
    UPDATE dbo.sdgo_mod_g1_UserUnitSkill
    SET Skill_1 = @v_Skill_1, Skill_2 = @v_Skill_2, Skill_3 = @v_Skill_3, Skill_4 = @v_Skill_4,
        Revision = Revision + 1, UpdatedAt = GETDATE()
    WHERE UserUnitNumber = @v_UserUnitNum AND AccountNumber = @v_AccountNum AND UnitNumber = @UnitNumber
ELSE
BEGIN
    DELETE FROM dbo.sdgo_mod_g1_UserUnitSkill WHERE UserUnitNumber = @v_UserUnitNum
    INSERT INTO dbo.sdgo_mod_g1_UserUnitSkill (UserUnitNumber, AccountNumber, UnitNumber, Skill_1, Skill_2, Skill_3, Skill_4)
    VALUES (@v_UserUnitNum, @v_AccountNum, @UnitNumber, @v_Skill_1, @v_Skill_2, @v_Skill_3, @v_Skill_4)
END
GO

-- Empties the loadout and counts the reset (the caller charges the GP from
-- sdgo_mod_g1_GetResetCost first). Return values: 0 ok, 2002 unit not owned,
-- 2011 nothing equipped.
IF OBJECT_ID(N'dbo.sdgo_mod_g1_Reset', N'P') IS NOT NULL DROP PROCEDURE dbo.sdgo_mod_g1_Reset;
GO
CREATE PROCEDURE dbo.sdgo_mod_g1_Reset
    @v_AccountNum INT,
    @v_UserUnitNum BIGINT,
    @v_RetVal INT OUTPUT
AS
SET NOCOUNT ON
DECLARE @UnitNumber INT
SET @v_RetVal = 0
SELECT TOP 1 @UnitNumber = UnitNumber FROM dbo.tbl_UserUnits WHERE AccountNumber = @v_AccountNum AND UserUnitNumber = @v_UserUnitNum
IF @@ROWCOUNT = 0 BEGIN SET @v_RetVal = 2002; RETURN; END
UPDATE dbo.sdgo_mod_g1_UserUnitSkill
SET Skill_1 = 0, Skill_2 = 0, Skill_3 = 0, Skill_4 = 0, ResetCnt = ResetCnt + 1, Revision = Revision + 1, UpdatedAt = GETDATE()
WHERE UserUnitNumber = @v_UserUnitNum AND AccountNumber = @v_AccountNum AND UnitNumber = @UnitNumber
  AND (Skill_1 <> 0 OR Skill_2 <> 0 OR Skill_3 <> 0 OR Skill_4 <> 0)
IF @@ROWCOUNT = 0 SET @v_RetVal = 2011
GO
