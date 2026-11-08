CREATE PROCEDURE sdgo_collection_capsule.Draw
    @v_AccountNum int,
    @v_MachineID int
AS
BEGIN
    SET NOCOUNT ON;
    SET XACT_ABORT ON;
    -- The stock World path has already charged the registered GP price. This
    -- procedure owns delivery only; failure returns through native compensation.
    IF @@TRANCOUNT=0 RETURN 3002;
    BEGIN TRY
        DECLARE @lock int,@resource nvarchar(128),@dummy int;
        SET @resource=N'sdgo.collection_capsule.account.'+CONVERT(nvarchar(20),@v_AccountNum);
        EXEC @lock=sys.sp_getapplock @Resource=@resource,@LockMode='Exclusive',
             @LockOwner='Transaction',@LockTimeout=5000;
        IF @lock<0 BEGIN RETURN 3002;END;
        IF NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.State WITH(HOLDLOCK)
                      WHERE Singleton=1 AND SchemaVersion=3 AND Enabled=1 AND MachineID=@v_MachineID AND PriceGP BETWEEN 1 AND 1000000000)
        BEGIN RETURN 3002;END;
        DECLARE @period varchar(4),@limit int,@offset int,@periodStart date;
        SELECT @period=LimitPeriod,@limit=DrawLimit,@offset=UtcOffsetMinutes FROM sdgo_collection_capsule.State WITH(HOLDLOCK) WHERE Singleton=1;
        IF @period NOT IN('none','day','week') OR @limit NOT BETWEEN 0 AND 100000 OR @offset NOT BETWEEN -720 AND 840 RETURN 3002;
        SET @periodStart=CONVERT(date,DATEADD(minute,@offset,GETUTCDATE()));
        IF @period='week' SET @periodStart=DATEADD(day,-(DATEDIFF(day,CONVERT(date,'19000101'),@periodStart)%7),@periodStart);
        IF @period<>'none' AND @limit>0 AND EXISTS(
          SELECT 1 FROM sdgo_collection_capsule.DrawQuota WITH(UPDLOCK,HOLDLOCK)
          WHERE AccountNumber=@v_AccountNum AND MachineID=@v_MachineID AND PeriodKind=@period AND PeriodStart=@periodStart AND DrawCount>=@limit)
          RETURN 6006; -- ERR_PICK_EVENT_CAPSULE_ALREADY_USE; native caller refunds the precharge
        -- Existing profiles can draw immediately; no complete-history proof.
        IF NOT EXISTS(SELECT 1 FROM dbo.tbl_Users WITH(HOLDLOCK)
                      WHERE AccountNumber=@v_AccountNum)
        BEGIN RETURN 3002;END;
        -- Native event rewards lock the gift box before inventory/history.
        SELECT @dummy=COUNT(*) FROM dbo.tbl_UserDropItemBox WITH(UPDLOCK,HOLDLOCK)
         WHERE AccountNumber=@v_AccountNum;
        SELECT @dummy=COUNT(*) FROM dbo.tbl_UserUnits WITH(UPDLOCK,HOLDLOCK)
         WHERE AccountNumber=@v_AccountNum;
        -- SDGO_COLLECTION_POOL --
        IF @poolError=1 RETURN 3008;
        -- Catalog membership is persistent; inventory and pending gifts are
        -- current possession. Prefer missing catalog families, then missing
        -- inventory families only after the entire eligible catalog is unlocked.
        DECLARE @unheld TABLE(UnitID int PRIMARY KEY,UnitRank tinyint);
        INSERT @unheld(UnitID,UnitRank)
        SELECT u.UnitID,u.UnitRank FROM @eligible u
        WHERE NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnits owned JOIN @reach family ON family.NodeID=owned.UnitNumber
                         WHERE owned.AccountNumber=@v_AccountNum AND owned.TimeLimit=0 AND owned.RunOut=0 AND family.StartID=u.UnitID)
          AND NOT EXISTS(SELECT 1 FROM dbo.tbl_UserDropItemBox b JOIN dbo.tbl_ItemCategory c ON c.CatalogueID=b.ItemCategory
                         JOIN @reach family ON family.NodeID=c.RefIndex
                         WHERE b.AccountNumber=@v_AccountNum AND c.CatalogueID/100000=11 AND c.TimeLimit=0 AND family.StartID=u.UnitID);
        DECLARE @missingCatalog bit=CASE WHEN EXISTS(
            SELECT 1 FROM @eligible u WHERE NOT EXISTS(
                SELECT 1 FROM sdgo_collection_capsule.CatalogUnlocks unlocked WITH(HOLDLOCK)
                JOIN @reach family ON family.NodeID=unlocked.UnitID
                WHERE unlocked.AccountNumber=@v_AccountNum AND family.StartID=u.UnitID)) THEN 1 ELSE 0 END;
        DECLARE @choices TABLE(Ordinal int IDENTITY(1,1),UnitID int,UnitRank tinyint);
        INSERT @choices(UnitID,UnitRank)
        SELECT u.UnitID,u.UnitRank FROM @unheld u
        WHERE @missingCatalog=0 OR NOT EXISTS(
            SELECT 1 FROM sdgo_collection_capsule.CatalogUnlocks unlocked WITH(HOLDLOCK)
            JOIN @reach family ON family.NodeID=unlocked.UnitID
            WHERE unlocked.AccountNumber=@v_AccountNum AND family.StartID=u.UnitID)
        ORDER BY u.UnitID;
        DECLARE @count int,@random bigint,@ceiling bigint,@unit int,@rank tinyint;
        SELECT @count=COUNT(*) FROM @choices;
        IF @count=0 BEGIN RETURN 3002;END;
        IF EXISTS(SELECT 1 FROM @choices u WHERE NOT EXISTS(
            SELECT 1 FROM dbo.tbl_ItemCategory c WHERE c.RefIndex=u.UnitID AND c.CatalogueID/100000=11
              AND c.TimeLimit=0 AND c.Option1=1 AND c.Option2=2)) RETURN 3008;
        IF EXISTS(SELECT 1 FROM sdgo_collection_capsule.OwnedUnitItems o LEFT JOIN dbo.tbl_ItemCategory c ON c.CatalogueID=o.CategoryID
            WHERE c.CatalogueID IS NULL OR c.RefIndex<>o.UnitID OR c.TimeLimit<>0 OR c.Option1<>1 OR c.Option2<>2) RETURN 3008;
        IF EXISTS(SELECT 1 FROM @choices u JOIN sdgo_collection_capsule.UnitDelivery d ON d.UnitID=u.UnitID
            LEFT JOIN dbo.tbl_ItemCategory c ON c.CatalogueID=d.CategoryID
            WHERE c.CatalogueID IS NULL OR c.RefIndex<>u.UnitID OR c.TimeLimit<>0 OR c.Option1<>1 OR c.Option2<>2) RETURN 3008;
        -- Grade probabilities follow sp_DailyLoginReward: C35/B30/A20/S15.
        -- Exhausted grades leave the lottery; unit counts never multiply rank odds.
        DECLARE @grades TABLE(UnitRank tinyint PRIMARY KEY,Weight int);
        INSERT @grades SELECT w.UnitRank,w.Weight FROM (VALUES(1,35),(2,30),(3,20),(4,15)) w(UnitRank,Weight)
         WHERE EXISTS(SELECT 1 FROM @choices c WHERE c.UnitRank=w.UnitRank);
        DECLARE @total int,@ticket int;
        SELECT @total=SUM(Weight) FROM @grades;
        SET @ceiling=CONVERT(bigint,4294967296)-(CONVERT(bigint,4294967296)%@total);
        WHILE 1=1
        BEGIN
            SET @random=CONVERT(bigint,CRYPT_GEN_RANDOM(4));
            IF @random<@ceiling BREAK;
        END;
        SET @ticket=CONVERT(int,@random%@total);
        SELECT TOP 1 @rank=g.UnitRank FROM @grades g
         WHERE @ticket<(SELECT SUM(p.Weight) FROM @grades p WHERE p.UnitRank<=g.UnitRank) ORDER BY g.UnitRank;
        SELECT TOP 1 @unit=UnitID FROM @choices WHERE UnitRank=@rank ORDER BY NEWID();
        DECLARE @category int,@instance bigint;
        SELECT @category=MIN(CatalogueID) FROM dbo.tbl_ItemCategory
         WHERE RefIndex=@unit AND CatalogueID/100000=11 AND TimeLimit=0 AND Option1=1 AND Option2=2;
        -- Prefer the category proven against both client and server at build.
        -- A smaller server-only category must not override that proof.
        SELECT @category=CategoryID FROM sdgo_collection_capsule.UnitDelivery WHERE UnitID=@unit;
        IF @category IS NULL RETURN 3008;
        INSERT dbo.tbl_UserDropItemBox(AccountNumber,ItemCategory,Amount,DropConditionID)
        VALUES(@v_AccountNum,@category,1,@v_MachineID);
        SET @instance=CONVERT(bigint,SCOPE_IDENTITY());
        IF @instance IS NULL OR NOT EXISTS(SELECT 1 FROM dbo.tbl_UserDropItemBox
                    WHERE AccountNumber=@v_AccountNum AND UserDropItemIdx=@instance AND ItemCategory=@category AND Amount=1)
        BEGIN RETURN 3002;END;
        IF NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.CatalogUnlocks
                      WHERE AccountNumber=@v_AccountNum AND UnitID=@unit)
        BEGIN RETURN 3002;END;
        IF @period<>'none'
        BEGIN
          IF EXISTS(SELECT 1 FROM sdgo_collection_capsule.DrawQuota WHERE AccountNumber=@v_AccountNum AND MachineID=@v_MachineID AND PeriodKind=@period AND PeriodStart=@periodStart)
            UPDATE sdgo_collection_capsule.DrawQuota SET DrawCount=DrawCount+1 WHERE AccountNumber=@v_AccountNum AND MachineID=@v_MachineID AND PeriodKind=@period AND PeriodStart=@periodStart;
          ELSE INSERT sdgo_collection_capsule.DrawQuota VALUES(@v_AccountNum,@v_MachineID,@period,@periodStart,1);
        END;
        INSERT dbo.tbl_CapsulePicked(AccountID,MachineID,UnitGradeNumber,UnitID)
        VALUES(@v_AccountNum,@v_MachineID,@rank,@unit);
        SELECT @instance AS DropBoxSerial,@category AS ItemCategory,CONVERT(int,1) AS Amount;
        RETURN 0;
    END TRY
    BEGIN CATCH
        RETURN 3002;
    END CATCH;
END
