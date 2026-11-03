#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include "sdgo/sdgo_api.h"
#include "policy.h"
#include "native_names.h"

namespace {
using ListFn = int(__thiscall*)(void*, void*, void*, void*, int, int*);
ListFn originalList = nullptr;
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
std::wstring database;
std::mutex setupMutex;
bool prepared = false;
std::atomic<unsigned> failures{0};
wchar_t replacement[] = L"{call [sdgo_friend_list_fix].[sp_FriendsList](?,?,?,?,?)}";
void* nativeCommand = nullptr;
UINT nameCodePage = 0;
std::atomic<unsigned> nameReceipts{0};

bool PrepareNative(void* connection) {
    sdgo::friend_fix::Session session;
    if (!session.Open(connection) || !sdgo::friend_fix::Prepare(session, database)) return false;
    std::wstring codePage;
    if (!session.String(L"SELECT CONVERT(nvarchar(20),COLLATIONPROPERTY(collation_name,'CodePage')) FROM sys.columns WHERE object_id=OBJECT_ID(N'dbo.tbl_Account_Pilot') AND name=N'f_pilot'",codePage)) return false;
    const auto value = std::stoul(codePage);
    if (value > UINT_MAX || !value || !IsValidCodePage(static_cast<UINT>(value))) return false;
    nameCodePage = static_cast<UINT>(value);
    return true;
}
bool (*preparePolicy)(void*) = &PrepareNative;
bool Ensure(void* connection) {
    std::lock_guard<std::mutex> guard(setupMutex);
    if (!prepared) prepared = preparePolicy(connection);
    return prepared;
}
void EmptyCount(int* count) noexcept {
    __try { if (count) *count = 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
unsigned SafeCount(int* count, int capacity) noexcept {
    __try {
        return count && capacity > 0 && capacity <= 65535 / sdgo::friend_fix::kFriendRowBytes &&
            *count > 0 && *count <= capacity ? static_cast<unsigned>(*count) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
void RepairNames(void* connection, void* rows, unsigned count) {
    if (!rows || !count || !nameCodePage) return;
    sdgo::friend_fix::Session session;
    if (!session.Open(connection)) return;
    const auto repaired = sdgo::friend_fix::RepairFriendNames(rows,count,nameCodePage,
        [&](std::uint32_t id,std::wstring& value) {
            return session.String(L"SELECT f_pilot FROM dbo.tbl_Account_Pilot WHERE f_idx="+
                                  std::to_wstring(id),value);
        });
    if (repaired && nameReceipts.fetch_add(1) < 5)
        g_api->Log(g_mod,SDGO_LOG_INFO,"native friend name encoding refreshed: rows=%u codepage=%u",repaired,nameCodePage);
}
int __fastcall List(void* self, void*, void* connection, void* args,
                    void* rows, int capacity, int* count) {
    const DWORD error = GetLastError();
    bool ready = false;
    try { ready = connection && Ensure(connection); } catch (...) {}
    if (!ready) {
        EmptyCount(count);
        if (failures.fetch_add(1) < 5)
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                "friend list policy unavailable; native database failure returned, retry is allowed");
        SetLastError(error);
        return 1;
    }
    SetLastError(error);
    EmptyCount(count);
    const int result = originalList(self, connection, args, rows, capacity, count);
    const DWORD nativeError = GetLastError();
    if (result == 0) {
        try { RepairNames(connection, rows, SafeCount(count,capacity)); } catch (...) {}
    }
    SetLastError(nativeError);
    return result;
}
}

SDGO_MOD_ENTRY {
    if (!api || api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, ModDirectory) + sizeof(api->ModDirectory) ||
        !api->Log || !api->Symbol || !api->HookCode || !api->HookSlot) return 0;
    g_api = api; g_mod = mod;
    wchar_t path[32768]{}, name[130]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, 32768);
    if (!length || length >= 32768) return 0;
    std::wstring ini(path);
    const auto dot = ini.find_last_of(L'.');
    if (dot == std::wstring::npos) return 0;
    ini.replace(dot, std::wstring::npos, L".ini");
    const auto n = GetPrivateProfileStringW(L"DB Member", L"DBName", L"", name, 130, ini.c_str());
    if (!n || n > 128) return 0;
    database.assign(name, n);
    const auto* command = static_cast<const wchar_t*>(api->Symbol(mod, "friends.command"));
    if (!command || wcscmp(command, L"{call dbo.sp_FriendsList(?,?,?,?,?)}") ||
        !api->Symbol(mod, "friends.connection_layout") ||
        !api->Symbol(mod, "friends.args_layout") || !api->Symbol(mod, "friends.row_layout") ||
        !api->Symbol(mod, "friends.name_layout")) return 0;
    if (!api->HookCode(mod, "friends.list", reinterpret_cast<void*>(&List),
                      reinterpret_cast<void**>(&originalList))) return 0;
    if (!api->HookSlot(mod, "friends.command_slot", replacement, &nativeCommand)) return 0;
    api->Log(mod, SDGO_LOG_INFO,
        "friend list fix registered; owned SQL query initializes on the first list request; native dbo procedure retained");
    return 1;
}
