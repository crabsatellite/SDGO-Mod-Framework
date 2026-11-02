"""Build five explicit, reusable accounts only inside an owned template database.

These are declared fixture records. Runtime GameTest still uses native login,
inventory loading, requests and replies; it never fabricates client state.
"""
from __future__ import annotations
import configparser
import hashlib
from pathlib import Path
import secrets

ROSTER = (10003, 22040, 22042, 22097, 24402, 24787, 24788)
PRIMARIES = (24788, 24787, 10003, 22042, 22097)
ROLES = ('alternate-form-regression', 'normal-form-control', 'general-combat', 'ranged', 'second-player')


def result(cursor, marker):
    while True:
        if cursor.description:
            names = [c[0] for c in cursor.description]
            rows = cursor.fetchall()
            if marker in names:
                if len(rows) != 1:
                    raise RuntimeError('Native fixture procedure returned an unexpected row count.')
                return dict(zip(names, rows[0]))
        if not cursor.nextset():
            raise RuntimeError('Native fixture procedure result was not observed.')


def build_accounts(connection, destination: Path, template_id: str, *, isolated_template=False):
    if not isolated_template:
        raise RuntimeError('Fixture creation is allowed only while building an isolated template.')
    destination.mkdir(parents=True, exist_ok=False)
    suffix = hashlib.sha256(template_id.encode()).hexdigest()[:6]
    accounts = []
    cursor = connection.cursor()
    cursor.execute('SET XACT_ABORT ON;SET NOCOUNT ON;')
    try:
        categories = {}
        for unit in ROSTER:
            category_unit = 24787 if unit == 24788 else unit
            category = cursor.execute('SELECT MIN(CatalogueID) FROM GOnlineGame.dbo.tbl_ItemCategory '
                'WHERE RefIndex=? AND CatalogueID/100000=11 AND TimeLimit=0', category_unit).fetchone()[0]
            if category is None:
                # The legacy shop table predates some of the client roster.
                # Native GM/tutorial units legitimately use category zero.
                # Take only this metadata from an existing permanent unit;
                # the procedure still allocates all fixture identities anew.
                existing=cursor.execute('SELECT MIN(CategoryNumber) FROM GOnlineGame.dbo.tbl_UserUnits '
                    'WHERE UnitNumber=? AND TimeLimit=0',category_unit).fetchone()[0]
                if existing is None:
                    raise RuntimeError(f'Declared fixture unit {unit} has no verified native inventory category.')
                category=existing
            categories[unit] = category
        for slot, primary in enumerate(PRIMARIES, 1):
            username = f'gt{suffix}{slot}'
            nickname = f'GT{suffix}{slot}'
            digest = hashlib.md5(secrets.token_bytes(32)).hexdigest()
            cursor.execute('DECLARE @r int; EXEC GDCommon.dbo.sp_Account_Regist ?,?,0,1,@r OUTPUT; '
                           'SELECT @r AS fixture_registration;', username, digest)
            result(cursor, 'fixture_registration')
            row = cursor.execute('SELECT f_idx FROM GDCommon.dbo.tbl_Account WHERE f_loginId=?', username).fetchone()
            if row is None:
                raise RuntimeError('Native registration did not create the declared fixture account.')
            account = int(row[0])
            # The stock nickname table is normally filled by the nickname UI.
            # A ready-to-use template supplies that declared nickname once.
            cursor.execute('IF EXISTS(SELECT 1 FROM GDCommon.dbo.tbl_Account_Pilot WHERE f_idx=?) '
                           'UPDATE GDCommon.dbo.tbl_Account_Pilot SET f_pilot=? WHERE f_idx=?; '
                           'ELSE INSERT GDCommon.dbo.tbl_Account_Pilot(f_idx,f_pilot,f_regdate) VALUES(?,?,GETDATE());',
                           account, nickname, account, account, nickname)
            cursor.execute('DECLARE @t tinyint,@r int; EXEC GOnlineGame.dbo.sp_UpdateUserInfo '
                           '?,?,15,1,0,0,1990,0,0,?,@t OUTPUT,@r OUTPUT; '
                           'SELECT @r AS fixture_user,@t AS tutor;', account, nickname, username)
            created = result(cursor, 'fixture_user')
            if created['fixture_user'] != 0:
                raise RuntimeError('Native user initialization refused a fixture account.')
            units = []
            for unit in (primary,) + tuple(u for u in ROSTER if u != primary):
                category = categories[unit]
                cursor.execute('DECLARE @r int,@s int,@u bigint,@serial bigint,@max tinyint; '
                    'EXEC @r=GOnlineGame.dbo.sp_CreateUserUnit ?,\'\',0,?,NULL,0,?,9,0,99,'
                    '@s OUTPUT,@u OUTPUT,@serial OUTPUT,@max OUTPUT; '
                    'SELECT @r AS fixture_unit,@s AS slot,@u AS uid;', account, category, unit)
                added = result(cursor, 'fixture_unit')
                if added['fixture_unit'] != 0 or not added['uid']:
                    raise RuntimeError('Native unit creation refused a declared fixture unit.')
                units.append({'unit_id': unit, 'slot': int(added['slot']), 'uid': int(added['uid']),
                              'regression_only_form': unit == 24788})
            primary_uid = next(u['uid'] for u in units if u['unit_id'] == primary)
            cursor.execute('UPDATE GOnlineGame.dbo.tbl_Users SET TutorLv=2,GMOption=0,GoUserUnitNum=?, '
                'UserUnitKindCnt=(SELECT COUNT(DISTINCT UnitNumber) FROM GOnlineGame.dbo.tbl_UserUnits WHERE AccountNumber=?),'
                'UserUnitTotalCnt=(SELECT COUNT(*) FROM GOnlineGame.dbo.tbl_UserUnits WHERE AccountNumber=?) '
                'WHERE AccountNumber=?;', primary_uid, account, account, account)
            # Registration normally provides this row; initialize only a
            # missing fixture wallet, leaving all original accounts untouched.
            cursor.execute('IF NOT EXISTS(SELECT 1 FROM GDStoreBilling.dbo.tbls_AccountPocket WHERE f_idx=?) '
                'INSERT GDStoreBilling.dbo.tbls_AccountPocket(f_idx,f_GP,f_GC) VALUES(?,1000000,1000000); '
                'ELSE UPDATE GDStoreBilling.dbo.tbls_AccountPocket SET f_GP=1000000,f_GC=1000000 WHERE f_idx=?;',
                account, account, account)
            profile = configparser.ConfigParser(interpolation=None)
            profile['account'] = {'username': username, 'password': digest, 'password_format': 'md5'}
            profile['registration'] = {'state': 'existing_account', 'retry_count': '1', 'expect_first_fail': 'false'}
            path = destination / f'account-{slot}.ini'
            with path.open('w', encoding='utf-8') as stream:
                profile.write(stream)
            accounts.append({'slot': slot, 'role': ROLES[slot-1], 'account_id': account,
                             'nickname': nickname, 'profile': path.name,
                             'primary_unit': primary, 'primary_uid': primary_uid, 'units': units})
        verify_accounts(connection, accounts)
        connection.commit()
        return accounts
    except BaseException:
        connection.rollback()
        raise


def verify_accounts(connection, accounts):
    if len(accounts) != 5 or len({a['account_id'] for a in accounts}) != 5:
        raise RuntimeError('An environment template requires exactly five distinct fixture accounts.')
    cursor = connection.cursor()
    for account in accounts:
        rows = cursor.execute(
            'SELECT UnitNumber,UserUnitNumber FROM GOnlineGame.dbo.tbl_UserUnits WHERE AccountNumber=?',
            account['account_id']).fetchall()
        actual = {int(r[0]): int(r[1]) for r in rows}
        expected = {u['unit_id']: u['uid'] for u in account['units']}
        if len(rows)!=len(ROSTER) or actual != expected or set(actual) != set(ROSTER):
            raise RuntimeError('Fixture roster does not match the actual owned unit records.')
        row = cursor.execute('SELECT TutorLv,GoUserUnitNum FROM GOnlineGame.dbo.tbl_Users WHERE AccountNumber=?',
                             account['account_id']).fetchone()
        if row is None or row[0] != 2 or int(row[1]) != account['primary_uid']:
            raise RuntimeError('Fixture account is not initialized with its declared active unit.')
