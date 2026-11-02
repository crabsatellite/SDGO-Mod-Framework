#include "shop_search.h"

#include <windows.h>
#include <intrin.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "sdgo/shop_search_protocol.h"

namespace {
using sdgo::shop_search::PrepareQuery;
using sdgo::shop_search::QueryDisposition;

using SearchFn = void(__thiscall*)(void*, std::uint32_t, void*);
using EditTextFn = void*(__thiscall*)(void*);
using WideToCpFn = const char*(__cdecl*)(UINT, const wchar_t*);
using ResourceToStringFn = void*(__thiscall*)(void*, void*);
using NativeAssignFn =
    void*(__thiscall*)(void*, const wchar_t*, std::uint32_t);

SearchFn g_search = nullptr;
EditTextFn g_editText = nullptr;
WideToCpFn g_wideToCp = nullptr;
ResourceToStringFn g_resourceToString = nullptr;
NativeAssignFn g_nativeAssign = nullptr;
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
const bool* g_searchEnabled = nullptr;
const bool* g_interfaceEnabled = nullptr;
std::uintptr_t g_submitConvertReturn = 0;
std::uintptr_t g_pageConvertReturn = 0;
std::uintptr_t g_rankAllReturn = 0;
std::uintptr_t g_paymentAllReturn = 0;
thread_local std::array<char, sdgo::shop_search::kDatabaseSearchBytes + 1>
    g_encodedKeyword = {};

enum class NativeReadDisposition {
    Ok,
    TooLong,
    Invalid
};

bool SearchEnabled()
{
    return g_searchEnabled && *g_searchEnabled;
}

bool InterfaceEnabled()
{
    return g_interfaceEnabled && *g_interfaceEnabled;
}

NativeReadDisposition ReadNativeString(
    const void* native, char16_t* output, std::size_t outputCapacity,
    std::size_t* outputSize)
{
    static_assert(sizeof(wchar_t) == sizeof(char16_t));
    if (!native || !output || !outputSize)
        return NativeReadDisposition::Invalid;
    __try {
        const auto* words = static_cast<const std::uint32_t*>(native);
        const auto size = words[4];
        const auto capacity = words[5];
        *outputSize = size;
        if (size > capacity)
            return NativeReadDisposition::Invalid;
        if (size >= outputCapacity)
            return NativeReadDisposition::TooLong;
        const auto* data = capacity < 8
            ? static_cast<const wchar_t*>(native)
            : *static_cast<const wchar_t* const*>(native);
        if (!data && size)
            return NativeReadDisposition::Invalid;
        std::memcpy(output, data, size * sizeof(char16_t));
        output[size] = 0;
        return NativeReadDisposition::Ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NativeReadDisposition::Invalid;
    }
}

NativeReadDisposition ReadTerminatedString(
    const wchar_t* text, char16_t* output, std::size_t outputCapacity,
    std::size_t* outputSize)
{
    static_assert(sizeof(wchar_t) == sizeof(char16_t));
    if (!text || !output || !outputSize)
        return NativeReadDisposition::Invalid;
    __try {
        std::size_t size = 0;
        while (size + 1 < outputCapacity && text[size]) {
            output[size] = static_cast<char16_t>(text[size]);
            ++size;
        }
        *outputSize = size;
        if (size + 1 == outputCapacity && text[size])
            return NativeReadDisposition::TooLong;
        output[size] = 0;
        return NativeReadDisposition::Ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NativeReadDisposition::Invalid;
    }
}

bool ReadControls(void* self, void** searchButton, void** edit)
{
    if (!self || !searchButton || !edit)
        return false;
    __try {
        *searchButton = *reinterpret_cast<void**>(
            static_cast<std::uint8_t*>(self) + 0xB8);
        *edit = *reinterpret_cast<void**>(
            static_cast<std::uint8_t*>(self) + 0xB4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *searchButton = nullptr;
        *edit = nullptr;
        return false;
    }
}

void ShowRejected(QueryDisposition disposition)
{
    const wchar_t* text = disposition == QueryDisposition::TooLong
        ? L"\u4E2D\u6587\u641C\u7D22\u8BCD\u8FC7\u957F\u3002\u6700\u591A"
          L"\u53EF\u8F93\u5165 36 \u4E2A UTF-16 \u5B57\u7B26\uFF1B"
          L"\u751F\u50FB\u5B57\u53EF\u80FD\u5360 2 \u4E2A\u5B57\u7B26\u3002"
        : L"\u641C\u7D22\u8BCD\u5305\u542B\u65E0\u6548\u5B57\u7B26\uFF0C"
          L"\u8BF7\u91CD\u65B0\u8F93\u5165\u3002";
    MessageBoxW(
        GetActiveWindow(), text,
        L"\u5546\u57CE\u641C\u7D22",
        MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}

void LogRejected(QueryDisposition disposition, std::size_t units)
{
    if (!g_api || !g_api->Log)
        return;
    g_api->Log(
        g_mod, SDGO_LOG_WARN,
        "shop search rejected %s query (%u UTF-16 units); query text was not "
        "logged",
        disposition == QueryDisposition::TooLong ? "overlong" : "invalid",
        static_cast<unsigned>(units));
}

void __fastcall Search(
    void* self, void*, std::uint32_t eventArgument, void* clickedControl)
{
    if (SearchEnabled() && self) {
        void* searchButton = nullptr;
        void* edit = nullptr;
        if (ReadControls(self, &searchButton, &edit) &&
            clickedControl == searchButton && edit && g_editText) {
            std::array<char16_t, 4097> query = {};
            std::size_t querySize = 0;
            const auto read = ReadNativeString(
                g_editText(edit), query.data(), query.size(), &querySize);
            if (read != NativeReadDisposition::Ok) {
                const auto disposition =
                    read == NativeReadDisposition::TooLong
                    ? QueryDisposition::TooLong
                    : QueryDisposition::InvalidUtf16;
                LogRejected(disposition, querySize);
                ShowRejected(disposition);
                return;
            }
            {
                const auto prepared = PrepareQuery(
                    std::u16string_view(query.data(), querySize));
                if (prepared.disposition == QueryDisposition::TooLong ||
                    prepared.disposition == QueryDisposition::InvalidUtf16) {
                    LogRejected(prepared.disposition, querySize);
                    ShowRejected(prepared.disposition);
                    return;
                }
            }
        }
    }
    g_search(self, eventArgument, clickedControl);
}

const char* __cdecl WideToCp(UINT codePage, const wchar_t* text)
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const bool keywordCall =
        caller == g_submitConvertReturn || caller == g_pageConvertReturn;
    if (!SearchEnabled() || codePage != 874 || !keywordCall)
        return g_wideToCp(codePage, text);

    std::array<char16_t, 4097> query = {};
    std::size_t querySize = 0;
    const auto read =
        ReadTerminatedString(text, query.data(), query.size(), &querySize);
    if (read != NativeReadDisposition::Ok) {
        LogRejected(
            read == NativeReadDisposition::TooLong
                ? QueryDisposition::TooLong
                : QueryDisposition::InvalidUtf16,
            querySize);
        return "";
    }
    const auto prepared = PrepareQuery(
        std::u16string_view(query.data(), querySize));
    if (prepared.disposition == QueryDisposition::TooLong ||
        prepared.disposition == QueryDisposition::InvalidUtf16) {
        LogRejected(prepared.disposition, querySize);
        return "";
    }
    if (prepared.disposition != QueryDisposition::Encoded)
        return g_wideToCp(codePage, text);

    std::memcpy(
        g_encodedKeyword.data(), prepared.bytes.data(), prepared.bytes.size());
    g_encodedKeyword[prepared.bytes.size()] = '\0';
    return g_encodedKeyword.data();
}

void* __fastcall ResourceToString(void* self, void*, void* output)
{
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    void* result = g_resourceToString(self, output);
    const bool allCaller =
        caller == g_rankAllReturn || caller == g_paymentAllReturn;
    if (!InterfaceEnabled() || !allCaller || !result)
        return result;
    return g_nativeAssign(
        result, L"\u5168\u90E8", 2);
}
} // namespace

namespace sdgo::shop_localization {

bool BindSearchHooks(
    const SdgoApi* api, SdgoMod* mod, const bool* searchEnabled,
    const bool* interfaceEnabled)
{
    if (!api || !api->Symbol || !api->HookCode || !searchEnabled ||
        !interfaceEnabled)
        return false;
    const auto submit =
        api->Symbol(mod, "shop_localization.search.submit");
    const auto editText =
        api->Symbol(mod, "shop_localization.search.edit_text");
    const auto wideToCp =
        api->Symbol(mod, "shop_localization.search.wide_to_cp");
    const auto submitCall =
        api->Symbol(mod, "shop_localization.search.call.submit_convert");
    const auto pageCall =
        api->Symbol(mod, "shop_localization.search.call.page_convert");
    const auto resourceToString =
        api->Symbol(mod, "shop_localization.search.resource_to_string");
    const auto nativeAssign =
        api->Symbol(mod, "shop_localization.assign");
    const auto rankAllCall =
        api->Symbol(mod, "shop_localization.search.call.rank_all");
    const auto paymentAllCall =
        api->Symbol(mod, "shop_localization.search.call.payment_all");
    if (!submit || !editText || !wideToCp || !submitCall || !pageCall ||
        !resourceToString || !nativeAssign || !rankAllCall || !paymentAllCall)
        return false;

    g_api = api;
    g_mod = mod;
    g_searchEnabled = searchEnabled;
    g_interfaceEnabled = interfaceEnabled;
    g_editText = reinterpret_cast<EditTextFn>(editText);
    g_nativeAssign = reinterpret_cast<NativeAssignFn>(nativeAssign);
    g_submitConvertReturn =
        reinterpret_cast<std::uintptr_t>(submitCall) + 5;
    g_pageConvertReturn =
        reinterpret_cast<std::uintptr_t>(pageCall) + 5;
    g_rankAllReturn =
        reinterpret_cast<std::uintptr_t>(rankAllCall) + 5;
    g_paymentAllReturn =
        reinterpret_cast<std::uintptr_t>(paymentAllCall) + 5;
    if (!api->HookCode(
            mod, "shop_localization.search.submit",
            reinterpret_cast<void*>(&Search),
            reinterpret_cast<void**>(&g_search)) ||
        !api->HookCode(
            mod, "shop_localization.search.wide_to_cp",
            reinterpret_cast<void*>(&WideToCp),
            reinterpret_cast<void**>(&g_wideToCp)) ||
        !api->HookCode(
            mod, "shop_localization.search.resource_to_string",
            reinterpret_cast<void*>(&ResourceToString),
            reinterpret_cast<void**>(&g_resourceToString)))
        return false;
    if (api->Log) {
        api->Log(
            mod, SDGO_LOG_INFO,
            "Chinese shop search bridge ready: native category/rank/payment "
            "fields preserved; keyword and All-label conversions scoped");
    }
    return true;
}

} // namespace sdgo::shop_localization
