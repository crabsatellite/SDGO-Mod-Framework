SET NOCOUNT ON;
SET XACT_ABORT ON;
IF OBJECT_ID(N'dbo.SdgoBattleGatheringPeriod',N'U') IS NULL
 CREATE TABLE dbo.SdgoBattleGatheringPeriod(
  AccountNumber int NOT NULL,RuleId varchar(48) NOT NULL,PeriodKind varchar(5) NOT NULL,
  PeriodKey nvarchar(128) NOT NULL,MatchId nvarchar(128) NOT NULL,
  Gb int NOT NULL,Mb int NOT NULL,AppliedUtc datetime2(3) NOT NULL,
  PRIMARY KEY(AccountNumber,RuleId,PeriodKind,PeriodKey));
IF OBJECT_ID(N'dbo.SdgoBattleGatheringReceipt',N'U') IS NULL
 CREATE TABLE dbo.SdgoBattleGatheringReceipt(
  MatchId nvarchar(128) NOT NULL,AccountNumber int NOT NULL,RuleId varchar(48) NOT NULL,
  SourceHash char(64) NOT NULL,PolicyRevision char(64) NOT NULL,
  PeriodKind varchar(5) NOT NULL,PeriodKey nvarchar(128) NOT NULL,
  RequestedGb int NOT NULL,RequestedMb int NOT NULL,Gb int NOT NULL,Mb int NOT NULL,
  AppliedUtc datetime2(3) NOT NULL,PRIMARY KEY(MatchId,AccountNumber,RuleId));
GO
IF OBJECT_ID(N'dbo.SdgoBattleGatheringApply',N'P') IS NULL
 EXEC(N'CREATE PROCEDURE dbo.SdgoBattleGatheringApply AS RETURN 0');
GO
ALTER PROCEDURE dbo.SdgoBattleGatheringApply
 @MatchId nvarchar(128),@AccountNumber int,@ExpectedScope uniqueidentifier,
 @SourceHash char(64),@PolicyRevision char(64),@RuleId varchar(48),
 @PeriodKind varchar(5),@PeriodKey nvarchar(128),@Gb int,@Mb int
