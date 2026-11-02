// ime_fix - bounded native IME fixes for this GOnline.exe build.
//
// One call site reads GCS_RESULTSTR from ImmGetCompositionStringW and treats
// the returned BYTE count as a CHARACTER count, so the terminator it writes
// lands past the committed text (or past the buffer). Only for that call site
// (recognised by its return address) the hook returns the character count and
// terminates the string itself. Every other caller sees the normal Win32 contract.

#include <windows.h>
#include <imm.h>
#include <intrin.h>

#include <cstdint>
#include <cstring>

#include "sdgo/sdgo_api.h"

#pragma intrinsic(_ReturnAddress)

namespace
{
    using ImmGetCompositionStringWFn = LONG(WINAPI*)(HIMC, DWORD, LPVOID, DWORD);
    using MessageFilterFn = bool(__stdcall*)(void*, HWND*, UINT*, WPARAM*, LPARAM*);
    using AssignFn = void*(__thiscall*)(void*, const wchar_t*, uint32_t);
    using ReplaceFn = void*(__thiscall*)(void*, uint32_t, uint32_t, const void*, uint32_t, uint32_t);

    // ime.result_call starts at the "push 0x208" before the call; the call returns 0x17 bytes later.
    constexpr uintptr_t kReturnOffset = 0x17;

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    ImmGetCompositionStringWFn g_original = nullptr;
    uintptr_t g_buggyReturn = 0;
    bool g_trace = false;
    MessageFilterFn g_filter = nullptr;
    AssignFn g_assign = nullptr;
    volatile LONG g_candidateReported = 0;
    volatile LONG g_endReported = 0;
    ReplaceFn g_replace = nullptr;
    uintptr_t g_resultReplaceReturn = 0;
    volatile LONG g_replaceReported = 0, g_replaceRefused = 0;
    constexpr uint32_t kMaxPreedit = 260;
    struct CommitSnapshot {
        void* handler;
        uint32_t position, length;
        bool matched;
        wchar_t preedit[kMaxPreedit];
    };
    thread_local CommitSnapshot* g_commit = nullptr;

    bool NativeText(const void* value, const wchar_t** data, uint32_t* size)
    {
        const auto* words = static_cast<const uint32_t*>(value);
        const uint32_t capacity = words[5];
        *size = words[4];
        if (capacity > 0x10000 || *size > capacity || (capacity < 8 && capacity != 7))
            return false;
        *data = capacity < 8 ? static_cast<const wchar_t*>(value) : *static_cast<const wchar_t* const*>(value);
        return *data != nullptr;
    }

    void CaptureCommit(void* handler, CommitSnapshot* snapshot)
    {
        snapshot->handler = handler;
        __try {
            const auto* bytes = static_cast<const uint8_t*>(handler);
            const wchar_t *preedit = nullptr, *display = nullptr;
            uint32_t length = 0, displaySize = 0;
            if (!NativeText(bytes + 0xA8, &preedit, &length) || !length || length > kMaxPreedit ||
                !NativeText(bytes + 0xC4, &display, &displaySize))
                return;
            snapshot->length = length;
            snapshot->position = *reinterpret_cast<const uint32_t*>(bytes + 0x48);
            if (snapshot->position > displaySize || length > displaySize - snapshot->position ||
                std::memcmp(display + snapshot->position, preedit, length * sizeof(wchar_t)))
                return;
            std::memcpy(snapshot->preedit, preedit, length * sizeof(wchar_t));
            snapshot->matched = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            snapshot->matched = false;
        }
    }

