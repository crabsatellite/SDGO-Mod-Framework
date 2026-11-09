#include "../mods/shop_localization/unit_name_display.h"

#include <windows.h>
#include <intrin.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <limits>
#include <string_view>

#pragma intrinsic(_ReturnAddress)

namespace {

static_assert(sizeof(void*) == 4, "native caller fixture requires Win32");

using Caller =
    void(__cdecl*)(void*, void*, const std::uint8_t*);
using Hook =
    void(__fastcall*)(void*, void*, void*, const std::uint8_t*);

struct DisplayStub {
    static constexpr std::size_t kBytes = 22;
    static constexpr std::size_t kCall = 16;
    std::uint8_t* code = nullptr;

    bool Create() {
        code = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, kBytes, MEM_RESERVE | MEM_COMMIT,
                         PAGE_EXECUTE_READWRITE));
        if (!code) return false;
        const std::uint8_t body[kBytes] = {
            0x8B, 0x4C, 0x24, 0x04,        // mov ecx,[esp+4]
            0x33, 0xD2,                    // xor edx,edx
            0x8B, 0x44, 0x24, 0x0C,        // mov eax,[esp+12] record
            0x50,                          // push eax
            0x8B, 0x44, 0x24, 0x0C,        // mov eax,[esp+12] control
            0x50,                          // push eax
            0xE8, 0x00, 0x00, 0x00, 0x00, // call rel32
            0xC3                           // ret
        };
        std::memcpy(code, body, sizeof(body));
        return true;
    }

    bool Target(void* target) {
        if (!code || !target) return false;
        const auto next =
            reinterpret_cast<std::intptr_t>(code + kCall + 5);
        const auto distance =
            reinterpret_cast<std::intptr_t>(target) - next;
        if (distance < (std::numeric_limits<std::int32_t>::min)() ||
            distance > (std::numeric_limits<std::int32_t>::max)()) {
            return false;
        }
        const auto relative = static_cast<std::int32_t>(distance);
        std::memcpy(code + kCall + 1, &relative, sizeof(relative));
        return FlushInstructionCache(GetCurrentProcess(), code, kBytes) != 0;
    }

    void* Site() const { return code ? code + kCall : nullptr; }
    Caller Invoke() const { return reinterpret_cast<Caller>(code); }

    void Release() {
        if (code) VirtualFree(code, 0, MEM_RELEASE);
        code = nullptr;
    }
};

struct ProbeStub {
    static constexpr std::size_t kBytes = 6;
    std::uint8_t* code = nullptr;

    bool Create(void* target) {
        code = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, kBytes, MEM_RESERVE | MEM_COMMIT,
                         PAGE_EXECUTE_READWRITE));
        if (!code || !target) return false;
        code[0] = 0xE8;
        const auto next = reinterpret_cast<std::intptr_t>(code + 5);
        const auto distance =
            reinterpret_cast<std::intptr_t>(target) - next;
        if (distance < (std::numeric_limits<std::int32_t>::min)() ||
            distance > (std::numeric_limits<std::int32_t>::max)()) {
            return false;
        }
        const auto relative = static_cast<std::int32_t>(distance);
        std::memcpy(code + 1, &relative, sizeof(relative));
        code[5] = 0xC3;
        return FlushInstructionCache(GetCurrentProcess(), code, kBytes) != 0;
    }

    void* Site() const { return code; }
    void Invoke() const {
        reinterpret_cast<void(__cdecl*)()>(code)();
    }

    void Release() {
        if (code) VirtualFree(code, 0, MEM_RELEASE);
        code = nullptr;
    }
};

DisplayStub g_hangar;
DisplayStub g_header;
DisplayStub g_grid;
DisplayStub g_trade;
DisplayStub g_capsule_preview;
DisplayStub g_factory_preview;
DisplayStub g_mixing_preview;
DisplayStub g_shop_preview;
DisplayStub g_unknown;
ProbeStub g_plain_assign;
ProbeStub g_display_assign;
ProbeStub g_preview_assign;
Hook g_plain_hook = nullptr;
Hook g_preview_hook = nullptr;
Hook g_display_hook = nullptr;
int g_checks = 0;
int g_failures = 0;
int g_plain_original_calls = 0;
int g_preview_original_calls = 0;
int g_display_original_calls = 0;
int g_probe_calls = 0;
int g_context_hits = 0;
sdgo::unit_name_display::ContextView g_last_context{};
bool g_wrong_internal = false;
bool g_nested = false;
const std::uint8_t* g_nested_record = nullptr;

