"""Build five explicit, reusable accounts only inside an owned template database.

These are declared fixture records. Runtime GameTest still uses native login,
inventory loading, requests and replies; it never fabricates client state.
"""
from __future__ import annotations
import configparser
import hashlib
import itertools
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
        initialize_battleships(connection,accounts,destination,isolated_template=True)
        initialize_operators(connection,accounts,isolated_template=True)
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
        fleet=account.get('battleship')
        if fleet:
            ship=cursor.execute('SELECT SelectPointUnit,UserUnitNumber_1,UserUnitNumber_2,'
                'UserUnitNumber_3,UserUnitNumber_4 FROM GOnlineGame.dbo.tbl_UserBattleShips '
                'WHERE AccountNumber=? AND UserBattleShipNumber=?',account['account_id'],fleet['uid']).fetchone()
            go=cursor.execute('SELECT GoUserBattleShipNumber FROM GOnlineGame.dbo.tbl_Users WHERE AccountNumber=?',account['account_id']).fetchone()
            padded=fleet['deck_uids']+[0]*(4-len(fleet['deck_uids']))
            if (ship is None or int(ship[0])!=account['primary_uid'] or
                [int(v) for v in ship[1:]]!=padded or int(go[0])!=fleet['uid']):
                raise RuntimeError('Fixture battleship, leader or owned deck units do not match.')
        operator=account.get('operator')
        if operator:
            row=cursor.execute('SELECT P.GoUserOperatorNum,U.OperatorNumber,U.RunOut,U.Forbidden '
                'FROM GOnlineGame.dbo.tbl_Users P LEFT JOIN GOnlineGame.dbo.tbl_UserOperators U '
                'ON U.AccountNumber=P.AccountNumber AND U.UserOperatorNumber=P.GoUserOperatorNum '
                'WHERE P.AccountNumber=?',account['account_id']).fetchone()
            if (row is None or int(row[0] or 0)!=operator['uid'] or row[1]!=operator['operator_id'] or
                    row[2]!=0 or row[3]!=0):
                raise RuntimeError('Fixture selected operator does not match an owned usable native record.')


def initialize_selected_operators(connection,accounts,*,isolated_template=False):
    """Select an already-owned stock operator; never grant or copy holdings."""
    if not isolated_template:raise RuntimeError('Operator initialization requires an isolated template revision.')
    verify_accounts(connection,accounts)
    cursor=connection.cursor()
    account_count=cursor.execute('SELECT COUNT(*) FROM GDCommon.dbo.tbl_Account').fetchone()[0]
    for account in accounts:
        who=account['account_id']
        count=cursor.execute('SELECT COUNT(*) FROM GOnlineGame.dbo.tbl_UserOperators WHERE AccountNumber=?',who).fetchone()[0]
        selected=int(cursor.execute('SELECT GoUserOperatorNum FROM GOnlineGame.dbo.tbl_Users WHERE AccountNumber=?',who).fetchone()[0] or 0)
        row=cursor.execute('SELECT TOP 1 U.UserOperatorNumber,U.OperatorNumber FROM GOnlineGame.dbo.tbl_UserOperators U '
            'JOIN GOnlineGame.dbo.tbl_OperatorData D ON D.OperatorID=U.OperatorNumber '
            'WHERE U.AccountNumber=? AND U.RunOut=0 AND U.Forbidden=0 AND '
            '(U.DateEnd IS NULL OR U.DateEnd>GETDATE()) AND '
            '((?<>0 AND U.UserOperatorNumber=?) OR (?=0 AND U.OperatorNumber IN (60001,60003))) '
            'ORDER BY CASE WHEN U.OperatorNumber=60001 THEN 0 ELSE 1 END,U.UserOperatorNumber',who,selected,selected,selected).fetchone()
        if row is None:raise RuntimeError('The fixture has no usable owned stock operator to select.')
        uid,identity=int(row[0]),int(row[1])
        if not selected:
            cursor.execute('DECLARE @op int,@category int,@slot int,@nickname varchar(16),@got datetime,@ends datetime,'
                '@shop tinyint,@runout tinyint,@forbidden tinyint,@kind tinyint,@item int,@named tinyint,@remaining smallint,@rc int;'
                'EXEC @rc=GOnlineGame.dbo.sp_SetUserOperator_GoOut ?,?,@op OUTPUT,@category OUTPUT,@slot OUTPUT,'
                '@nickname OUTPUT,@got OUTPUT,@ends OUTPUT,@shop OUTPUT,@runout OUTPUT,@forbidden OUTPUT,@kind OUTPUT,'
                '@item OUTPUT,@named OUTPUT,@remaining OUTPUT;SELECT @rc AS fixture_operator_select,@op AS operator_id;',who,uid)
            selected_result=result(cursor,'fixture_operator_select')
            if selected_result['fixture_operator_select']!=0 or selected_result['operator_id']!=identity:
                raise RuntimeError('Native operator selection rejected the existing fixture holding.')
        account['operator']={'uid':uid,'operator_id':identity}
        if cursor.execute('SELECT COUNT(*) FROM GOnlineGame.dbo.tbl_UserOperators WHERE AccountNumber=?',who).fetchone()[0]!=count:
            raise RuntimeError('Operator initialization changed the existing holdings.')
    verify_accounts(connection,accounts)
    if cursor.execute('SELECT COUNT(*) FROM GDCommon.dbo.tbl_Account').fetchone()[0]!=account_count:
        raise RuntimeError('Operator initialization created or removed an account.')


