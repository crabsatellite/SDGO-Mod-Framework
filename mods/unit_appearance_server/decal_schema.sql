-- Framework-owned cosmetic placement metadata. Native stickers and inventory
-- remain authoritative; this schema contains no account or game content data.
SET NOCOUNT ON;
SET XACT_ABORT ON;
BEGIN TRANSACTION;
DECLARE @lock int;
EXEC @lock=sys.sp_getapplock @Resource=N'sdgo_mod_surface_decal_schema',
    @LockMode='Exclusive',@LockOwner='Transaction',@LockTimeout=5000;
IF @lock<0 BEGIN ROLLBACK; RAISERROR('Surface decal schema lock unavailable',16,1); RETURN; END;
IF OBJECT_ID(N'dbo.sdgo_mod_surface_decal',N'U') IS NULL
BEGIN
    CREATE TABLE dbo.sdgo_mod_surface_decal (
        UserUnitNumber bigint NOT NULL PRIMARY KEY,
        UnitNumber int NOT NULL,
        Revision bigint NOT NULL CHECK (Revision>0),
        Sticker1 int NOT NULL, Sticker2 int NOT NULL, Sticker3 int NOT NULL,
        Placement1 varbinary(104) NULL, Placement2 varbinary(104) NULL, Placement3 varbinary(104) NULL
    );
END;
COMMIT;