void Check(bool value, const char* message) {
    ++g_checks;
    if (value) return;
    ++g_failures;
    std::printf("FAIL %s\n", message);
}

void __cdecl Probe() {
    ++g_probe_calls;
    sdgo::unit_name_display::ContextView value{};
    value.unit_id = 0xA5A5A5A5u;
    value.primary_chars = 0x5A5A5A5Au;
    if (sdgo::unit_name_display::CurrentUnitNameContext(
            _ReturnAddress(), &value)) {
        ++g_context_hits;
        g_last_context = value;
    } else {
        Check(value.unit_id == 0xA5A5A5A5u &&
                  value.primary_chars == 0x5A5A5A5Au,
              "a declined context leaves the caller output untouched");
    }
}

void __fastcall PlainOriginal(void*, void*, void*,
                              const std::uint8_t*) {
    ++g_plain_original_calls;
    if (g_wrong_internal)
        g_preview_assign.Invoke();
    else
        g_plain_assign.Invoke();
    if (g_nested) {
        g_capsule_preview.Invoke()(reinterpret_cast<void*>(0x12345678),
                                   nullptr, g_nested_record);
        g_plain_assign.Invoke();
    }
    SetLastError(0x1234);
}

void __fastcall DisplayOriginal(void*, void*, void*,
                                const std::uint8_t*) {
    ++g_display_original_calls;
    if (g_wrong_internal)
        g_plain_assign.Invoke();
    else
        g_display_assign.Invoke();
    SetLastError(0x2345);
}

void __fastcall PreviewOriginal(void*, void*, void*,
                                const std::uint8_t*) {
    ++g_preview_original_calls;
    if (g_wrong_internal)
        g_display_assign.Invoke();
    else
        g_preview_assign.Invoke();
    SetLastError(0x3456);
}

void* __cdecl Symbol(SdgoMod*, const char* name) {
    if (!std::strcmp(name, "shop_localization.unit_name_display.call.005F6BFD"))
        return g_hangar.Site();
    if (!std::strcmp(name, "shop_localization.unit_name_display.call.00620E58"))
        return g_header.Site();
    if (!std::strcmp(name, "shop_localization.unit_name_display.call.0083A5A4"))
        return g_grid.Site();
    if (!std::strcmp(name, "shop_localization.unit_name_display.call.00970764"))
        return g_trade.Site();
    if (!std::strcmp(name, "shop_localization.unit_name_display.call.00812654"))
        return g_capsule_preview.Site();
    if (!std::strcmp(name, "shop_localization.unit_name_display.call.0089751A"))
        return g_factory_preview.Site();
    if (!std::strcmp(name, "shop_localization.unit_name_display.call.008C051C"))
        return g_mixing_preview.Site();
    if (!std::strcmp(name, "shop_localization.unit_name_display.call.009CA9D4"))
        return g_shop_preview.Site();
    if (!std::strcmp(name,
                     "shop_localization.unit_name_display.assign_call.006EF1F4"))
        return g_plain_assign.Site();
    if (!std::strcmp(name,
                     "shop_localization.unit_name_display.assign_call.006EF31D"))
        return g_display_assign.Site();
    if (!std::strcmp(name,
                     "shop_localization.unit_name_display.assign_call.006EF404"))
        return g_preview_assign.Site();
    return nullptr;
}

int __cdecl HookCode(SdgoMod*, const char* name, void* replacement,
                     void** original) {
    if (!std::strcmp(name, "shop_localization.unit_name_display.unit_name_5")) {
        g_plain_hook = reinterpret_cast<Hook>(replacement);
        *original = reinterpret_cast<void*>(&PlainOriginal);
        return 1;
    }
    if (!std::strcmp(name, "shop_localization.unit_name_display.unit_name_205")) {
        g_display_hook = reinterpret_cast<Hook>(replacement);
        *original = reinterpret_cast<void*>(&DisplayOriginal);
        return 1;
    }
    if (!std::strcmp(name, "shop_localization.unit_name_display.unit_name_105")) {
        g_preview_hook = reinterpret_cast<Hook>(replacement);
        *original = reinterpret_cast<void*>(&PreviewOriginal);
        return 1;
    }
    return 0;
}

