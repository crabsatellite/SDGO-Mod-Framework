CREATE PROCEDURE sdgo_collection_capsule.EligibleRoots
AS
BEGIN
    SET NOCOUNT ON;
    -- Read-only view of exactly the families the draw can award. Manager
    -- reviews and the native catalog explanation use this same pool.
    -- SDGO_COLLECTION_POOL --
    SELECT CONVERT(bit,@poolError) AS PoolError,
           (SELECT COUNT(*) FROM @bases) AS Bases,
           (SELECT COUNT(*) FROM @eligible) AS Eligible,
           (SELECT COUNT(*) FROM sdgo_collection_capsule.ExcludedUnits) AS ExcludedForms;
    SELECT b.UnitID,b.UnitRank,
           CONVERT(bit,CASE WHEN e.UnitID IS NULL THEN 1 ELSE 0 END) AS Excluded,
           CONVERT(bit,CASE WHEN EXISTS(SELECT 1 FROM dbo.tbl_ItemCategory c
                WHERE c.RefIndex=b.UnitID AND c.CatalogueID/100000=11 AND c.TimeLimit=0
                  AND c.Option1=1 AND c.Option2=2) THEN 1 ELSE 0 END) AS Deliverable
    FROM @bases b LEFT JOIN @eligible e ON e.UnitID=b.UnitID
    ORDER BY b.UnitID;
END
