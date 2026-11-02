#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include "sdgo/sdgo_api.h"
#include "policy.h"

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

bool PrepareNative(void* connection) {
    sdgo::friend_fix::Session session;
    return session.Open(connection) && sdgo::friend_fix::Prepare(session, database);
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
    return originalList(self, connection, args, rows, capacity, count);
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
        !api->Symbol(mod, "friends.args_layout") || !api->Symbol(mod, "friends.row_layout")) return 0;
    if (!api->HookCode(mod, "friends.list", reinterpret_cast<void*>(&List),
                      reinterpret_cast<void**>(&originalList))) return 0;
    if (!api->HookSlot(mod, "friends.command_slot", replacement, &nativeCommand)) return 0;
    api->Log(mod, SDGO_LOG_INFO,
        "friend list fix registered; owned SQL query initializes on the first list request; native dbo procedure retained");
    return 1;
}
