SET NOCOUNT ON;
SET XACT_ABORT ON;
BEGIN TRY
 BEGIN TRANSACTION;
 DECLARE @lock int;
 EXEC @lock=sys.sp_getapplock @Resource=N'sdgo.calendar_gifts.install.v1',
  @LockMode='Exclusive',@LockOwner='Transaction',@LockTimeout=5000;
 IF @lock<0 RAISERROR('Calendar gift installation is busy',16,1);
 IF OBJECT_ID(N'dbo.tbl_Users',N'U') IS NULL OR OBJECT_ID(N'dbo.tbl_UserUnits',N'U') IS NULL
  OR OBJECT_ID(N'dbo.tbl_ItemCategory',N'U') IS NULL OR OBJECT_ID(N'dbo.tbl_Units',N'U') IS NULL
  OR OBJECT_ID(N'dbo.tbl_UserDropItemBox',N'U') IS NULL
  RAISERROR('Required native gift tables are unavailable',16,1);
 IF (OBJECT_ID(N'dbo.sdgo_collection_catalog_drop_items',N'TR') IS NOT NULL
      OR OBJECT_ID(N'dbo.sdgo_collection_catalog_units',N'TR') IS NOT NULL)
  AND ISNULL(HAS_PERMS_BY_NAME(NULL,NULL,N'VIEW SERVER STATE'),0)<>1
  RAISERROR('The installed native collection triggers require VIEW SERVER STATE on the configured existing game login',16,1);
 IF SCHEMA_ID(N'sdgo_calendar_gifts') IS NULL
 BEGIN
  EXEC(N'CREATE SCHEMA sdgo_calendar_gifts AUTHORIZATION dbo');
  EXEC sys.sp_addextendedproperty @name=N'sdgo_owner',@value=N'calendar_gifts/v1',
   @level0type=N'SCHEMA',@level0name=N'sdgo_calendar_gifts';
  CREATE TABLE sdgo_calendar_gifts.Receipt(
   EventKey varchar(64) NOT NULL,AccountNumber int NOT NULL,RuleHash char(64) NOT NULL,
   GiftDate date NOT NULL,Status varchar(16) NOT NULL,CommittedAt datetime2(3) NOT NULL,
   CONSTRAINT PK_calendar_gift_receipt PRIMARY KEY(EventKey,AccountNumber));
  CREATE TABLE sdgo_calendar_gifts.Delivery(
   EventKey varchar(64) NOT NULL,AccountNumber int NOT NULL,Ordinal int NOT NULL,
   UnitNumber int NULL,ItemCategory int NOT NULL,Amount int NOT NULL,NativeSerial bigint NOT NULL,
   CONSTRAINT PK_calendar_gift_delivery PRIMARY KEY(EventKey,AccountNumber,Ordinal),
   CONSTRAINT FK_calendar_gift_delivery_receipt FOREIGN KEY(EventKey,AccountNumber)
    REFERENCES sdgo_calendar_gifts.Receipt(EventKey,AccountNumber));
 END;
 IF ISNULL(CONVERT(nvarchar(128),(SELECT value FROM sys.extended_properties WHERE class=3
   AND major_id=SCHEMA_ID(N'sdgo_calendar_gifts') AND name=N'sdgo_owner')),N'')<>N'calendar_gifts/v1'
  RAISERROR('Calendar gift schema belongs to another owner',16,1);
 IF OBJECT_ID(N'sdgo_calendar_gifts.Receipt',N'U') IS NULL
  OR OBJECT_ID(N'sdgo_calendar_gifts.Delivery',N'U') IS NULL
  OR (SELECT COUNT(*) FROM sys.columns WHERE object_id=OBJECT_ID(N'sdgo_calendar_gifts.Receipt'))<>6
  OR (SELECT COUNT(*) FROM sys.columns WHERE object_id=OBJECT_ID(N'sdgo_calendar_gifts.Delivery'))<>7
  RAISERROR('Calendar gift schema is incompatible',16,1);
 COMMIT TRANSACTION;
END TRY
BEGIN CATCH
 IF @@TRANCOUNT>0 ROLLBACK TRANSACTION;
 THROW;
END CATCH;
