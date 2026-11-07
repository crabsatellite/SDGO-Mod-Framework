ALTER TRIGGER dbo.sdgo_mod_InventoryDeleteGuard
ON dbo.tbl_UserUnits
AFTER DELETE
AS
BEGIN
 SET NOCOUNT ON;
 IF NOT EXISTS(SELECT 1 FROM deleted) RETURN;
 IF (SELECT COUNT(DISTINCT AccountNumber) FROM deleted)>1
 BEGIN
   RAISERROR('Cross-account unit deletion refused',16,1);
   ROLLBACK TRANSACTION;
   RETURN;
 END;
 INSERT dbo.sdgo_mod_HangarUnitDeleteAudit
   (DeletedUtc,SessionId,ClientApplication,ClientHost,__ALL_COLUMNS__)
 SELECT SYSUTCDATETIME(),@@SPID,
   LEFT(COALESCE(APP_NAME(),N''),128),
   LEFT(COALESCE(HOST_NAME(),N''),128),
   __DELETED_COLUMNS__
 FROM deleted AS D;
END;