def choose_fleet(ships,primary,available,stars):
    # Large MAs can consume most of a native fleet's capacity. Preserve its
    # actual Star value and choose the largest legal deck, never inflate caps.
    for count in range(min(4,len(available)+1),0,-1):
        for ship in ships:
            for rest in itertools.combinations(available,count-1):
                deck=(primary,*rest)
                cost=sum(stars[u['unit_id']] for u in deck)
                if int(ship[2])<=cost<=int(ship[3]):return ship,deck
    return None


def initialize_operators(connection,accounts,*,isolated_template=False):
    return initialize_selected_operators(connection,accounts,isolated_template=isolated_template)


def initialize_battleships(connection,accounts,profiles,*,isolated_template=False):
    """Native starter fleets, only during explicit offline template creation/revision."""
    if not isolated_template:raise RuntimeError('Shared fleet initialization requires an isolated template revision.')
    verify_accounts(connection,accounts)
    cursor=connection.cursor()
    account_count=cursor.execute('SELECT COUNT(*) FROM GDCommon.dbo.tbl_Account').fetchone()[0]
    for account in accounts:
        who=account['account_id']
        count=cursor.execute('SELECT COUNT(*) FROM GOnlineGame.dbo.tbl_UserBattleShips WHERE AccountNumber=?',who).fetchone()[0]
        if not count:
            cursor.execute('DECLARE @r int;EXEC @r=GOnlineGame.dbo.sp_CreateBattleShipTutorial ?;SELECT @r AS fixture_ships;',who)
            if result(cursor,'fixture_ships')['fixture_ships']!=0:
                raise RuntimeError('Native starter battleship creation failed.')
        ships=cursor.execute('SELECT u.UserBattleShipNumber,u.BattleShipNumber,d.MinCost,d.MaxCost '
            'FROM GOnlineGame.dbo.tbl_UserBattleShips u JOIN GOnlineGame.dbo.tbl_BattleShipData d '
            'ON u.BattleShipNumber=d.BattleShipNumber WHERE u.AccountNumber=? ORDER BY u.BattleShipNumber,u.UserBattleShipNumber',who).fetchall()
        stars={int(r[0]):int(r[1]) for r in cursor.execute('SELECT u.UnitNumber,d.Star FROM GOnlineGame.dbo.tbl_UserUnits u '
            'JOIN GOnlineGame.dbo.tbl_Units d ON u.UnitNumber=d.UnitNumber WHERE u.AccountNumber=?',who).fetchall()}
        primary=next(u for u in account['units'] if u['uid']==account['primary_uid'])
        available=[u for u in account['units'] if u!=primary and u['unit_id'] not in (24787,24788)]
        choice=choose_fleet(ships,primary,available,stars)
        if choice is None:raise RuntimeError(f'No legal native fleet for fixture slot {account["slot"]}; unit stars={stars}; bounds={[(int(s[1]),int(s[2]),int(s[3])) for s in ships]}')
        ship,deck=choice;uids=[u['uid'] for u in deck];padded=uids+[0]*(4-len(uids))
        cursor.execute('DECLARE @previous_unit1 bigint,@previous_unit2 bigint,@previous_unit3 bigint,@previous_unit4 bigint,'
            '@previous_count1 smallint,@previous_count2 smallint,@previous_count3 smallint,@previous_count4 smallint,@fixture_result int; '
            'EXEC GOnlineGame.dbo.sp_SetUserBattleShipData ?,?,?,?,?,?,?,?,@previous_unit1 OUTPUT,@previous_unit2 OUTPUT,@previous_unit3 OUTPUT,@previous_unit4 OUTPUT,'
            '@previous_count1 OUTPUT,@previous_count2 OUTPUT,@previous_count3 OUTPUT,@previous_count4 OUTPUT,@fixture_result OUTPUT;SELECT @fixture_result AS fixture_deck;',who,int(ship[0]),account['primary_uid'],*padded,len(uids))
        status=result(cursor,'fixture_deck')['fixture_deck']
        if status!=0:raise RuntimeError(f'Native fleet assignment refused fixture slot {account["slot"]}: {status}')
        cursor.execute('DECLARE @n int,@u bigint,@r int;EXEC GOnlineGame.dbo.sp_SetUserBattleShip_GoOut ?,?,@n OUTPUT,@u OUTPUT,@r OUTPUT;'
                       'SELECT @r AS fixture_go,@u AS leader;',who,int(ship[0]))
        ready=result(cursor,'fixture_go')
        if ready['fixture_go']!=0 or int(ready['leader'])!=account['primary_uid']:
            raise RuntimeError('Native fleet go-out did not preserve the declared primary unit.')
        account['battleship']={'uid':int(ship[0]),'unit_id':int(ship[1]),'deck_uids':uids,'leader_uid':account['primary_uid']}
        path=profiles/account['profile'];profile=configparser.ConfigParser(interpolation=None)
        profile.read(path,encoding='utf-8')
        if len(uids)==4:profile['deck']={f'unit_uid{i}':str(uid) for i,uid in enumerate(uids)}
        elif profile.has_section('deck'):profile.remove_section('deck')
        with path.open('w',encoding='utf-8') as stream:profile.write(stream)
    verify_accounts(connection,accounts)
    if cursor.execute('SELECT COUNT(*) FROM GDCommon.dbo.tbl_Account').fetchone()[0]!=account_count:
        raise RuntimeError('Shared fleet initialization created or removed an account.')
