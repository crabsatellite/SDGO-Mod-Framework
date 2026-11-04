// Restore the shipped messenger layout and let a verified accepted offline
// friend open the game's existing conversation UI. Message transport, errors
// and any server-side delivery policy remain native.
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sdgo/sdgo_api.h"
#include "chat_open_policy.h"
#include "messenger_layout_policy.h"
#include "name_update_policy.h"
#include "community_menu.h"

namespace
{
    using CommunityLabels = void(__thiscall*)(void*);
    CommunityLabels g_communityLabels = nullptr;
    friend_list_client_fix::MenuText g_menuText = nullptr;
    using IgnoreCache = void*(__thiscall*)(void*);
    using IsBlocked = std::uint32_t(__thiscall*)(void*, const char*);
    using NativeAnsi = const char*(__cdecl*)(unsigned,const wchar_t*);
    void** g_playerData = nullptr;
    IgnoreCache g_ignoreCache = nullptr;
    IsBlocked g_isBlocked = nullptr;
    NativeAnsi g_nativeAnsi = nullptr;
    void __fastcall CommunityMenuLabels(void* self, void*) {
        g_communityLabels(self);
        __try {
            const auto* name=static_cast<const std::uint8_t*>(self)+0x68;
            const auto* wide=friend_list_client_fix::MenuRead<unsigned>(name,0x14)<8
                ? reinterpret_cast<const wchar_t*>(name)
                : friend_list_client_fix::MenuRead<const wchar_t*>(name,0);
            const auto* ansi=g_nativeAnsi(874,wide);
            const bool blocked=ansi && *g_playerData && g_isBlocked(g_ignoreCache(*g_playerData),ansi)!=0;
            friend_list_client_fix::LocalizeCommunityMenu(self,g_menuText,blocked);
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    using UpdateRecord = std::uintptr_t(__thiscall*)(void*, const void*);
    UpdateRecord g_originalUpdate = nullptr;

    std::uintptr_t __fastcall UpdateFriendRecord(void* self, void*, const void* incoming)
    {
        if (!friend_list_client_fix::PreserveKnownName(self,incoming))
            return g_originalUpdate(self,incoming);
        std::uint8_t row[friend_list_client_fix::kUpdateRowBytes];
        std::memcpy(row,incoming,sizeof(row));
        std::memcpy(row + friend_list_client_fix::kNameOffset,
                    static_cast<const std::uint8_t*>(self) + friend_list_client_fix::kNameOffset,
                    friend_list_client_fix::kNameBytes);
        return g_originalUpdate(self,row);
    }

    void* g_originalGate = nullptr;
    void* g_originalServiceLayoutCall = nullptr;
    void* g_originalPopupLayoutCall = nullptr;
    const wchar_t* g_originalLayout = nullptr;
    const wchar_t* const g_friendLayout =
        friend_list_client_fix::kFriendLayoutPath;

    bool IsExpectedAssignCall(
        const void* callSite, const void* returnAddress, const void* assign)
    {
        const auto callValue =
            reinterpret_cast<std::uintptr_t>(callSite);
        const auto returnValue =
            reinterpret_cast<std::uintptr_t>(returnAddress);
        if (!callValue || returnValue != callValue + 5) return false;
        const auto* call =
            reinterpret_cast<const std::uint8_t*>(callValue);
        std::int32_t displacement = 0;
        std::memcpy(&displacement, call + 1, sizeof(displacement));
        return call[0] == 0xE8 &&
               returnValue + static_cast<std::intptr_t>(displacement) ==
                   reinterpret_cast<std::uintptr_t>(assign);
    }

    // HookSite replaces the native five-byte CALL with a jump. At each site,
    // [ESP] is the pushed text pointer and [ESP+4] is its length. Rewrite only
    // the exact shipped 1364 argument, then let that site's trampoline replay
    // the original CALL to std::wstring::assign and continue at its native
    // return address. The temporary EAX value and incoming flags are restored
    // before the trampoline.
    __declspec(naked) void ServiceMessengerLayoutCall()
    {
        __asm {
            pushfd
            push eax
            cmp dword ptr [esp + 0Ch], 13
            jne preserve_native
            mov eax, dword ptr [g_originalLayout]
            cmp dword ptr [esp + 8], eax
            jne preserve_native
            mov eax, dword ptr [g_friendLayout]
            mov dword ptr [esp + 8], eax
        preserve_native:
            pop eax
            popfd
            jmp dword ptr [g_originalServiceLayoutCall]
        }
    }

    __declspec(naked) void PopupMessengerLayoutCall()
    {
        __asm {
            pushfd
            push eax
            cmp dword ptr [esp + 0Ch], 13
            jne preserve_native
            mov eax, dword ptr [g_originalLayout]
            cmp dword ptr [esp + 8], eax
            jne preserve_native
            mov eax, dword ptr [g_friendLayout]
            mov dword ptr [esp + 8], eax
        preserve_native:
            pop eax
            popfd
            jmp dword ptr [g_originalPopupLayoutCall]
        }
    }

    // Native site 0x00964549 follows:
    //   cmp dword ptr [eax+1Ch], 0
    //   je  0x00964553
    // EAX is the selected FriendTree row. The earlier native selection guard has
    // already returned when the tree has no selected payload. Preserve the
    // original flags unless this is a complete accepted offline-friend record.
    __declspec(naked) void OfflineChatGate()
    {
        __asm {
            pushfd
            cmp dword ptr [eax], 0
            je preserve_native
            cmp byte ptr [eax + 0Ah], 0
            je preserve_native
            cmp byte ptr [eax + 1Bh], 7
            jne preserve_native
            cmp dword ptr [eax + 1Ch], 0
            jne preserve_native
            and dword ptr [esp], 0FFFFFFBFh
        preserve_native:
            popfd
            jmp dword ptr [g_originalGate]
        }
    }
}

SDGO_MOD_ENTRY
{
    if (!api || api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) ||
        !api->Symbol || !api->HookSite || !api->HookCode || !api->Log) {
        return 0;
    }

    void* assign =
        api->Symbol(mod, friend_list_client_fix::kAssignSymbol);
    void* serviceCall =
        api->Symbol(mod, friend_list_client_fix::kServiceLayoutCallSymbol);
    void* popupCall =
        api->Symbol(mod, friend_list_client_fix::kPopupLayoutCallSymbol);
    void* serviceReturn =
        api->Symbol(mod, friend_list_client_fix::kServiceLayoutReturnSymbol);
    void* popupReturn =
        api->Symbol(mod, friend_list_client_fix::kPopupLayoutReturnSymbol);
    const auto* originalLayout = static_cast<const wchar_t*>(
        api->Symbol(mod, friend_list_client_fix::kOriginalLayoutSymbol));

    // This independent layout receipt pins the preceding presence comparison.
    // The hook symbol itself starts at the conditional branch so its trampoline
    // can replay the original JE after the detour selectively clears ZF.
    if (!assign || !serviceCall || !popupCall ||
        !serviceReturn || !popupReturn || !originalLayout ||
        !api->Symbol(mod, friend_list_client_fix::kLayoutSymbol) ||
        !api->Symbol(mod, friend_list_client_fix::kNameUpdateSymbol) ||
        std::memcmp(originalLayout,
                    friend_list_client_fix::kOriginalLayoutPath,
                    sizeof(friend_list_client_fix::kOriginalLayoutPath)) != 0 ||
        !IsExpectedAssignCall(serviceCall, serviceReturn, assign) ||
        !IsExpectedAssignCall(popupCall, popupReturn, assign)) {
        return 0;
    }

    g_menuText=reinterpret_cast<friend_list_client_fix::MenuText>(
        api->Symbol(mod,friend_list_client_fix::kCommunityTextSymbol));
    g_playerData=static_cast<void**>(api->Symbol(mod,"friends.player_data_pointer"));
    g_ignoreCache=reinterpret_cast<IgnoreCache>(api->Symbol(mod,"friend_list.client.ignore_cache"));
    g_isBlocked=reinterpret_cast<IsBlocked>(api->Symbol(mod,"friend_list.client.is_blocked"));
    g_nativeAnsi=reinterpret_cast<NativeAnsi>(api->Symbol(mod,"friend_list.client.native_ansi"));
    if (!g_menuText || !g_playerData || !g_ignoreCache || !g_isBlocked || !g_nativeAnsi ||
        !api->HookCode(mod,friend_list_client_fix::kCommunityLabelsSymbol,
            reinterpret_cast<void*>(&CommunityMenuLabels),reinterpret_cast<void**>(&g_communityLabels))) return 0;

    g_originalLayout = originalLayout;

    if (!api->HookSite(
            mod, friend_list_client_fix::kServiceLayoutCallSymbol,
            reinterpret_cast<void*>(&ServiceMessengerLayoutCall),
            &g_originalServiceLayoutCall) ||
        !api->HookSite(
            mod, friend_list_client_fix::kPopupLayoutCallSymbol,
            reinterpret_cast<void*>(&PopupMessengerLayoutCall),
            &g_originalPopupLayoutCall) ||
        !api->HookSite(mod, friend_list_client_fix::kGateSymbol,
                       reinterpret_cast<void*>(&OfflineChatGate),
                       &g_originalGate) ||
        !api->HookCode(mod, friend_list_client_fix::kNameUpdateSymbol,
                       reinterpret_cast<void*>(&UpdateFriendRecord),
                       reinterpret_cast<void**>(&g_originalUpdate))) {
        return 0;
    }

    api->Log(mod, SDGO_LOG_INFO,
             "two native messenger layout calls use the shipped friend layout "
             "without owning global string assignment; accepted offline "
             "friends may open the native conversation UI; message send, "
             "rejection and delivery remain native; lossy presence replies "
             "retain the known friend name while native status fields update");
    return 1;
}
