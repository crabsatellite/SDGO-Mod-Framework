        -- The current native catalog is the pool. Roots is form metadata, not
        -- a frozen list of prizes: new standalone/ordinary transform families
        -- participate automatically. Reciprocal ownable forms use verified
        -- native/import metadata and only that family's source must still match.
        DECLARE @reach TABLE(StartID int,NodeID int,PRIMARY KEY(StartID,NodeID));
        ;WITH paths AS (
            SELECT UnitNumber AS StartID,UnitNumber AS NodeID,
                   CAST('/'+CONVERT(varchar(11),UnitNumber)+'/' AS varchar(max)) AS Visited
            FROM dbo.tbl_Units WITH(HOLDLOCK)
            UNION ALL
            SELECT p.StartID,n.UnitNumber,
                   CAST(p.Visited+CONVERT(varchar(11),n.UnitNumber)+'/' AS varchar(max))
            FROM paths p JOIN dbo.tbl_Units u ON u.UnitNumber=p.NodeID
            JOIN dbo.tbl_Units n ON n.UnitNumber=u.UnitNumber_Transform
            WHERE CHARINDEX('/'+CONVERT(varchar(11),n.UnitNumber)+'/',p.Visited)=0
        )
        INSERT @reach SELECT StartID,NodeID FROM paths OPTION(MAXRECURSION 32767);
        DECLARE @ownable TABLE(UnitID int PRIMARY KEY,UnitRank tinyint,UnitSubgrade tinyint);
        INSERT @ownable
        SELECT UnitNumber,UnitGradeNumber,UnitGradeNumber2 FROM dbo.tbl_Units
        WHERE SallyReady=1 AND UnitGradeNumber BETWEEN 1 AND 4
          AND UnitGradeNumber2 IN(2,3,4,5)
          AND (NOT(LOWER(LTRIM(RTRIM(UnitModelName)))='name' AND UnitGraph=1010001)
               OR EXISTS(SELECT 1 FROM sdgo_collection_capsule.Roots root
                         JOIN sdgo_collection_capsule.UnitSource source ON source.UnitID=root.UnitID
                         WHERE root.UnitID=dbo.tbl_Units.UnitNumber
                           AND source.TransformTo=ISNULL(dbo.tbl_Units.UnitNumber_Transform,0)
                           AND source.Ready=dbo.tbl_Units.SallyReady
                           AND source.UnitRank=dbo.tbl_Units.UnitGradeNumber
                           AND source.UnitSubgrade=dbo.tbl_Units.UnitGradeNumber2
                           AND source.UnitGraph=dbo.tbl_Units.UnitGraph));
        DECLARE @bases TABLE(UnitID int PRIMARY KEY,UnitRank tinyint);
        INSERT @bases(UnitID,UnitRank)
        SELECT u.UnitID,u.UnitRank FROM @ownable u
        WHERE NOT EXISTS(
            SELECT 1 FROM @reach r JOIN @ownable other ON other.UnitID=r.StartID
            WHERE r.NodeID=u.UnitID AND other.UnitID<>u.UnitID
              AND NOT EXISTS(SELECT 1 FROM @reach back WHERE back.StartID=u.UnitID AND back.NodeID=other.UnitID))
          AND (
            NOT EXISTS(SELECT 1 FROM @reach r JOIN @ownable other ON other.UnitID=r.NodeID
                       WHERE r.StartID=u.UnitID AND other.UnitID<>u.UnitID
                         AND EXISTS(SELECT 1 FROM @reach back WHERE back.StartID=other.UnitID AND back.NodeID=u.UnitID))
            OR (EXISTS(SELECT 1 FROM sdgo_collection_capsule.Roots root WHERE root.UnitID=u.UnitID)
                AND NOT EXISTS(
                    SELECT 1 FROM @reach family JOIN dbo.tbl_Units live ON live.UnitNumber=family.NodeID
                    LEFT JOIN sdgo_collection_capsule.UnitSource source ON source.UnitID=live.UnitNumber
                    WHERE family.StartID=u.UnitID AND (source.UnitID IS NULL
                      OR ISNULL(live.UnitNumber_Transform,0)<>source.TransformTo
                      OR live.SallyReady<>source.Ready OR live.UnitGradeNumber<>source.UnitRank
                      OR live.UnitGradeNumber2<>source.UnitSubgrade OR live.UnitGraph<>source.UnitGraph)))
          )
        ;
        -- Owner exclusions remove every drawable base whose own transform chain
        -- contains a listed form. The draw and the catalog explanation share this.
        DECLARE @eligible TABLE(UnitID int PRIMARY KEY,UnitRank tinyint);
        INSERT @eligible(UnitID,UnitRank)
        SELECT b.UnitID,b.UnitRank FROM @bases b
        WHERE NOT EXISTS(SELECT 1 FROM @reach family JOIN sdgo_collection_capsule.ExcludedUnits excluded
                         ON excluded.UnitID=family.NodeID WHERE family.StartID=b.UnitID);
        -- A newly imported reciprocal family must register its initial form.
        -- Surface missing/conflicting metadata instead of silently hiding it.
        DECLARE @poolError bit=CASE WHEN EXISTS(SELECT 1 FROM @ownable u
            WHERE EXISTS(SELECT 1 FROM @reach r JOIN @ownable other ON other.UnitID=r.NodeID
                         WHERE r.StartID=u.UnitID AND other.UnitID<>u.UnitID
                           AND EXISTS(SELECT 1 FROM @reach back WHERE back.StartID=other.UnitID AND back.NodeID=u.UnitID))
              AND NOT EXISTS(SELECT 1 FROM @reach incoming JOIN @ownable prior ON prior.UnitID=incoming.StartID
                             WHERE incoming.NodeID=u.UnitID AND prior.UnitID<>u.UnitID
                               AND NOT EXISTS(SELECT 1 FROM @reach back WHERE back.StartID=u.UnitID AND back.NodeID=prior.UnitID))
              AND (SELECT COUNT(*) FROM @bases chosen JOIN @reach members ON members.NodeID=chosen.UnitID
                   WHERE members.StartID=u.UnitID)<>1
              AND NOT EXISTS(SELECT 1 FROM @reach family JOIN sdgo_collection_capsule.ExcludedUnits excluded
                             ON excluded.UnitID=family.NodeID WHERE family.StartID=u.UnitID)) THEN 1 ELSE 0 END;