    bool MatchCommit(void* self, uint32_t position, uint32_t eraseCount, const void* source,
                     uint32_t sourcePosition, uint32_t sourceCount, uint32_t* oldLength)
    {
        __try {
            if (!g_commit || !g_commit->matched || sourcePosition || sourceCount != UINT32_MAX)
                return false;
            const auto& snapshot = *g_commit;
            const auto* bytes = static_cast<const uint8_t*>(snapshot.handler);
            if (self != bytes + 0xC4 || source != bytes + 0xA8 || position != snapshot.position)
                return false;
            const wchar_t *display = nullptr, *result = nullptr;
            uint32_t size = 0, resultSize = 0;
            if (!NativeText(self, &display, &size) || !NativeText(source, &result, &resultSize) ||
                resultSize > kMaxPreedit || eraseCount != resultSize ||
                position > size || snapshot.length > size - position ||
                std::memcmp(display + position, snapshot.preedit, snapshot.length * sizeof(wchar_t)))
                return false;
            *oldLength = snapshot.length;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void* __fastcall FixedReplace(void* self, void*, uint32_t position, uint32_t eraseCount,
                                  const void* source, uint32_t sourcePosition, uint32_t sourceCount)
    {
        // The native RESULT branch overwrites A8/B8 with the new result before
        // taking B8 as the old display range's erase count. Match the pre-result
        // snapshot exactly; never infer a range by looking for English letters.
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_resultReplaceReturn && g_commit) {
            uint32_t oldLength = 0;
            if (MatchCommit(self, position, eraseCount, source, sourcePosition, sourceCount, &oldLength)) {
                if ((oldLength != eraseCount || g_trace) &&
                    (g_trace || InterlockedCompareExchange(&g_replaceReported, 1, 0) == 0))
                    g_api->Log(g_mod, SDGO_LOG_INFO,
                               "native IME result erase count corrected: %u -> %u UTF-16 units; no text content logged",
                               eraseCount, oldLength);
                eraseCount = oldLength;
            } else if (g_trace || InterlockedCompareExchange(&g_replaceRefused, 1, 0) == 0) {
                g_api->Log(g_mod, SDGO_LOG_WARN,
                           "native IME result replacement retained: preedit/display guard mismatch (cached %u, native erase %u)",
                           g_commit->length, eraseCount);
            }
        }
        return g_replace(self, position, eraseCount, source, sourcePosition, sourceCount);
    }

    // This is the game's CIMEHandler, not a process-wide IME or Windows setting.
    bool ChineseEdit(void* handler, const HWND* window)
    {
        __try {
            if (!handler || !window || !*window)
                return false;
            const auto* bytes = static_cast<const uint8_t*>(handler);
            const auto layout = *reinterpret_cast<const uint32_t*>(bytes + 0x60);
            return PRIMARYLANGID(LOWORD(layout)) == LANG_CHINESE &&
                   *window == *reinterpret_cast<const HWND*>(bytes + 0x134);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool ClearPreedit(void* handler, uint32_t* previousLength)
    {
        __try {
            auto* bytes = static_cast<uint8_t*>(handler);
            auto* text = reinterpret_cast<uint32_t*>(bytes + 0xA8);
            const uint32_t size = text[4], capacity = text[5];
            if (capacity > 0x10000 || size > capacity || (capacity < 8 && capacity != 7))
                return false;
            *previousLength = size;
            // Native allocator/string implementation; never assign a modern-CRT
            // std::wstring into the host object. Keep committed 0x70/0xC4 and the
            // hidden EDIT's text untouched. Its next update rebuilds the display.
            g_assign(text, L"", 0);
            *reinterpret_cast<uint32_t*>(bytes + 0x5C) = 0;
            *reinterpret_cast<uint32_t*>(bytes + 0x54) = 0;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool __stdcall FixedMessageFilter(void* handler, HWND* window, UINT* message,
                                      WPARAM* wparam, LPARAM* lparam)
    {
        const UINT incomingMessage = message ? *message : 0;
        const LPARAM incomingFlags = lparam ? *lparam : 0;
        const bool chinese = ChineseEdit(handler, window);
        CommitSnapshot snapshot{};
        auto* previous = g_commit;
        if (chinese && incomingMessage == WM_IME_COMPOSITION &&
            ((incomingFlags & GCS_RESULTSTR) || !incomingFlags)) {
            CaptureCommit(handler, &snapshot);
            g_commit = &snapshot;
        }
        bool consumed = false;
        __try {
            consumed = g_filter(handler, window, message, wparam, lparam);
        } __finally {
            g_commit = previous;
        }
        if (!chinese)
            return consumed;

        if (incomingMessage == WM_IME_SETCONTEXT && lparam && !consumed) {
            constexpr LPARAM candidates = ISC_SHOWUIALLCANDIDATEWINDOW;
            const LPARAM nativeFlags = *lparam;
            *lparam = (nativeFlags & ~candidates) | (incomingFlags & candidates);
            if (*lparam != nativeFlags &&
                (g_trace || InterlockedCompareExchange(&g_candidateReported, 1, 0) == 0))
                g_api->Log(g_mod, SDGO_LOG_INFO,
                           "native Chinese EDIT candidate visibility flags restored; system UI rendering still requires visual acceptance");
        } else if (incomingMessage == WM_IME_ENDCOMPOSITION) {
            uint32_t previousLength = 0;
            if (!ClearPreedit(handler, &previousLength)) {
                g_api->Log(g_mod, SDGO_LOG_WARN, "composition-end preedit cleanup refused: unsupported native string");
            } else if (previousLength &&
                       (g_trace || InterlockedCompareExchange(&g_endReported, 1, 0) == 0)) {
                g_api->Log(g_mod, SDGO_LOG_INFO,
                           "cleared stale native Chinese preedit at composition end (%u UTF-16 units); no text content logged",
                           previousLength);
            }
        }
        return consumed;
    }

    LONG WINAPI FixedImmGetCompositionStringW(HIMC context, DWORD index, LPVOID buffer, DWORD bufferBytes)
    {
        const LONG result = g_original(context, index, buffer, bufferBytes);
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_buggyReturn || index != GCS_RESULTSTR)
            return result;

        if (result <= 0) {
            if (buffer && bufferBytes >= sizeof(wchar_t))
                *static_cast<wchar_t*>(buffer) = L'\0';
            if (g_trace)
                g_api->Log(g_mod, SDGO_LOG_INFO, "GCS_RESULTSTR error %ld normalized to 0", result);
            return 0;
        }
        if (!buffer || bufferBytes < sizeof(wchar_t))
            return 0;

        DWORD bytes = static_cast<DWORD>(result);
        if (bytes > bufferBytes - sizeof(wchar_t))
            bytes = bufferBytes - sizeof(wchar_t);
        bytes &= ~static_cast<DWORD>(1);
        static_cast<wchar_t*>(buffer)[bytes / sizeof(wchar_t)] = L'\0';

        const LONG characters = static_cast<LONG>(bytes / sizeof(wchar_t));
        if (g_trace)
            g_api->Log(g_mod, SDGO_LOG_INFO, "GCS_RESULTSTR %ld bytes -> %ld chars (hkl %p)", result, characters,
                       GetKeyboardLayout(0));
        return characters;
    }
}

SDGO_MOD_ENTRY
{
    if (!api || api->version != SDGO_API_VERSION || !api->Symbol || !api->HookCode ||
        !api->HookImport || !api->SettingInt || !api->Log)
        return 0;
    g_api = api;
    g_mod = mod;
    g_trace = api->SettingInt(mod, "Trace", 0) != 0;

    auto site = reinterpret_cast<uintptr_t>(api->Symbol(mod, "ime.result_call"));
    if (!site)
        return 0;
    g_buggyReturn = site + kReturnOffset;
    g_assign = reinterpret_cast<AssignFn>(api->Symbol(mod, "ime.native_string_assign"));
    auto replaceSite = reinterpret_cast<uintptr_t>(api->Symbol(mod, "ime.result_replace_call"));
    if (!g_assign || !replaceSite || !api->Symbol(mod, "ime.native_replace") || !api->Symbol(mod, "ime.message_filter"))
        return 0;
    g_resultReplaceReturn = replaceSite + 5;

    if (!api->HookImport(mod, "imm32.dll", "ImmGetCompositionStringW",
                           reinterpret_cast<void*>(&FixedImmGetCompositionStringW),
                           reinterpret_cast<void**>(&g_original)) ||
        !api->HookCode(mod, "ime.message_filter", reinterpret_cast<void*>(&FixedMessageFilter),
                       reinterpret_cast<void**>(&g_filter)) ||
        !api->HookCode(mod, "ime.native_replace", reinterpret_cast<void*>(&FixedReplace),
                       reinterpret_cast<void**>(&g_replace)))
        return 0;
    api->Log(mod, SDGO_LOG_INFO,
             "ready: scoped commit length, Chinese candidates, guarded old-preedit result replacement and END cleanup; no system IME settings changed");
    return 1;
}
