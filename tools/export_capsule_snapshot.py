"""Export non-user capsule/acquisition configuration using an existing server INI.

Read-only SELECTs. Credentials remain in the supplied INI and are never exported.
Requires a locally installed SQL Server ODBC driver and pyodbc on the host only.
"""
import argparse
import configparser
import datetime
import hashlib
import json
from pathlib import Path


TABLES = {
    "GOnlineGame": ["tbl_Units", "tbl_ItemCategory", "tbl_MachineList", "tbl_MachinePrice",
        "tbl_MachineDice", "tbl_CapsuleList1", "tbl_CapsulePrime", "tbl_CoinMachineList",
        "tbl_CoinMachinePrice", "tbl_CoinMachineDice", "tbl_CoinCapsuleList", "tbl_CoinCapsulePrime",
        "tbl_CustomCapsuleList", "tbl_CustomCapsuleInfo", "tbl_MixTreeFile", "tbl_MixTreeInfo",
        "tbl_MixTreeList", "tbl_QuestInfo", "tbl_QuestPrerequest", "tbl_BattleResultItemDropDice",
        "tbl_BattleResultItemDropSetting"],
    "GDStoreBilling": ["tbls_StoreItems", "tbls_SubItems"],
}


def export_snapshot(server_ini, output, *, databases=None):
    """Read the selected host's non-player definitions for owner installation."""
    server_ini, output = Path(server_ini), Path(output)
    databases = set(TABLES) if databases is None else set(databases)
    if not databases or databases - TABLES.keys():
        raise ValueError('Unsupported capsule configuration database scope')
    import pyodbc
    from manager.engines.capsules.capsule_deployment import _read_database_settings
    drivers = [d for d in pyodbc.drivers() if "SQL Server" in d]
    if not drivers:
        raise ValueError("No local SQL Server ODBC driver")
    output.mkdir(parents=True, exist_ok=False)
    def escape(value):
        return "{" + str(value).replace("}", "}}") + "}"
    meta = {"schema_version": 1, "exported_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "scope": "read-only non-user configuration", "files": {}}
    for role, names in TABLES.items():
        if role not in databases:
            continue
        db = _read_database_settings(server_ini, "DB Game" if role == "GOnlineGame" else "DB StoreBilling")
        connection = (f"DRIVER={escape(drivers[-1])};SERVER={escape(db['DBServerName'])};"
                      f"DATABASE={escape(db['DBName'])};UID={escape(db['ID'])};PWD={escape(db['PWD'])};")
        try:
            cn = pyodbc.connect(connection, timeout=5, autocommit=True)
        except pyodbc.Error as exc:
            raise ValueError(f"SQL connection unavailable, SQLSTATE={exc.args[0]}") from None
        try:
            cn.timeout = 30
            cur = cn.cursor()
            target = output / role
            target.mkdir()
            for name in names:
                cur.execute(f"SELECT * FROM dbo.[{name}]")
                keys = [d[0] for d in cur.description]
                rows = [dict(zip(keys, r)) for r in cur.fetchall()]
                raw = json.dumps(rows, ensure_ascii=False, default=str, allow_nan=False).encode("utf-8")
                path = target / f"dbo.{name}.json"
                path.write_bytes(raw)
                meta["files"][path.relative_to(output).as_posix()] = {
                    "rows": len(rows), "sha256": hashlib.sha256(raw).hexdigest()}
        finally:
            cn.close()
    (output / "_meta.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    return meta


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--server-ini", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    meta = export_snapshot(args.server_ini, args.output)
    for name, info in meta['files'].items():
        print(f"{name}: {info['rows']}")


if __name__ == "__main__":
    main()
