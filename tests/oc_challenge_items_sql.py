"""Emit read-only SQL Server checks for the production OC item definition.

Execute the output on the designated native test database. No schema, inventory,
unit, random-dice or consumable state is written. --native uses its real catalogue;
the default supplies inline definition rows to cover rejection and level bounds.
"""
from pathlib import Path
import argparse
import re


def query(native=False):
    schema = (Path(__file__).resolve().parents[1] / 'mods/oc_custom_server/oc_schema.sql').read_text(encoding='utf-8')
    match = re.search(r'CREATE VIEW dbo.sdgo_mod_oc_ChallengeItemDefinition AS\s*(SELECT.*?)\s*GO', schema, re.S)
    if not match:
        raise ValueError('production item definition not found')
    definition = match[1].strip().rstrip(';')
    catalogue = ''
    if not native:
        catalogue = '''Catalogue AS (
 SELECT * FROM (VALUES
 (3240003,9,0,0,1,18), (3240004,9,0,0,1,18),
 (3240005,3,10000,0,9,15), (3240006,3,1,0,9,12),
 (3240030,3,0,1,9,15), (3240013,9,0,0,1,18),
 (3240024,9,0,0,1,18), (990001,3,0,0,1,18),
 (990002,2,10000,1,1,18), (990003,9,10000,1,1,18)
 ) R(EtcItemNumber,[Type],ProbabilityPlus,LvPrevention,UseMinLv,UseMaxLv)
),'''
        definition = definition.replace('dbo.tbl_EtcItems', 'Catalogue')
    return f'''SET NOCOUNT ON;
;WITH {catalogue}
Definitions AS ({definition}),
Inventory AS (
 SELECT * FROM (VALUES
 (17,3240003,9,3),(17,3240004,9,7),(17,3240005,3,2),
 (17,3240006,3,4),(17,3240030,3,5),(17,3240013,9,1),
 (17,3240024,9,1),(17,990001,3,1),(17,990002,2,1),(17,990003,9,1),
 (18,3240004,9,99),(17,3240004,3,99),(17,3240004,9,0)
 ) R(AccountNumber,EtcItemNumber,ItemType,RemainCount)
), Owned AS (
 SELECT I.EtcItemNumber,I.RemainCount,D.ProbabilityPlus,D.LvPrevention,D.UseMinLv,D.UseMaxLv
 FROM Inventory I JOIN Definitions D ON I.EtcItemNumber=D.EtcItemNumber AND I.ItemType=D.ItemType
 WHERE I.AccountNumber=17 AND I.RemainCount>0
)
SELECT CASE WHEN
 (SELECT COUNT(*) FROM Owned)=5 AND
 (SELECT COUNT(*) FROM Owned WHERE EtcItemNumber=3240004 AND RemainCount=7 AND ProbabilityPlus=10000 AND LvPrevention=0)=1 AND
 (SELECT COUNT(*) FROM Owned WHERE EtcItemNumber=3240003 AND RemainCount=3 AND ProbabilityPlus=0 AND LvPrevention=1)=1 AND
 (SELECT COUNT(*) FROM Owned WHERE EtcItemNumber=3240005 AND ProbabilityPlus=10000)=1 AND
 (SELECT COUNT(*) FROM Owned WHERE EtcItemNumber=3240006 AND ProbabilityPlus=1)=1 AND
 (SELECT COUNT(*) FROM Owned WHERE EtcItemNumber=3240030 AND LvPrevention=1)=1 AND
 (SELECT COUNT(*) FROM Owned WHERE 8 BETWEEN UseMinLv AND UseMaxLv)=2 AND
 (SELECT COUNT(*) FROM Owned WHERE 9 BETWEEN UseMinLv AND UseMaxLv)=5 AND
 (SELECT COUNT(*) FROM Owned WHERE 13 BETWEEN UseMinLv AND UseMaxLv)=4 AND
 (SELECT COUNT(*) FROM Owned WHERE 19 BETWEEN UseMinLv AND UseMaxLv)=0
 THEN 1 ELSE 0 END AS Passed,
 (SELECT COUNT(*) FROM Owned) AS OwnedCount,
 (SELECT SUM(RemainCount) FROM Owned) AS TotalCount;
'''


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true')
    args = parser.parse_args()
    print(query(args.native))