AS
BEGIN
 SET NOCOUNT ON;SET XACT_ABORT ON;
 IF @MatchId IS NULL OR LEN(@MatchId) NOT BETWEEN 1 AND 128
  OR @MatchId COLLATE Latin1_General_100_BIN2 LIKE N'%[^-0-9A-Za-z_.:]%'
  OR @AccountNumber IS NULL OR @AccountNumber<=0 OR @ExpectedScope IS NULL
  OR @SourceHash IS NULL OR LEN(@SourceHash)<>64 OR @SourceHash COLLATE Latin1_General_100_BIN2 LIKE '%[^0-9a-f]%'
  OR @PolicyRevision IS NULL OR LEN(@PolicyRevision)<>64 OR @PolicyRevision COLLATE Latin1_General_100_BIN2 LIKE '%[^0-9a-f]%'
  OR @RuleId IS NULL OR LEN(@RuleId) NOT BETWEEN 1 AND 48 OR LEFT(@RuleId,1) COLLATE Latin1_General_100_BIN2 NOT LIKE '[a-z]'
  OR @RuleId COLLATE Latin1_General_100_BIN2 LIKE '%[^-a-z0-9_]%'
  OR @PeriodKind IS NULL OR @PeriodKind NOT IN('match','day','week') OR @PeriodKey IS NULL OR LEN(@PeriodKey)=0
  OR @Gb IS NULL OR @Gb NOT BETWEEN 0 AND 1000000 OR @Mb IS NULL OR @Mb NOT BETWEEN 0 AND 100000 OR @Gb+@Mb=0
  THROW 51220,'Invalid gathering request',1;
 IF (@PeriodKind='match' AND @PeriodKey<>@MatchId)
  OR (@PeriodKind IN('day','week') AND (LEN(@PeriodKey)<>10 OR ISDATE(REPLACE(@PeriodKey,'-',''))=0
      OR @PeriodKey COLLATE Latin1_General_100_BIN2 NOT LIKE N'[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'))
  THROW 51220,'Invalid gathering period identity',1;
 BEGIN TRANSACTION;
 BEGIN TRY
  IF NOT EXISTS(SELECT 1 FROM dbo.SdgoBattleRewardWalletMeta WITH(HOLDLOCK)
   WHERE Singleton=1 AND ScopeId=@ExpectedScope AND SchemaVersion=1
    AND OriginServer=CONVERT(nvarchar(128),SERVERPROPERTY('ServerName')) AND OriginDatabase=DB_NAME())
   THROW 51221,'Gathering wallet scope changed',1;
  DECLARE @LockResult int,@LockName nvarchar(255)=N'sdgo-gathering:'+CONVERT(nvarchar(16),@AccountNumber);
  EXEC @LockResult=sys.sp_getapplock @Resource=@LockName,@LockMode='Exclusive',@LockOwner='Transaction',@LockTimeout=10000;
  IF @LockResult<0 THROW 51222,'Gathering lock unavailable',1;
  IF EXISTS(SELECT 1 FROM dbo.SdgoBattleGatheringReceipt WHERE MatchId=@MatchId AND AccountNumber=@AccountNumber AND RuleId=@RuleId)
  BEGIN
   IF NOT EXISTS(SELECT 1 FROM dbo.SdgoBattleGatheringReceipt WHERE MatchId=@MatchId AND AccountNumber=@AccountNumber AND RuleId=@RuleId
    AND SourceHash=@SourceHash AND PolicyRevision=@PolicyRevision AND PeriodKind=@PeriodKind AND PeriodKey=@PeriodKey
    AND RequestedGb=@Gb AND RequestedMb=@Mb) THROW 51223,'Gathering retry identity changed',1;
   SELECT Gb,Mb FROM dbo.SdgoBattleGatheringReceipt WHERE MatchId=@MatchId AND AccountNumber=@AccountNumber AND RuleId=@RuleId;
   COMMIT TRANSACTION;RETURN;
  END;
  DECLARE @PaidGb int=0,@PaidMb int=0;
  IF NOT EXISTS(SELECT 1 FROM dbo.SdgoBattleGatheringPeriod WHERE AccountNumber=@AccountNumber AND RuleId=@RuleId AND PeriodKind=@PeriodKind AND PeriodKey=@PeriodKey)
  BEGIN
   DECLARE @BeforeGb int,@BeforeMb int,@AfterGb int=-1,@AfterMb int=-1,@Ret int=-1;
   SELECT @BeforeGb=f_GP,@BeforeMb=f_GC FROM dbo.tbls_AccountPocket WITH(UPDLOCK,HOLDLOCK) WHERE f_idx=@AccountNumber;
   IF @BeforeGb IS NULL OR @BeforeMb IS NULL OR @BeforeGb<0 OR @BeforeMb<0
    OR @BeforeGb>2147483647-@Gb OR @BeforeMb>2147483647-@Mb THROW 51224,'Gathering wallet unavailable or full',1;
   IF @Gb>0
   BEGIN
    EXEC dbo.sp_PocketGP_IO @type=2,@s_idx=@AccountNumber,@s_GP=@Gb,@s_from=2,@s_etc=31604,@retVal=@Ret OUTPUT,@retGP=@AfterGb OUTPUT;
    IF @Ret<>0 OR @AfterGb<>@BeforeGb+@Gb THROW 51225,'Native gathering GP credit rejected',1;
   END;
   IF @Mb>0
   BEGIN
    SET @Ret=-1;
    EXEC dbo.sp_PocketGC_IO @type=2,@s_idx=@AccountNumber,@s_GC=@Mb,@s_from=2,@s_productID=0,@retVal=@Ret OUTPUT,@retGC=@AfterMb OUTPUT;
    IF @Ret<>0 OR @AfterMb<>@BeforeMb+@Mb THROW 51226,'Native gathering GC credit rejected',1;
   END;
   SET @PaidGb=@Gb;SET @PaidMb=@Mb;
   INSERT dbo.SdgoBattleGatheringPeriod VALUES(@AccountNumber,@RuleId,@PeriodKind,@PeriodKey,@MatchId,@Gb,@Mb,SYSUTCDATETIME());
  END;
  INSERT dbo.SdgoBattleGatheringReceipt VALUES(@MatchId,@AccountNumber,@RuleId,@SourceHash,@PolicyRevision,
   @PeriodKind,@PeriodKey,@Gb,@Mb,@PaidGb,@PaidMb,SYSUTCDATETIME());
  SELECT @PaidGb AS Gb,@PaidMb AS Mb;
  COMMIT TRANSACTION;
 END TRY
 BEGIN CATCH
  IF XACT_STATE()<>0 ROLLBACK TRANSACTION;
  THROW;
 END CATCH;
END;
GO