void Reset() {
    g_plain_original_calls = 0;
    g_preview_original_calls = 0;
    g_display_original_calls = 0;
    g_probe_calls = 0;
    g_context_hits = 0;
    g_last_context = {};
    g_wrong_internal = false;
    g_nested = false;
    g_nested_record = nullptr;
    SetLastError(0);
}

std::array<std::uint8_t, 107> Record(
    std::uint32_t id, const wchar_t* primary) {
    std::array<std::uint8_t, 107> record{};
    std::memcpy(record.data(), &id, sizeof(id));
    for (std::size_t index = 0; primary[index]; ++index) {
        const auto code = static_cast<std::uint16_t>(primary[index]);
        std::memcpy(record.data() + 5 + index * sizeof(code),
                    &code, sizeof(code));
    }
    return record;
}

void Invoke(const DisplayStub& stub,
            const std::uint8_t* record) {
    stub.Invoke()(reinterpret_cast<void*>(0x12345678), nullptr,
                  record);
}

bool PrimaryEquals(const wchar_t* expected) {
    const auto chars = std::wcslen(expected);
    return g_last_context.primary_chars == chars &&
           std::wstring_view(g_last_context.primary_name,
                             g_last_context.primary_chars) ==
               expected;
}

} // namespace

int main() {
    Check(g_hangar.Create() && g_header.Create() &&
              g_grid.Create() && g_trade.Create() &&
              g_capsule_preview.Create() &&
              g_factory_preview.Create() &&
              g_mixing_preview.Create() &&
              g_shop_preview.Create() && g_unknown.Create() &&
              g_plain_assign.Create(reinterpret_cast<void*>(&Probe)) &&
              g_display_assign.Create(reinterpret_cast<void*>(&Probe)) &&
              g_preview_assign.Create(reinterpret_cast<void*>(&Probe)),
          "audited UI and internal assignment stubs allocated");

    SdgoApi api{};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Symbol = &Symbol;
    api.HookCode = &HookCode;
    Check(sdgo::unit_name_display::Attach(
              &api, reinterpret_cast<SdgoMod*>(1)),
          "narrow unit-name context hooks attached");
    Check(g_plain_hook && g_preview_hook && g_display_hook,
          "all three native helper detours captured");
    Check(g_hangar.Target(reinterpret_cast<void*>(g_display_hook)) &&
              g_header.Target(reinterpret_cast<void*>(g_display_hook)) &&
              g_grid.Target(reinterpret_cast<void*>(g_plain_hook)) &&
              g_trade.Target(reinterpret_cast<void*>(g_plain_hook)) &&
              g_capsule_preview.Target(
                  reinterpret_cast<void*>(g_preview_hook)) &&
              g_factory_preview.Target(
                  reinterpret_cast<void*>(g_preview_hook)) &&
              g_mixing_preview.Target(
                  reinterpret_cast<void*>(g_preview_hook)) &&
              g_shop_preview.Target(
                  reinterpret_cast<void*>(g_preview_hook)) &&
              g_unknown.Target(reinterpret_cast<void*>(g_plain_hook)),
          "caller stubs target the installed helper detours");

    const auto stock =
        Record(25060, L"\u539F\u7248\u4E3B\u540D\u79F0");

    Reset();
    Invoke(g_grid, stock.data());
    Check(g_plain_original_calls == 1 && g_probe_calls == 1 &&
              g_context_hits == 1 &&
              g_last_context.unit_id == 25060 &&
              PrimaryEquals(L"\u539F\u7248\u4E3B\u540D\u79F0"),
          "verified unit-grid caller exposes copied identity and primary name");
    Check(GetLastError() == 0x1234,
          "plain helper retains the original last-error result");

    Reset();
    Invoke(g_trade, stock.data());
    Check(g_plain_original_calls == 1 && g_context_hits == 1 &&
              PrimaryEquals(L"\u539F\u7248\u4E3B\u540D\u79F0"),
          "verified trade caller exposes the same primary-name context");

    Reset();
    Invoke(g_hangar, stock.data());
    Check(g_display_original_calls == 1 && g_context_hits == 1 &&
              g_last_context.unit_id == 25060 &&
              PrimaryEquals(L"\u539F\u7248\u4E3B\u540D\u79F0"),
          "verified hangar caller exposes the formatted-name context");
    Check(GetLastError() == 0x2345,
          "formatted helper retains the original last-error result");

    Reset();
    Invoke(g_header, stock.data());
    Check(g_display_original_calls == 1 && g_context_hits == 1,
          "verified unit-information header exposes its context");

    const std::array<const DisplayStub*, 4> preview_callers = {
        &g_capsule_preview, &g_factory_preview, &g_mixing_preview,
        &g_shop_preview};
    for (const auto* caller : preview_callers) {
        Reset();
        Invoke(*caller, stock.data());
        Check(g_preview_original_calls == 1 &&
                  g_context_hits == 1 &&
                  g_last_context.unit_id == 25060 &&
                  PrimaryEquals(L"\u539F\u7248\u4E3B\u540D\u79F0"),
              "verified shared-preview caller exposes its context");
        Check(GetLastError() == 0x3456,
              "shared-preview helper retains original last-error");
    }

    Reset();
    Invoke(g_unknown, stock.data());
    Check(g_plain_original_calls == 1 && g_probe_calls == 1 &&
              g_context_hits == 0,
          "unknown helper caller executes original without a context");

    Reset();
    g_wrong_internal = true;
    Invoke(g_grid, stock.data());
    Check(g_probe_calls == 1 && g_context_hits == 0,
          "a different internal assignment return cannot claim context");

    Reset();
    const auto zero = Record(0, L"\u65E0\u6548");
    Invoke(g_grid, zero.data());
    Check(g_plain_original_calls == 1 && g_context_hits == 0,
          "a zero unit identity executes original without a context");

    Reset();
    Invoke(g_grid, reinterpret_cast<const std::uint8_t*>(1));
    Check(g_plain_original_calls == 1 && g_context_hits == 0,
          "an unreadable record cannot expose a context");

    Reset();
    auto unterminated = Record(25061, L"");
    for (std::size_t index = 0; index < 50; ++index) {
        const std::uint16_t code = L'X';
        std::memcpy(unterminated.data() + 5 + index * sizeof(code),
                    &code, sizeof(code));
    }
    Invoke(g_grid, unterminated.data());
    Check(g_context_hits == 1 &&
              g_last_context.unit_id == 25061 &&
              g_last_context.primary_chars == 0 &&
              g_last_context.primary_name[0] == L'\0',
          "unterminated primary text cannot escape the bounded context copy");

    Reset();
    const auto nested =
        Record(90001, L"\u81EA\u5B9A\u4E49\u4E3B\u540D\u79F0");
    g_nested = true;
    g_nested_record = nested.data();
    Invoke(g_grid, stock.data());
    Check(g_plain_original_calls == 1 &&
              g_preview_original_calls == 1 &&
              g_probe_calls == 3 && g_context_hits == 3 &&
              g_last_context.unit_id == 25060 &&
              PrimaryEquals(L"\u539F\u7248\u4E3B\u540D\u79F0"),
          "a nested verified helper restores the outer copied context");

    Reset();
    g_plain_assign.Invoke();
    Check(g_probe_calls == 1 && g_context_hits == 0,
          "the context ends when the native helper returns");

    g_hangar.Release();
    g_header.Release();
    g_grid.Release();
    g_trade.Release();
    g_capsule_preview.Release();
    g_factory_preview.Release();
    g_mixing_preview.Release();
    g_shop_preview.Release();
    g_unknown.Release();
    g_plain_assign.Release();
    g_display_assign.Release();
    g_preview_assign.Release();

    if (g_failures) {
        std::printf("unit_name_display_test: %d/%d checks failed\n",
                    g_failures, g_checks);
        return 1;
    }
    std::printf("unit_name_display_test: %d checks passed\n",
                g_checks);
    return 0;
}
