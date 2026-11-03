#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace sdgo::friend_unit_gift {

struct NativeColumn {
    const wchar_t* name;
    const wchar_t* sql_type;
    int system_type_id;
    int max_length;
    int precision;
    int scale;
    bool nullable;
    bool identity;
};

inline constexpr std::array<NativeColumn, 50> kNativeUnitColumns{{
    {L"UserUnitNumber", L"bigint", 127, 8, 19, 0, false, true},
    {L"AccountNumber", L"int", 56, 4, 10, 0, false, false},
    {L"UserUnitSlotNumber", L"int", 56, 4, 10, 0, false, false},
    {L"UnitNumber", L"int", 56, 4, 10, 0, false, false},
    {L"UnitLevelNumber", L"tinyint", 48, 1, 3, 0, false, false},
    {L"UnitKill", L"int", 56, 4, 10, 0, false, false},
    {L"StateSaleIntention", L"char(1) COLLATE Thai_CI_AS", 175, 1, 0, 0, false, false},
    {L"DateGet", L"datetime", 61, 8, 23, 3, false, false},
    {L"TermValidityUse", L"smallint", 52, 2, 5, 0, false, false},
    {L"UnitColorNumber_1", L"smallint", 52, 2, 5, 0, false, false},
    {L"UnitColorNumber_2", L"smallint", 52, 2, 5, 0, false, false},
    {L"UnitColorNumber_3", L"smallint", 52, 2, 5, 0, false, false},
    {L"UnitColorNumber_4", L"smallint", 52, 2, 5, 0, false, false},
    {L"UnitColorNumber_5", L"smallint", 52, 2, 5, 0, false, false},
    {L"UnitColorNumber_6", L"smallint", 52, 2, 5, 0, false, false},
    {L"UnitDecalNumber_1", L"int", 56, 4, 10, 0, false, false},
    {L"UnitDecalNumber_2", L"int", 56, 4, 10, 0, false, false},
    {L"UnitDecalNumber_3", L"int", 56, 4, 10, 0, false, false},
    {L"UnitDead", L"int", 56, 4, 10, 0, false, false},
    {L"UnitBattle", L"int", 56, 4, 10, 0, false, false},
    {L"UnitWin", L"int", 56, 4, 10, 0, false, false},
    {L"UnitLost", L"int", 56, 4, 10, 0, false, false},
    {L"UnitTie", L"int", 56, 4, 10, 0, false, false},
    {L"BuyType", L"int", 56, 4, 10, 0, false, false},
    {L"CurEnergy", L"real", 59, 4, 24, 0, false, false},
    {L"DateEnd", L"datetime", 61, 8, 23, 3, true, false},
    {L"CategoryNumber", L"int", 56, 4, 10, 0, false, false},
    {L"ShopUnit", L"tinyint", 48, 1, 3, 0, false, false},
    {L"RunOut", L"tinyint", 48, 1, 3, 0, false, false},
    {L"Forbidden", L"tinyint", 48, 1, 3, 0, false, false},
    {L"TimeLimit", L"tinyint", 48, 1, 3, 0, false, false},
    {L"ShopItemId", L"varchar(6) COLLATE Thai_CI_AS", 167, 6, 0, 0, true, false},
    {L"ShopItemtblidx", L"int", 56, 4, 10, 0, true, false},
    {L"SerialNum", L"bigint", 127, 8, 19, 0, true, false},
    {L"CubeMapLevel", L"smallint", 52, 2, 5, 0, false, false},
    {L"SkillPartID", L"int", 56, 4, 10, 0, false, false},
    {L"BattleShipCount", L"smallint", 52, 2, 5, 0, false, false},
    {L"LapUnitExp", L"int", 56, 4, 10, 0, false, false},
    {L"Normal_MaxLv", L"tinyint", 48, 1, 3, 0, false, false},
    {L"LockState", L"tinyint", 48, 1, 3, 0, false, false},
    {L"UnitSkill_1", L"smallint", 52, 2, 5, 0, true, false},
    {L"UnitSkill_2", L"smallint", 52, 2, 5, 0, true, false},
    {L"UnitSkill_3", L"smallint", 52, 2, 5, 0, true, false},
    {L"UnitSkill_4", L"smallint", 52, 2, 5, 0, true, false},
    {L"ResetCnt", L"smallint", 52, 2, 5, 0, true, false},
    {L"UnitClass", L"tinyint", 48, 1, 3, 0, false, false},
    {L"UnitClassMark", L"tinyint", 48, 1, 3, 0, false, false},
    {L"ComposeExp", L"tinyint", 48, 1, 3, 0, false, false},
    {L"ComposeRepair", L"tinyint", 48, 1, 3, 0, false, false},
    {L"ComposeSkillReset", L"tinyint", 48, 1, 3, 0, false, false},
}};

