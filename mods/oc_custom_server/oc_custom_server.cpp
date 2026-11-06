// Over-custom (OC) six-stat enhancement, World server side.
//
// The 2009 OC rules (normal points by level, over points through the OC
// challenge, per-unit growth rows, stat caps) are kept as stored procedures in
// the Mod's own tables (oc_schema.sql). The current custom-parts data in
// tbl_UserUnits is never written. Two runtime paths:
//   1. Lobby requests ride the friend-gift carrier as extension commands and
//      run the ported procedures with the request's own database session.
//   2. In "oc" mode every owned-unit record the World server sends (unit
//      pages, active-unit replies, unit info, user info, end-of-battle user
//      update) carries the allocation's stat deltas in its reserved gap, and
//      the four current custom-skill slots are blanked so only one rule set is
//      active.
//   3. Battle servers keep a per-account copy of the user info (requested when
//      the player enters their channel) and build room pilots from it. After a
//      saved allocation, reset or challenge the native user-info handler is
//      replayed to every peer that holds that account's user info, which is the
//      native refresh path (outside a room it updates the cached user, inside a
//      room it updates the pilot and broadcasts it).
#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "oc_session.h"
#include "sdgo/oc_custom_protocol.h"
#include "sdgo/sdgo_api.h"

namespace {

using namespace sdgo::oc_custom;
using sdgo::oc_custom::server::Int64Or;
using sdgo::oc_custom::server::ResultSets;
using sdgo::oc_custom::server::Session;
using sdgo::oc_custom::server::TextRow;
using sdgo::friend_unit_gift::WireStatus;

constexpr char kWorldHost[] = "WorldServer-548A86E3-0010C000";
constexpr std::uint16_t kSchemaVersion = 1;

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;

struct Settings {
    bool enabled = false;
    bool oc_mode = false;
    bool log_fill = false;
    std::int32_t reward_item = 3240013;
};
Settings g_settings;
std::wstring g_game_db;
std::wstring g_billing_db;
std::wstring g_mod_directory;

// 0 unknown, 1 ready, 2 failed (logged once).
std::atomic<int> g_schema_state{0};
std::mutex g_schema_gate;
std::mutex g_validated_gate;
std::array<void*, 32> g_validated{};
std::size_t g_validated_count = 0;
LONG g_fill_log_budget = 20;

// CRequest handlers run on native worker threads; the send hook needs that
// request's database session.
thread_local void* g_request = nullptr;

using Handler = int(__fastcall*)(void*, void*);
std::array<Handler, 5> g_handler_original{};
sdgo::friend_unit_gift::CurrentRequestFn g_current_request = nullptr;

// Peers (native connection ids) that received an account's user info (0x5C1).
struct UserInfoPeers {
    std::uint32_t account = 0;
    std::array<std::uint32_t, 4> peers{};
    std::uint64_t stamp = 0;
};
std::array<UserInfoPeers, 512> g_user_info_peers{};
SRWLOCK g_user_info_lock = SRWLOCK_INIT;
std::uint64_t g_user_info_clock = 0;
constexpr std::uint16_t kUserInfoReply = 0x05C1;
constexpr std::uint16_t kUserInfoRequest = 0x06B3;
constexpr int kUserInfoReplyBytes = 0x431;
LONG g_refresh_log_budget = 40;
using WorldSend = int(__thiscall*)(void*, std::uint32_t, const std::uint8_t*, int, std::uint32_t);
WorldSend g_send_original = nullptr;

template <class T>
bool ReadAt(const void* base, std::size_t offset, T* value) noexcept {
    __try {
        std::memcpy(value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CopyBytes(void* destination, const void* source, std::size_t size) noexcept {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Log(int level, const char* format, ...) {
    if (!g_api || !g_api->Log) return;
    char buffer[512];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
    va_end(args);
    const DWORD error = GetLastError();
    g_api->Log(g_mod, level, "%s", buffer);
    SetLastError(error);
}

// ---------------------------------------------------------------------------
// Files

bool ReadFileUtf8(const std::wstring& path, std::string* output) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (8 << 20);
    if (ok) {
        output->resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, output->data(), static_cast<DWORD>(output->size()), &read, nullptr) &&
             read == output->size();
    }
    CloseHandle(file);
    if (ok && output->size() >= 3 && static_cast<unsigned char>((*output)[0]) == 0xEF &&
        static_cast<unsigned char>((*output)[1]) == 0xBB &&
        static_cast<unsigned char>((*output)[2]) == 0xBF)
        output->erase(0, 3);
    return ok;
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring output(static_cast<std::size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0)
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            output.data(), length);
    return output;
}

std::vector<std::wstring> SplitBatches(const std::wstring& script) {
    std::vector<std::wstring> batches;
    std::wstring current;
    std::size_t start = 0;
    while (start <= script.size()) {
        const auto end = script.find(L'\n', start);
        std::wstring line = script.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t'))
            line.pop_back();
        std::wstring trimmed = line;
        while (!trimmed.empty() && (trimmed.front() == L' ' || trimmed.front() == L'\t'))
            trimmed.erase(0, 1);
        if (_wcsicmp(trimmed.c_str(), L"GO") == 0) {
            if (current.find_first_not_of(L" \t\r\n") != std::wstring::npos) batches.push_back(current);
            current.clear();
        } else {
            current += line;
            current += L'\n';
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    if (current.find_first_not_of(L" \t\r\n") != std::wstring::npos) batches.push_back(current);
    return batches;
}

struct UnitRevRow {
    std::int64_t unit = 0;
    std::int64_t custom = 0;
    int source = 2;
};

std::vector<UnitRevRow> ParseUnitRev(const std::string& text) {
    std::vector<UnitRevRow> rows;
    std::size_t start = 0;
    bool header_seen = false;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (!header_seen) { header_seen = true; continue; }
        const auto first = line.find('\t');
        const auto second = first == std::string::npos ? std::string::npos : line.find('\t', first + 1);
        if (first == std::string::npos || second == std::string::npos) continue;
        UnitRevRow row;
        char* end_ptr = nullptr;
        row.unit = std::strtoll(line.substr(0, first).c_str(), &end_ptr, 10);
        row.custom = std::strtoll(line.substr(first + 1, second - first - 1).c_str(), &end_ptr, 10);
        row.source = line.compare(second + 1, std::string::npos, "original") == 0 ? 1 : 2;
        if (row.unit > 0 && row.custom > 0) rows.push_back(row);
    }
    return rows;
}

// ---------------------------------------------------------------------------
// Database

bool ValidatedConnection(void* connection) {
    std::lock_guard<std::mutex> lock(g_validated_gate);
    for (std::size_t index = 0; index < g_validated_count; ++index)
        if (g_validated[index] == connection) return true;
    return false;
}

bool ValidateConnection(void* connection, Session& session) {
    if (ValidatedConnection(connection)) return true;
    ResultSets sets;
    if (!session.Query(L"SELECT DB_NAME()", &sets) || sets.size() != 1 ||
        sets[0].size() != 1 || sets[0][0].size() != 1 ||
        _wcsicmp(sets[0][0][0].c_str(), g_game_db.c_str()) != 0)
        return false;
    std::lock_guard<std::mutex> lock(g_validated_gate);
    if (g_validated_count < g_validated.size()) g_validated[g_validated_count++] = connection;
    return true;
}

bool SchemaPresent(Session& session) {
    ResultSets sets;
    if (!session.Query(
            L"SET NOCOUNT ON; SELECT [Value] FROM dbo.sdgo_mod_oc_Meta WHERE [Key] = N'SchemaVersion'; "
            L"SELECT COUNT(*) FROM dbo.sdgo_mod_oc_UnitRev; "
            L"SELECT COUNT(*) FROM sys.objects WHERE type = 'P' AND name LIKE N'sdgo[_]mod[_]oc[_]%'",
            &sets) ||
        sets.size() != 3 || sets[0].size() != 1 || sets[1].size() != 1 || sets[2].size() != 1)
        return false;
    return sets[0][0].size() == 1 && sets[0][0][0] == std::to_wstring(kSchemaVersion) &&
           Int64Or(sets[1][0], 0) > 0 && Int64Or(sets[2][0], 0) >= 8;
}

bool InstallSchema(Session& session) {
    std::string script;
    if (!ReadFileUtf8(g_mod_directory + L"\\oc_schema.sql", &script)) {
        Log(SDGO_LOG_ERROR, "oc_schema.sql is missing from the mod directory");
        return false;
    }
    const auto batches = SplitBatches(Widen(script));
    for (std::size_t index = 0; index < batches.size(); ++index) {
        if (!session.Execute(batches[index])) {
            Log(SDGO_LOG_ERROR, "oc schema batch %u of %u failed", static_cast<unsigned>(index + 1),
                static_cast<unsigned>(batches.size()));
            return false;
        }
    }
    std::string mapping;
    if (!ReadFileUtf8(g_mod_directory + L"\\oc_unit_rev.tsv", &mapping)) {
        Log(SDGO_LOG_ERROR, "oc_unit_rev.tsv is missing from the mod directory");
        return false;
    }
    const auto rows = ParseUnitRev(mapping);
    if (rows.empty()) {
        Log(SDGO_LOG_ERROR, "oc_unit_rev.tsv holds no unit rows");
        return false;
    }
    for (std::size_t start = 0; start < rows.size(); start += 200) {
        std::wstring sql =
            L"SET NOCOUNT ON; INSERT INTO dbo.sdgo_mod_oc_UnitRev (UnitNumber, CustomNumber, Source) "
            L"SELECT v.UnitNumber, v.CustomNumber, v.Source FROM (VALUES ";
        const auto end = (std::min)(rows.size(), start + 200);
        for (std::size_t index = start; index < end; ++index) {
            if (index != start) sql += L",";
            sql += L"(" + std::to_wstring(rows[index].unit) + L"," + std::to_wstring(rows[index].custom) +
                   L"," + std::to_wstring(rows[index].source) + L")";
        }
        sql += L") AS v(UnitNumber, CustomNumber, Source) WHERE NOT EXISTS "
               L"(SELECT 1 FROM dbo.sdgo_mod_oc_UnitRev r WHERE r.UnitNumber = v.UnitNumber)";
        if (!session.Execute(sql)) {
            Log(SDGO_LOG_ERROR, "oc unit mapping seed failed at row %u", static_cast<unsigned>(start));
            return false;
        }
    }
    const std::wstring version = std::to_wstring(kSchemaVersion);
    if (!session.Execute(
            L"SET NOCOUNT ON; MERGE dbo.sdgo_mod_oc_Meta AS t USING (SELECT N'SchemaVersion' AS [Key], N'" +
            version + L"' AS [Value]) AS s ON t.[Key] = s.[Key] "
            L"WHEN MATCHED THEN UPDATE SET [Value] = s.[Value], UpdatedAt = GETDATE() "
            L"WHEN NOT MATCHED THEN INSERT ([Key], [Value]) VALUES (s.[Key], s.[Value]);"))
        return false;
    Log(SDGO_LOG_INFO, "oc schema version %u installed in %ls with %u unit growth rows",
        static_cast<unsigned>(kSchemaVersion), g_game_db.c_str(), static_cast<unsigned>(rows.size()));
    return true;
}

// Opens the request's game session, validates its database and makes sure the
// owned schema exists. Returns false when OC must stay inactive.
bool OpenGameSession(void* connection, Session& session) {
    if (!g_settings.enabled || !connection || !session.Open(connection)) return false;
    if (!ValidateConnection(connection, session)) {
        Log(SDGO_LOG_ERROR, "native game session is not bound to %ls; oc request ignored", g_game_db.c_str());
        return false;
    }
    if (g_schema_state.load(std::memory_order_acquire) == 1) return true;
    if (g_schema_state.load(std::memory_order_acquire) == 2) return false;
    std::lock_guard<std::mutex> lock(g_schema_gate);
    const int state = g_schema_state.load(std::memory_order_relaxed);
    if (state == 1) return true;
    if (state == 2) return false;
    Session installer(120);
    if (!installer.Open(connection)) return false;
    const bool ready = SchemaPresent(installer) || (InstallSchema(installer) && SchemaPresent(installer));
    g_schema_state.store(ready ? 1 : 2, std::memory_order_release);
    if (!ready) Log(SDGO_LOG_ERROR, "oc schema unavailable; over-custom stays inactive until restart");
    return ready;
}

bool SchemaReady() noexcept {
    return g_schema_state.load(std::memory_order_acquire) == 1;
}

// ---------------------------------------------------------------------------
// Outgoing owned-unit records

struct CarrierRow {
    std::uint64_t uid = 0;
    Carrier carrier{};
};

bool LoadCarriers(Session& session, const std::vector<std::uint64_t>& uids, std::vector<CarrierRow>* output) {
    output->clear();
    if (uids.empty()) return true;
    std::wstring list;
    for (std::size_t index = 0; index < uids.size(); ++index) {
        if (index) list += L",";
        list += std::to_wstring(uids[index]);
    }
    ResultSets sets;
    if (!session.Query(
            L"SET NOCOUNT ON; SELECT P.UserUnitNumber, P.OcLevel, P.Normal_CustomPnt, P.Over_CustomPnt, P.Revision, "
            L"P.Normal_Speed * TCR.Normal_Speed_Stat + P.Over_Speed * TCR.Over_Speed_Stat, "
            L"P.Normal_HP * TCR.Normal_HP_Stat + P.Over_HP * TCR.Over_HP_Stat, "
            L"P.Normal_Attack * TCR.Normal_Attack_Stat + P.Over_Attack * TCR.Over_Attack_Stat, "
            L"P.Normal_Defense * TCR.Normal_Defense_Stat + P.Over_Defense * TCR.Over_Defense_Stat, "
            L"P.Normal_Agility * TCR.Normal_Agility_Stat + P.Over_Agility * TCR.Over_Agility_Stat, "
            L"P.Normal_Special * TCR.Normal_Special_Stat + P.Over_Special * TCR.Over_Special_Stat "
            L"FROM dbo.sdgo_mod_oc_UserUnitCustom P "
            L"INNER JOIN dbo.tbl_UserUnits UU ON UU.UserUnitNumber = P.UserUnitNumber AND UU.UnitNumber = P.UnitNumber AND UU.AccountNumber = P.AccountNumber "
            L"INNER JOIN dbo.sdgo_mod_oc_UnitRev R ON R.UnitNumber = P.UnitNumber "
            L"INNER JOIN dbo.tbl_CustomRev TCR ON TCR.CustomNumber = R.CustomNumber "
            L"WHERE P.State = 0 AND P.UserUnitNumber IN (" + list + L")",
            &sets) ||
        sets.size() != 1)
        return false;
    for (const auto& row : sets[0]) {
        if (row.size() != 11) return false;
        CarrierRow entry;
        entry.uid = static_cast<std::uint64_t>(Int64Or(row, 0));
        entry.carrier.flags = carrier_oc_mode | carrier_profile;
        entry.carrier.oc_level = static_cast<std::uint8_t>(Int64Or(row, 1));
        entry.carrier.normal_points = static_cast<std::uint8_t>(Int64Or(row, 2));
        entry.carrier.over_points = static_cast<std::uint8_t>(Int64Or(row, 3));
        entry.carrier.revision = static_cast<std::uint32_t>(Int64Or(row, 4));
        for (std::size_t stat = 0; stat < stat_count; ++stat) {
            auto value = Int64Or(row, 5 + stat);
            if (value > 32767) value = 32767;
            if (value < -32768) value = -32768;
            entry.carrier.delta[stat] = static_cast<std::int16_t>(value);
        }
        output->push_back(entry);
    }
    return true;
}

struct PacketLayout {
    std::uint16_t opcode;
    int fixed_size;      // -1: 7 + count * 0x99 with the count byte at +6
    std::size_t first;   // first record offset
    std::size_t maximum; // maximum record count
};
constexpr PacketLayout kLayouts[] = {
    {0x08F2, -1, 7, 6},       // CRequest::ReqUnitSlotPage
    {0x09B4, 0xA1, 8, 1},     // CRequest::ReqActiveWaitUnit
    {0x0699, 0xA7, 0x0E, 1},  // CRequest::ReqGetUserUnitInfo
    {0x05C1, 0x431, 0x51, 1}, // CRequest::ReqUserInfo (user info)
    {0x04A9, 0x134, 0x2B, 1}, // end-of-battle user update to the Battle server (request 0x68D)
};

void RememberUserInfoPeer(const std::uint8_t* packet, int size, std::uint32_t destination) {
    std::uint16_t opcode = 0;
    std::uint32_t account = 0;
    if (size != kUserInfoReplyBytes || !ReadAt(packet, 0, &opcode) || opcode != kUserInfoReply ||
        !ReadAt(packet, 2, &account) || !account)
        return;
    AcquireSRWLockExclusive(&g_user_info_lock);
    UserInfoPeers* slot = nullptr;
    UserInfoPeers* oldest = &g_user_info_peers[0];
    for (auto& entry : g_user_info_peers) {
        if (entry.account == account) { slot = &entry; break; }
        if (entry.stamp < oldest->stamp) oldest = &entry;
    }
    if (!slot) {
        slot = oldest;
        *slot = UserInfoPeers{};
        slot->account = account;
    }
    slot->stamp = ++g_user_info_clock;
    bool known = false;
    for (auto peer : slot->peers) known = known || peer == destination;
    if (!known) {
        // Newest first; the oldest peer falls off.
        for (std::size_t index = slot->peers.size() - 1; index > 0; --index) slot->peers[index] = slot->peers[index - 1];
        slot->peers[0] = destination;
    }
    ReleaseSRWLockExclusive(&g_user_info_lock);
}

void PatchPacket(std::uint8_t* packet, int size, std::uint32_t destination) {
    if (!packet || size < 7 || !g_request) return;
    std::uint16_t opcode = 0;
    if (!ReadAt(packet, 0, &opcode)) return;
    const PacketLayout* layout = nullptr;
    for (const auto& candidate : kLayouts)
        if (candidate.opcode == opcode) layout = &candidate;
    if (!layout) return;
    std::size_t count = layout->maximum;
    if (layout->fixed_size < 0) {
        std::uint8_t stored = 0;
        if (!ReadAt(packet, 6, &stored) || stored > layout->maximum ||
            size != static_cast<int>(7 + stored * kUnitRecordBytes))
            return;
        count = stored;
    } else if (size != layout->fixed_size) {
        return;
    }
    if (!count) return;
    void* connection = nullptr;
    if (!ReadAt(g_request, 0x14, &connection) || !connection) return;
    Session session(5);
    if (!OpenGameSession(connection, session)) return;

    std::vector<std::uint64_t> uids;
    for (std::size_t index = 0; index < count; ++index) {
        std::uint64_t uid = 0;
        if (ReadAt(packet, layout->first + index * kUnitRecordBytes + 8, &uid) && uid) uids.push_back(uid);
    }
    std::vector<CarrierRow> rows;
    if (!LoadCarriers(session, uids, &rows)) {
        Log(SDGO_LOG_WARN, "oc carrier query failed for opcode 0x%X; records sent unchanged", opcode);
        return;
    }
    unsigned filled = 0;
    std::uint64_t first_uid = 0;
    Carrier first{};
    for (std::size_t index = 0; index < count; ++index) {
        auto* record = packet + layout->first + index * kUnitRecordBytes;
        std::uint64_t uid = 0;
        if (!ReadAt(record, 8, &uid) || !uid) continue;
        Carrier carrier;
        carrier.flags = carrier_oc_mode;
        for (const auto& row : rows)
            if (row.uid == uid) carrier = row.carrier;
        std::uint8_t copy[kUnitRecordBytes];
        if (!CopyBytes(copy, record, sizeof(copy))) continue;
        WriteCarrier(copy, carrier);
        ClearCustomSkills(copy);
        if (CopyBytes(record, copy, sizeof(copy))) {
            if (!filled) {
                first_uid = uid;
                first = carrier;
            }
            ++filled;
        }
    }
    if (g_settings.log_fill && filled && InterlockedDecrement(&g_fill_log_budget) >= 0)
        Log(SDGO_LOG_INFO,
            "oc carrier filled %u record(s) in opcode 0x%X (%u with a profile) to 0x%X; first uid=%llu revision=%u hp=%d",
            filled, opcode, static_cast<unsigned>(rows.size()), destination,
            static_cast<unsigned long long>(first_uid), first.revision, first.delta[stat_hp]);
}

int __fastcall SendHook(void* network, void*, std::uint32_t destination, std::uint8_t* packet, int size,
                        std::uint32_t flags) {
    if (g_request && g_settings.enabled && g_settings.oc_mode) {
        const DWORD error = GetLastError();
        PatchPacket(packet, size, destination);
        __try {
            RememberUserInfoPeer(packet, size, destination);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        SetLastError(error);
    }
    return g_send_original(network, destination, packet, size, flags);
}

template <int Index>
int __fastcall HandlerHook(void* self, void* edx) {
    void* previous = g_request;
    g_request = self;
    const int result = g_handler_original[Index](self, edx);
    g_request = previous;
    return result;
}

// ---------------------------------------------------------------------------
// Lobby commands

bool ParseProfile(const TextRow& row, Profile* profile) {
    if (row.size() != 52) return false;
    Profile decoded;
    decoded.unit_uid = static_cast<std::uint64_t>(Int64Or(row, 0));
    decoded.unit_number = static_cast<std::uint32_t>(Int64Or(row, 2));
    decoded.oc_level = static_cast<std::uint8_t>(Int64Or(row, 3));
    decoded.normal_max_lv = static_cast<std::uint8_t>(Int64Or(row, 4));
    decoded.revision = static_cast<std::uint32_t>(Int64Or(row, 5));
    decoded.state = static_cast<std::uint8_t>(Int64Or(row, 6));
    decoded.extra_bonus = static_cast<std::uint8_t>(Int64Or(row, 7));
    decoded.normal_points = static_cast<std::uint8_t>(Int64Or(row, 8));
    for (std::size_t stat = 0; stat < stat_count; ++stat) {
        decoded.normal[stat] = static_cast<std::uint8_t>(Int64Or(row, 9 + stat));
        decoded.over[stat] = static_cast<std::uint8_t>(Int64Or(row, 16 + stat));
        decoded.base[stat] = static_cast<std::int32_t>(Int64Or(row, 27 + stat));
        decoded.normal_stat[stat] = static_cast<std::int32_t>(Int64Or(row, 33 + stat));
        decoded.over_stat[stat] = static_cast<std::int32_t>(Int64Or(row, 39 + stat));
    }
    decoded.over_points = static_cast<std::uint8_t>(Int64Or(row, 15));
    decoded.native_level = static_cast<std::uint8_t>(Int64Or(row, 22));
    decoded.lap_exp = static_cast<std::uint32_t>(Int64Or(row, 23));
    decoded.battles = static_cast<std::uint32_t>(Int64Or(row, 24));
    decoded.grade = static_cast<std::uint8_t>(Int64Or(row, 25));
    decoded.custom_number = static_cast<std::uint32_t>(Int64Or(row, 26));
    decoded.normal_budget = static_cast<std::uint8_t>(Int64Or(row, 45));
    decoded.over_budget = static_cast<std::uint8_t>(Int64Or(row, 46));
    decoded.next_level = static_cast<std::uint8_t>(Int64Or(row, 47));
    decoded.next_probability = static_cast<std::int32_t>(Int64Or(row, 48));
    decoded.next_give_point = static_cast<std::uint8_t>(Int64Or(row, 49));
    decoded.next_battle_cnt = static_cast<std::int32_t>(Int64Or(row, 50));
    decoded.level_max_exp = static_cast<std::int32_t>(Int64Or(row, 51));
    *profile = decoded;
    return true;
}

// Runs sdgo_mod_oc_GetProfile. Returns false on a database failure; ret_val
// carries the procedure's result.
bool QueryProfile(Session& session, std::uint32_t account, std::uint64_t uid, std::int32_t* ret_val,
                  Profile* profile) {
    ResultSets sets;
    if (!session.Query(L"SET NOCOUNT ON; DECLARE @r INT; EXEC dbo.sdgo_mod_oc_GetProfile " +
                           std::to_wstring(account) + L", " + std::to_wstring(uid) + L", @r OUTPUT; SELECT @r;",
                       &sets) ||
        sets.empty() || sets.back().size() != 1)
        return false;
    *ret_val = static_cast<std::int32_t>(Int64Or(sets.back()[0], 0, 2));
    *profile = {};
    if (*ret_val == 0) {
        if (sets.size() != 2 || sets[0].size() != 1 || !ParseProfile(sets[0][0], profile)) return false;
    }
    return true;
}

std::wstring Num(std::int64_t value) { return std::to_wstring(value); }

std::int32_t HandleGetProfile(Session& session, std::uint32_t account, const std::uint8_t* payload,
                              std::uint32_t bytes, std::uint8_t* reply, std::uint32_t* produced) {
    UnitRequest request;
    if (!ReadUnitRequest(payload, bytes, &request)) return static_cast<std::int32_t>(WireStatus::invalid_argument);
    std::int32_t ret_val = 0;
    Profile profile;
    if (!QueryProfile(session, account, request.unit_uid, &ret_val, &profile))
        return static_cast<std::int32_t>(WireStatus::database_unavailable);
    WriteProfileReply(reply, ret_val, profile);
    *produced = static_cast<std::uint32_t>(kProfileReplyBytes);
    return static_cast<std::int32_t>(WireStatus::success);
}

std::int32_t HandleSet(Session& session, std::uint32_t account, bool over, const std::uint8_t* payload,
                       std::uint32_t bytes, std::uint8_t* reply, std::uint32_t* produced) {
    SetRequest request;
    if (!ReadSetRequest(payload, bytes, &request)) return static_cast<std::int32_t>(WireStatus::invalid_argument);
    std::wstring sql = L"SET NOCOUNT ON; DECLARE @r INT, @p TINYINT; EXEC dbo.";
    sql += over ? L"sdgo_mod_oc_SetOver " : L"sdgo_mod_oc_SetNormal ";
    sql += Num(account) + L", " + Num(static_cast<std::int64_t>(request.unit_uid));
    for (std::size_t stat = 0; stat < stat_count; ++stat) sql += L", " + Num(request.values[stat]);
    sql += L", " + Num(request.revision) + L", @p OUTPUT, @r OUTPUT; SELECT @r, ISNULL(@p, 0);";
    ResultSets sets;
    if (!session.Query(sql, &sets) || sets.size() != 1 || sets[0].size() != 1 || sets[0][0].size() != 2)
        return static_cast<std::int32_t>(WireStatus::database_unavailable);
    SetReply result;
    result.ret_val = static_cast<std::int32_t>(Int64Or(sets[0][0], 0, 1));
    result.remaining = static_cast<std::uint8_t>(Int64Or(sets[0][0], 1));
    std::int32_t profile_ret = 0;
    if (!QueryProfile(session, account, request.unit_uid, &profile_ret, &result.profile))
        return static_cast<std::int32_t>(WireStatus::database_unavailable);
    WriteSetReply(reply, result);
    *produced = static_cast<std::uint32_t>(kSetReplyBytes);
    return static_cast<std::int32_t>(WireStatus::success);
}

std::int32_t HandleUpgrade(Session& session, std::uint32_t account, const std::uint8_t* payload,
                           std::uint32_t bytes, std::uint8_t* reply, std::uint32_t* produced) {
    UpgradeRequest request;
    if (!ReadUpgradeRequest(payload, bytes, &request))
        return static_cast<std::int32_t>(WireStatus::invalid_argument);
    std::wstring sql =
        L"SET NOCOUNT ON; SET XACT_ABORT ON; DECLARE @r INT, @un INT, @lv TINYINT, @op TINYINT, "
        L"@c1 SMALLINT, @c2 SMALLINT, @c3 SMALLINT, @ok TINYINT, @prev TINYINT, @now TINYINT, @next TINYINT; "
        L"BEGIN TRAN; EXEC dbo.sdgo_mod_oc_Upgrade " +
        Num(account) + L", " + Num(static_cast<std::int64_t>(request.unit_uid)) + L", " + Num(request.item[0]) +
        L", " + Num(request.item[1]) + L", " + Num(request.item[2]) + L", " + Num(request.count[0]) + L", " +
        Num(request.count[1]) + L", " + Num(request.count[2]) + L", " + Num(g_settings.reward_item) +
        L", @un OUTPUT, @lv OUTPUT, @op OUTPUT, @c1 OUTPUT, @c2 OUTPUT, @c3 OUTPUT, @ok OUTPUT, @prev OUTPUT, "
        L"@now OUTPUT, @next OUTPUT, @r OUTPUT; IF @r = 0 COMMIT TRAN ELSE ROLLBACK TRAN; "
        L"SELECT @r, ISNULL(@ok,0), ISNULL(@prev,0), ISNULL(@now,0), ISNULL(@next,0), ISNULL(@lv,0), "
        L"ISNULL(@op,0), ISNULL(@c1,0), ISNULL(@c2,0), ISNULL(@c3,0);";
    ResultSets sets;
    if (!session.Query(sql, &sets) || sets.size() != 1 || sets[0].size() != 1 || sets[0][0].size() != 10)
        return static_cast<std::int32_t>(WireStatus::database_unavailable);
    const auto& row = sets[0][0];
    UpgradeReply result;
    result.ret_val = static_cast<std::int32_t>(Int64Or(row, 0, 8));
    result.success = static_cast<std::uint8_t>(Int64Or(row, 1));
    result.lv_prevention = static_cast<std::uint8_t>(Int64Or(row, 2));
    result.now_lv = static_cast<std::uint8_t>(Int64Or(row, 3));
    result.next_lv = static_cast<std::uint8_t>(Int64Or(row, 4));
    result.oc_level = static_cast<std::uint8_t>(Int64Or(row, 5));
    result.over_points = static_cast<std::uint8_t>(Int64Or(row, 6));
    for (std::size_t index = 0; index < 3; ++index)
        result.item_left[index] = static_cast<std::int16_t>(Int64Or(row, 7 + index));
    std::int32_t profile_ret = 0;
    if (!QueryProfile(session, account, request.unit_uid, &profile_ret, &result.profile))
        return static_cast<std::int32_t>(WireStatus::database_unavailable);
    WriteUpgradeReply(reply, result);
    *produced = static_cast<std::uint32_t>(kUpgradeReplyBytes);
    return static_cast<std::int32_t>(WireStatus::success);
}

std::int32_t HandleResetCost(Session& session, std::uint32_t account, const std::uint8_t* payload,
                             std::uint32_t bytes, std::uint8_t* reply, std::uint32_t* produced) {
    WorkRequest request;
    if (!ReadWorkRequest(payload, bytes, &request)) return static_cast<std::int32_t>(WireStatus::invalid_argument);
    ResultSets sets;
    if (!session.Query(L"SET NOCOUNT ON; DECLARE @r INT, @rank TINYINT, @gp INT; EXEC dbo.sdgo_mod_oc_GetResetGP " +
                           Num(account) + L", " + Num(static_cast<std::int64_t>(request.unit_uid)) + L", " +
                           Num(request.work) + L", @rank OUTPUT, @gp OUTPUT, @r OUTPUT; "
                           L"SELECT @r, ISNULL(@rank,0), ISNULL(@gp,0);",
                       &sets) ||
        sets.size() != 1 || sets[0].size() != 1 || sets[0][0].size() != 3)
        return static_cast<std::int32_t>(WireStatus::database_unavailable);
    ResetCostReply result;
    result.ret_val = static_cast<std::int32_t>(Int64Or(sets[0][0], 0, 1));
    result.unit_rank = static_cast<std::uint8_t>(Int64Or(sets[0][0], 1));
    result.reset_gp = static_cast<std::int32_t>(Int64Or(sets[0][0], 2));
    WriteResetCostReply(reply, result);
    *produced = static_cast<std::uint32_t>(kResetCostReplyBytes);
    return static_cast<std::int32_t>(WireStatus::success);
}

std::int32_t HandleReset(Session& session, std::uint32_t account, const std::uint8_t* payload,
                         std::uint32_t bytes, std::uint8_t* reply, std::uint32_t* produced) {
    WorkRequest request;
    if (!ReadWorkRequest(payload, bytes, &request)) return static_cast<std::int32_t>(WireStatus::invalid_argument);
    const std::wstring billing = L"[" + g_billing_db + L"]";
    const std::wstring sql =
        L"SET NOCOUNT ON; SET XACT_ABORT ON; DECLARE @r INT, @rank TINYINT, @gp INT, @np TINYINT, @op TINYINT, "
        L"@charged INT; SET @charged = 0; EXEC dbo.sdgo_mod_oc_GetResetGP " +
        Num(account) + L", " + Num(static_cast<std::int64_t>(request.unit_uid)) + L", " + Num(request.work) +
        L", @rank OUTPUT, @gp OUTPUT, @r OUTPUT; IF @r = 0 BEGIN BEGIN TRAN; IF ISNULL(@gp, 0) > 0 BEGIN "
        L"UPDATE " + billing + L".dbo.tbls_AccountPocket SET f_GP = f_GP - @gp WHERE f_idx = " + Num(account) +
        L" AND f_GP >= @gp; IF @@ROWCOUNT <> 1 BEGIN ROLLBACK TRAN; SET @r = " + Num(reset_insufficient_gp) +
        L"; END ELSE SET @charged = @gp; END "
        L"IF @r = 0 BEGIN EXEC dbo.sdgo_mod_oc_Reset " + Num(account) + L", " +
        Num(static_cast<std::int64_t>(request.unit_uid)) + L", " + Num(request.work) +
        L", @np OUTPUT, @op OUTPUT, @r OUTPUT; IF @r = 0 COMMIT TRAN ELSE BEGIN ROLLBACK TRAN; SET @charged = 0; END END END "
        L"SELECT @r, ISNULL(@np,0), ISNULL(@op,0), @charged;";
    ResultSets sets;
    if (!session.Query(sql, &sets) || sets.size() != 1 || sets[0].size() != 1 || sets[0][0].size() != 4)
        return static_cast<std::int32_t>(WireStatus::database_unavailable);
    ResetReply result;
    result.ret_val = static_cast<std::int32_t>(Int64Or(sets[0][0], 0, 4));
    result.normal_points = static_cast<std::uint8_t>(Int64Or(sets[0][0], 1));
    result.over_points = static_cast<std::uint8_t>(Int64Or(sets[0][0], 2));
    result.charged_gp = static_cast<std::int32_t>(Int64Or(sets[0][0], 3));
    std::int32_t profile_ret = 0;
    if (!QueryProfile(session, account, request.unit_uid, &profile_ret, &result.profile))
        return static_cast<std::int32_t>(WireStatus::database_unavailable);
    WriteResetReply(reply, result);
    *produced = static_cast<std::uint32_t>(kResetReplyBytes);
    return static_cast<std::int32_t>(WireStatus::success);
}

std::int32_t HandleMode(std::uint8_t* reply, std::uint32_t* produced) {
    ModeReply mode;
    mode.enabled = g_settings.enabled ? 1 : 0;
    mode.oc_mode = g_settings.enabled && g_settings.oc_mode ? 1 : 0;
    mode.schema_version = kSchemaVersion;
    mode.flags = SchemaReady() ? 1u : 0u;
    WriteModeReply(reply, 0, mode);
    *produced = static_cast<std::uint32_t>(kModeReplyBytes);
    return static_cast<std::int32_t>(WireStatus::success);
}

// Replays the native user-info handler (CRequest::ReqUserInfo) for the
// account to every peer that already holds its user info, except the agent
// that carried this request. The copied request keeps the native database
// sessions (+0x14 game, +0x20 and +0x24 by the user-info builder); only the
// destination ([0]) and the packet buffer ([2]) change.
unsigned RefreshUserInfo(std::uint32_t account) {
    if (!g_current_request) return 0;
    void* request = g_current_request();
    if (!request) return 0;
    std::uint32_t origin = 0;
    if (!ReadAt(request, 0, &origin)) return 0;
    std::array<std::uint32_t, 4> peers{};
    AcquireSRWLockShared(&g_user_info_lock);
    for (const auto& entry : g_user_info_peers)
        if (entry.account == account) { peers = entry.peers; break; }
    ReleaseSRWLockShared(&g_user_info_lock);
    unsigned sent = 0;
    for (auto peer : peers) {
        if (!peer || peer == origin) continue;
        alignas(8) std::uint8_t fake[0x80] = {};
        alignas(8) std::uint8_t buffer[0x440] = {};
        bool copied = false;
        __try {
            std::memcpy(fake, request, sizeof(fake));
            copied = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (!copied) break;
        std::memcpy(fake, &peer, sizeof(peer));
        void* buffer_pointer = buffer;
        std::memcpy(fake + 8, &buffer_pointer, sizeof(buffer_pointer));
        std::memcpy(buffer, &kUserInfoRequest, sizeof(kUserInfoRequest));
        std::memcpy(buffer + 2, &account, sizeof(account));
        __try {
            // Through the hook, so this send is filled like any other.
            if (HandlerHook<3>(fake, nullptr) == 0) ++sent;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (g_settings.log_fill && InterlockedDecrement(&g_refresh_log_budget) >= 0)
        Log(SDGO_LOG_INFO, "oc user info refreshed for account %u on %u peer(s)", account, sent);
    return sent;
}

std::int32_t __cdecl ExtensionHandler(void*, std::uint32_t actor, std::uint16_t command, const std::uint8_t* payload,
                                      std::uint32_t payload_bytes, void*, void* game_database,
                                      std::uint8_t* reply, std::uint32_t reply_capacity, std::uint32_t* produced) {
    if (!reply || !produced || reply_capacity < kUpgradeReplyBytes || !actor)
        return static_cast<std::int32_t>(WireStatus::transport_invalid);
    *produced = 0;
    const auto wrapped = static_cast<Command>(command);
    if (wrapped == Command::mode) return HandleMode(reply, produced);
    if (!g_settings.enabled || !g_settings.oc_mode) return static_cast<std::int32_t>(WireStatus::disabled);
    std::int32_t status = static_cast<std::int32_t>(WireStatus::invalid_argument);
    {
        Session session(20);
        if (!OpenGameSession(game_database, session))
            return static_cast<std::int32_t>(WireStatus::database_unavailable);
        switch (wrapped) {
        case Command::get_profile:
            return HandleGetProfile(session, actor, payload, payload_bytes, reply, produced);
        case Command::set_normal:
            status = HandleSet(session, actor, false, payload, payload_bytes, reply, produced);
            break;
        case Command::set_over:
            status = HandleSet(session, actor, true, payload, payload_bytes, reply, produced);
            break;
        case Command::upgrade:
            status = HandleUpgrade(session, actor, payload, payload_bytes, reply, produced);
            break;
        case Command::reset_cost:
            return HandleResetCost(session, actor, payload, payload_bytes, reply, produced);
        case Command::reset:
            status = HandleReset(session, actor, payload, payload_bytes, reply, produced);
            break;
        default:
            return static_cast<std::int32_t>(WireStatus::invalid_argument);
        }
    }
    // A changed profile reaches the Battle servers through the native user info.
    std::int32_t ret_val = -1;
    if (status == static_cast<std::int32_t>(WireStatus::success) && *produced >= sizeof(ret_val))
        std::memcpy(&ret_val, reply, sizeof(ret_val));
    if (ret_val == 0) RefreshUserInfo(actor);
    return status;
}

// ---------------------------------------------------------------------------
// Load

bool ReadDatabaseNames() {
    wchar_t executable[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!length || length >= std::size(executable)) return false;
    std::wstring ini(executable, executable + length);
    const auto slash = ini.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    ini.resize(slash + 1);
    ini += L"WorldServer.ini";
    wchar_t game[128] = {};
    wchar_t billing[128] = {};
    const DWORD game_length = GetPrivateProfileStringW(L"DB Game", L"DBName", L"", game,
                                                       static_cast<DWORD>(std::size(game)), ini.c_str());
    const DWORD billing_length = GetPrivateProfileStringW(L"DB StoreBilling", L"DBName", L"", billing,
                                                          static_cast<DWORD>(std::size(billing)), ini.c_str());
    if (!game_length || game_length >= std::size(game) - 1 || !billing_length ||
        billing_length >= std::size(billing) - 1)
        return false;
    for (const auto* name : {game, billing})
        for (const wchar_t* c = name; *c; ++c)
            if (!(iswalnum(*c) || *c == L'_')) return false;
    g_game_db = game;
    g_billing_db = billing;
    return true;
}

bool RegisterExtension() {
    HMODULE gift = GetModuleHandleW(L"friend_unit_gift_server.dll");
    if (!gift) return false;
    const auto register_fn = reinterpret_cast<sdgo::friend_unit_gift::RegisterExtensionFn>(
        GetProcAddress(gift, SDGO_FRIEND_UNIT_GIFT_REGISTER_EXTENSION_EXPORT));
    g_current_request = reinterpret_cast<sdgo::friend_unit_gift::CurrentRequestFn>(
        GetProcAddress(gift, SDGO_FRIEND_UNIT_GIFT_CURRENT_REQUEST_EXPORT));
    if (!g_current_request && g_api && g_api->Log)
        g_api->Log(g_mod, SDGO_LOG_WARN,
                   "friend_unit_gift_server has no current-request export; Battle servers refresh OC values on the "
                   "next channel entry");
    return register_fn && register_fn(kFirstCommand, kLastCommand, &ExtensionHandler, nullptr) != 0;
}

} // namespace

SDGO_MOD_ENTRY {
    if (!api || !mod || api->version != SDGO_API_VERSION || !api->host_key || !api->Symbol || !api->HookCode ||
        !api->Log || !api->ModDirectory)
        return 0;
    g_api = api;
    g_mod = mod;
    if (std::strcmp(api->host_key, kWorldHost) != 0) return 0;
    g_settings.enabled = api->SettingInt(mod, "Enabled", 0) != 0;
    char mode[32] = {};
    api->SettingString(mod, "Mode", "current", mode, sizeof(mode));
    g_settings.oc_mode = _stricmp(mode, "oc") == 0;
    g_settings.log_fill = api->SettingInt(mod, "LogFill", 1) != 0;
    g_settings.reward_item = api->SettingInt(mod, "RewardItem", 3240013);
    const wchar_t* directory = api->ModDirectory(mod);
    if (!directory) return 0;
    g_mod_directory = directory;
    if (!g_settings.enabled) {
        api->Log(mod, SDGO_LOG_INFO, "over-custom disabled; records and lobby requests stay native");
        return 0;
    }
    if (!ReadDatabaseNames()) {
        api->Log(mod, SDGO_LOG_ERROR,
                 "WorldServer.ini [DB Game]/[DB StoreBilling] DBName are required; over-custom inactive");
        return 0;
    }
    const char* handlers[] = {
        "oc_custom.world_req_unit_slot_page",
        "oc_custom.world_req_active_wait_unit",
        "oc_custom.world_req_get_user_unit_info",
        "oc_custom.world_req_user_info",
        "oc_custom.world_req_battle_end_user",
    };
    void* detours[] = {
        reinterpret_cast<void*>(&HandlerHook<0>), reinterpret_cast<void*>(&HandlerHook<1>),
        reinterpret_cast<void*>(&HandlerHook<2>), reinterpret_cast<void*>(&HandlerHook<3>),
        reinterpret_cast<void*>(&HandlerHook<4>),
    };
    for (std::size_t index = 0; index < std::size(handlers); ++index) {
        if (!api->HookCode(mod, handlers[index], detours[index],
                           reinterpret_cast<void**>(&g_handler_original[index])))
            return 0;
    }
    if (!api->HookCode(mod, "oc_custom.world_send", reinterpret_cast<void*>(&SendHook),
                       reinterpret_cast<void**>(&g_send_original)))
        return 0;
    const bool lobby = RegisterExtension();
    if (!lobby)
        api->Log(mod, SDGO_LOG_WARN,
                 "friend_unit_gift_server is not loaded; OC lobby requests are unavailable (records still carry "
                 "existing allocations)");
    api->Log(mod, SDGO_LOG_INFO, "ready: mode=%s lobby=%s game=%ls", g_settings.oc_mode ? "oc" : "current",
             lobby ? "carrier" : "none", g_game_db.c_str());
    return 1;
}
