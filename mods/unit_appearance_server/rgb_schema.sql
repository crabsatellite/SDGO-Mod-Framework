-- Framework-owned exact RGB metadata; no account or original game data.
SET NOCOUNT ON;
SET XACT_ABORT ON;
BEGIN TRANSACTION;
DECLARE @lock int;
EXEC @lock=sys.sp_getapplock @Resource=N'sdgo_mod_rgb_paint_schema',
    @LockMode='Exclusive',@LockOwner='Transaction',@LockTimeout=5000;
IF @lock<0 BEGIN ROLLBACK; RAISERROR('RGB paint schema lock unavailable',16,1); RETURN; END;
IF OBJECT_ID(N'dbo.sdgo_mod_rgb_paint',N'U') IS NULL
    CREATE TABLE dbo.sdgo_mod_rgb_paint (
        UserUnitNumber bigint NOT NULL PRIMARY KEY,
        UnitNumber int NOT NULL,
        Revision bigint NOT NULL CHECK(Revision>0),
        NativeColors binary(12) NOT NULL,
        Colors binary(24) NULL
    );
COMMIT;