inline std::wstring SqlColumnList(const wchar_t* prefix = nullptr) {
    std::wstring result;
    for (std::size_t index = 0; index < kNativeUnitColumns.size(); ++index) {
        if (index) result += L",";
        if (prefix && *prefix) {
            result += prefix;
            result += L".";
        }
        result += L"[";
        result += kNativeUnitColumns[index].name;
        result += L"]";
    }
    return result;
}

inline std::wstring EscrowColumnDefinitions() {
    std::wstring result;
    for (const auto& column : kNativeUnitColumns) {
        result += L",\n        [";
        result += column.name;
        result += L"] ";
        result += column.sql_type;
        result += column.nullable ? L" NULL" : L" NOT NULL";
    }
    return result;
}

inline std::wstring PayloadVariableDefinitions() {
    std::wstring result;
    for (std::size_t index = 0; index < kNativeUnitColumns.size(); ++index) {
        if (index) result += L",\n        ";
        result += L"[";
        result += kNativeUnitColumns[index].name;
        result += L"] ";
        result += kNativeUnitColumns[index].sql_type;
        result += kNativeUnitColumns[index].nullable ? L" NULL" : L" NOT NULL";
    }
    return result;
}

inline std::wstring RestoreProjection(const wchar_t* prefix,
                                      const wchar_t* accountExpression,
                                      const wchar_t* slotExpression) {
    std::wstring result;
    for (std::size_t index = 0; index < kNativeUnitColumns.size(); ++index) {
        if (index) result += L",";
        const std::wstring_view name = kNativeUnitColumns[index].name;
        if (name == L"AccountNumber") {
            result += accountExpression;
        } else if (name == L"UserUnitSlotNumber") {
            result += slotExpression;
        } else {
            if (prefix && *prefix) {
                result += prefix;
                result += L".";
            }
            result += L"[";
            result += name;
            result += L"]";
        }
    }
    return result;
}

inline std::wstring NativeShapePredicate(std::wstring_view objectId) {
    std::wstring result =
        L"(SELECT COUNT(*) FROM sys.columns WHERE object_id=";
    result += objectId;
    result += L")=50";
    for (const auto& column : kNativeUnitColumns) {
        result += L" AND EXISTS(SELECT 1 FROM sys.columns WHERE object_id=";
        result += objectId;
        result += L" AND name=N'";
        result += column.name;
        result += L"' AND system_type_id=";
        result += std::to_wstring(column.system_type_id);
        result += L" AND max_length=";
        result += std::to_wstring(column.max_length);
        result += L" AND precision=";
        result += std::to_wstring(column.precision);
        result += L" AND scale=";
        result += std::to_wstring(column.scale);
        result += L" AND is_nullable=";
        result += column.nullable ? L"1" : L"0";
        result += L" AND is_identity=";
        result += column.identity ? L"1)" : L"0)";
    }
    return result;
}

inline std::wstring EscrowShapePredicate(std::wstring_view objectId) {
    std::wstring result =
        L"(SELECT COUNT(*) FROM sys.columns WHERE object_id=";
    result += objectId;
    result +=
        L")=51 AND EXISTS(SELECT 1 FROM sys.columns WHERE object_id=";
    result += objectId;
    result +=
        L" AND name=N'GiftId' AND system_type_id=127 AND max_length=8 "
        L"AND is_nullable=0 AND is_identity=0)";
    for (const auto& column : kNativeUnitColumns) {
        result += L" AND EXISTS(SELECT 1 FROM sys.columns WHERE object_id=";
        result += objectId;
        result += L" AND name=N'";
        result += column.name;
        result += L"' AND system_type_id=";
        result += std::to_wstring(column.system_type_id);
        result += L" AND max_length=";
        result += std::to_wstring(column.max_length);
        result += L" AND precision=";
        result += std::to_wstring(column.precision);
        result += L" AND scale=";
        result += std::to_wstring(column.scale);
        result += L" AND is_nullable=";
        result += column.nullable ? L"1" : L"0";
        result += L" AND is_identity=0)";
    }
    return result;
}

} // namespace sdgo::friend_unit_gift
