#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

HWND WINAPI ShopSearchTestGetActiveWindow();
int WINAPI ShopSearchTestMessageBoxW(
    HWND, const wchar_t*, const wchar_t*, UINT);

#define GetActiveWindow ShopSearchTestGetActiveWindow
#define MessageBoxW ShopSearchTestMessageBoxW
#include "../mods/shop_localization/shop_search.cpp"
#undef MessageBoxW
#undef GetActiveWindow

namespace fixture {

static_assert(sizeof(void*) == 4, "shop search ABI fixture requires x86");

struct NativeString {
    union {
        wchar_t inlineText[8];
        const wchar_t* pointer;
    };
    std::uint32_t size;
    std::uint32_t capacity;
};
static_assert(sizeof(NativeString) == 24);

struct Edit {
    NativeString text;
};

struct Shop {
    std::uint8_t padding[0xB4] = {};
    Edit* edit = nullptr;
    void* searchButton = nullptr;
    std::uint32_t unknown = 0x11223344;
    std::uint16_t category = 0x3344;
    std::uint32_t pageStart = 5;
    std::uint32_t pageSize = 8;
    std::uint16_t rank = 3;
    std::uint16_t subfilter = 0x5566;
    std::uint16_t payment = 2;
};
static_assert(offsetof(Shop, edit) == 0xB4);
static_assert(offsetof(Shop, searchButton) == 0xB8);

#pragma pack(push, 1)
struct Packet {
    std::uint16_t opcode;
    std::uint32_t unknown;
    std::uint16_t category;
    std::uint32_t pageStart;
    std::uint32_t pageSize;
    std::uint16_t rank;
    std::uint16_t subfilter;
    std::uint16_t payment;
    char keyword[101];
};

struct PacketEnvelope {
    std::uint32_t before;
    Packet packet;
    std::uint32_t after;
};
#pragma pack(pop)

static_assert(sizeof(Packet) == 123);

using WideCaller = const char*(__cdecl*)(UINT, const wchar_t*);
using ResourceCaller = void*(__cdecl*)(void*, void*);
using SearchCaller =
    void(__cdecl*)(void*, std::uint32_t, void*);

struct Code {
    std::uint8_t* allocation = nullptr;
    std::uint8_t* callSite = nullptr;
};

Code g_submitConvert;
Code g_pageConvert;
Code g_outsideConvert;
Code g_rankAll;
Code g_paymentAll;
Code g_outsideResource;
Code g_searchCall;
WideCaller g_submitCaller = nullptr;
WideCaller g_pageCaller = nullptr;
WideCaller g_outsideCaller = nullptr;
ResourceCaller g_rankAllCaller = nullptr;
ResourceCaller g_paymentAllCaller = nullptr;
ResourceCaller g_outsideResourceCaller = nullptr;
SearchCaller g_searchCaller = nullptr;

bool g_missingSymbol = false;
bool s_searchEnabled = true;
bool s_interfaceEnabled = true;
unsigned g_hookCount = 0;
unsigned g_nativeSearchCalls = 0;
unsigned g_nativeWideCalls = 0;
unsigned g_nativeResourceCalls = 0;
unsigned g_nativeAssignCalls = 0;
unsigned g_dialogCount = 0;
std::vector<std::string> g_logs;
std::wstring g_savedQuery;
PacketEnvelope g_submitPacket = {};
PacketEnvelope g_pagePacket = {};

void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
    std::printf("PASS %s\n", message);
}

std::uint8_t* AllocateCode()
{
    auto* result = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (!result)
        throw std::runtime_error("executable fixture allocation failed");
    return result;
}

void SetCall(const Code& code, void* target)
{
    const auto relative = static_cast<std::int32_t>(
        reinterpret_cast<std::uintptr_t>(target) -
        reinterpret_cast<std::uintptr_t>(code.callSite + 5));
    std::memcpy(code.callSite + 1, &relative, sizeof(relative));
    FlushInstructionCache(
        GetCurrentProcess(), code.allocation, 64);
}

Code MakeWideCaller(WideCaller* caller)
{
    auto* code = AllocateCode();
    std::size_t at = 0;
    const std::uint8_t loadText[] = {0x8B, 0x44, 0x24, 0x08};
    std::memcpy(code + at, loadText, sizeof(loadText));
    at += sizeof(loadText);
    code[at++] = 0x50;
    const std::uint8_t loadCodePage[] = {0x8B, 0x44, 0x24, 0x08};
    std::memcpy(code + at, loadCodePage, sizeof(loadCodePage));
    at += sizeof(loadCodePage);
    code[at++] = 0x50;
    Code result{code, code + at};
    code[at++] = 0xE8;
    at += 4;
    const std::uint8_t cleanAndReturn[] = {0x83, 0xC4, 0x08, 0xC3};
    std::memcpy(code + at, cleanAndReturn, sizeof(cleanAndReturn));
    *caller = reinterpret_cast<WideCaller>(code);
    return result;
}

Code MakeResourceCaller(ResourceCaller* caller)
{
    auto* code = AllocateCode();
    std::size_t at = 0;
    const std::uint8_t loadSelf[] = {0x8B, 0x4C, 0x24, 0x04};
    std::memcpy(code + at, loadSelf, sizeof(loadSelf));
    at += sizeof(loadSelf);
    const std::uint8_t pushOutput[] = {0xFF, 0x74, 0x24, 0x08};
    std::memcpy(code + at, pushOutput, sizeof(pushOutput));
    at += sizeof(pushOutput);
    Code result{code, code + at};
    code[at++] = 0xE8;
    at += 4;
    code[at++] = 0xC3;
    *caller = reinterpret_cast<ResourceCaller>(code);
    return result;
}

Code MakeSearchCaller(SearchCaller* caller)
{
    auto* code = AllocateCode();
    std::size_t at = 0;
    const std::uint8_t loadSelf[] = {0x8B, 0x4C, 0x24, 0x04};
    std::memcpy(code + at, loadSelf, sizeof(loadSelf));
    at += sizeof(loadSelf);
    const std::uint8_t loadClicked[] = {0x8B, 0x44, 0x24, 0x0C};
    std::memcpy(code + at, loadClicked, sizeof(loadClicked));
    at += sizeof(loadClicked);
    code[at++] = 0x50;
    const std::uint8_t loadEvent[] = {0x8B, 0x44, 0x24, 0x0C};
    std::memcpy(code + at, loadEvent, sizeof(loadEvent));
    at += sizeof(loadEvent);
    code[at++] = 0x50;
    Code result{code, code + at};
    code[at++] = 0xE8;
    at += 4;
    code[at++] = 0xC3;
    *caller = reinterpret_cast<SearchCaller>(code);
    return result;
}

void PointString(
    NativeString* output, const std::wstring& storage)
{
    std::memset(output, 0, sizeof(*output));
    output->size = static_cast<std::uint32_t>(storage.size());
    if (storage.size() < 8) {
        std::memcpy(
            output->inlineText, storage.data(),
            storage.size() * sizeof(wchar_t));
        output->capacity = 7;
    } else {
        output->pointer = storage.data();
        output->capacity = output->size;
    }
}

const wchar_t* Text(const NativeString& value)
{
    return value.capacity < 8 ? value.inlineText : value.pointer;
}

std::wstring TextCopy(const NativeString& value)
{
    return std::wstring(Text(value), value.size);
}

void* __fastcall NativeAssign(
    void* self, void*, const wchar_t* text, std::uint32_t length)
{
    ++g_nativeAssignCalls;
    auto* output = static_cast<NativeString*>(self);
    if (!output || !text || length > 7)
        return nullptr;
    std::memset(output, 0, sizeof(*output));
    std::memcpy(output->inlineText, text, length * sizeof(wchar_t));
    output->inlineText[length] = 0;
    output->size = length;
    output->capacity = 7;
    return output;
}

void* __fastcall NativeEditText(void* self, void*)
{
    return &static_cast<Edit*>(self)->text;
}

const char* __cdecl NativeWideToCp(
    UINT codePage, const wchar_t* text)
{
    ++g_nativeWideCalls;
    if (codePage != 874)
        return "native-other-code-page";
    if (text && std::wcscmp(text, L"GUNDAM") == 0)
        return "native-English";
    if (text && std::wcscmp(
                    text, L"\u0E01\u0E31\u0E19\u0E14\u0E31\u0E21") == 0)
        return "native-Thai";
    return "native-CP874";
}

void* __fastcall NativeResourceToString(
    void* self, void*, void* output)
{
    ++g_nativeResourceCalls;
    const auto* source = static_cast<const NativeString*>(self);
    return NativeAssign(
        output, nullptr, Text(*source), source->size);
}

void ConstructPacket(
    PacketEnvelope* envelope, const Shop& shop,
    std::uint32_t pageStart, const char* keyword)
{
    envelope->before = 0xA1B2C3D4;
    envelope->after = 0xD4C3B2A1;
    auto& packet = envelope->packet;
    std::memset(&packet, 0, sizeof(packet));
    packet.opcode = 0x72D;
    packet.unknown = shop.unknown;
    packet.category = shop.category;
    packet.pageStart = pageStart;
    packet.pageSize = shop.pageSize;
    packet.rank = shop.rank;
    packet.subfilter = shop.subfilter;
    packet.payment = shop.payment;
    if (keyword) {
        const auto length = (std::min)(
            std::strlen(keyword), std::size_t{100});
        std::memcpy(packet.keyword, keyword, length);
    }
}

void __fastcall NativeSearch(
    void* self, void*, std::uint32_t, void*)
{
    ++g_nativeSearchCalls;
    auto& shop = *static_cast<Shop*>(self);
    const auto& value = shop.edit->text;
    g_savedQuery.assign(Text(value), value.size);
    const char* converted =
        g_submitCaller(874, g_savedQuery.c_str());
    ConstructPacket(
        &g_submitPacket, shop, shop.pageStart, converted);
}

void SendPage(Shop* shop, std::uint32_t pageStart)
{
    const char* converted =
        g_pageCaller(874, g_savedQuery.c_str());
    ConstructPacket(&g_pagePacket, *shop, pageStart, converted);
}

void* Symbol(SdgoMod*, const char* name)
{
    if (g_missingSymbol &&
        std::strcmp(
            name,
            "shop_localization.search.call.payment_all") == 0)
        return nullptr;
    if (std::strcmp(
            name, "shop_localization.search.submit") == 0)
        return reinterpret_cast<void*>(&NativeSearch);
    if (std::strcmp(
            name, "shop_localization.search.edit_text") == 0)
        return reinterpret_cast<void*>(&NativeEditText);
    if (std::strcmp(
            name, "shop_localization.search.wide_to_cp") == 0)
        return reinterpret_cast<void*>(&NativeWideToCp);
    if (std::strcmp(
            name,
            "shop_localization.search.call.submit_convert") == 0)
        return g_submitConvert.callSite;
    if (std::strcmp(
            name,
            "shop_localization.search.call.page_convert") == 0)
        return g_pageConvert.callSite;
    if (std::strcmp(
            name,
            "shop_localization.search.resource_to_string") == 0)
        return reinterpret_cast<void*>(&NativeResourceToString);
    if (std::strcmp(name, "shop_localization.assign") == 0)
        return reinterpret_cast<void*>(&NativeAssign);
    if (std::strcmp(
            name, "shop_localization.search.call.rank_all") == 0)
        return g_rankAll.callSite;
    if (std::strcmp(
            name, "shop_localization.search.call.payment_all") == 0)
        return g_paymentAll.callSite;
    return nullptr;
}

int HookCode(
    SdgoMod*, const char* name, void* detour, void** original)
{
    if (std::strcmp(
            name, "shop_localization.search.submit") == 0) {
        *original = reinterpret_cast<void*>(&NativeSearch);
        SetCall(g_searchCall, detour);
    } else if (std::strcmp(
                   name,
                   "shop_localization.search.wide_to_cp") == 0) {
        *original = reinterpret_cast<void*>(&NativeWideToCp);
        SetCall(g_submitConvert, detour);
        SetCall(g_pageConvert, detour);
        SetCall(g_outsideConvert, detour);
    } else if (std::strcmp(
                   name,
                   "shop_localization.search.resource_to_string") == 0) {
        *original =
            reinterpret_cast<void*>(&NativeResourceToString);
        SetCall(g_rankAll, detour);
        SetCall(g_paymentAll, detour);
        SetCall(g_outsideResource, detour);
    } else {
        return 0;
    }
    ++g_hookCount;
    return 1;
}

void __cdecl Log(SdgoMod*, int, const char* format, ...)
{
    std::array<char, 512> buffer = {};
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(
        buffer.data(), buffer.size(), format, arguments);
    va_end(arguments);
    g_logs.emplace_back(buffer.data());
}

void PrepareCode()
{
    g_submitConvert = MakeWideCaller(&g_submitCaller);
    g_pageConvert = MakeWideCaller(&g_pageCaller);
    g_outsideConvert = MakeWideCaller(&g_outsideCaller);
    g_rankAll = MakeResourceCaller(&g_rankAllCaller);
    g_paymentAll =
        MakeResourceCaller(&g_paymentAllCaller);
    g_outsideResource =
        MakeResourceCaller(&g_outsideResourceCaller);
    g_searchCall = MakeSearchCaller(&g_searchCaller);
    SetCall(
        g_submitConvert,
        reinterpret_cast<void*>(&NativeWideToCp));
    SetCall(
        g_pageConvert,
        reinterpret_cast<void*>(&NativeWideToCp));
    SetCall(
        g_outsideConvert,
        reinterpret_cast<void*>(&NativeWideToCp));
    SetCall(
        g_rankAll,
        reinterpret_cast<void*>(&NativeResourceToString));
    SetCall(
        g_paymentAll,
        reinterpret_cast<void*>(&NativeResourceToString));
    SetCall(
        g_outsideResource,
        reinterpret_cast<void*>(&NativeResourceToString));
    SetCall(
        g_searchCall, reinterpret_cast<void*>(&NativeSearch));
}

void CheckPacketFields(
    const PacketEnvelope& envelope, const Shop& shop,
    std::uint32_t pageStart, const char* context)
{
    Check(
        envelope.before == 0xA1B2C3D4 &&
            envelope.after == 0xD4C3B2A1,
        context);
    const auto& packet = envelope.packet;
    Check(
        packet.opcode == 0x72D &&
            packet.unknown == shop.unknown &&
            packet.category == shop.category &&
            packet.pageStart == pageStart &&
            packet.pageSize == shop.pageSize &&
            packet.rank == shop.rank &&
            packet.subfilter == shop.subfilter &&
            packet.payment == shop.payment,
        "keyword bridge preserves packet numeric fields");
}

bool LogContains(std::string_view needle)
{
    return std::any_of(
        g_logs.begin(), g_logs.end(),
        [needle](const std::string& value) {
            return value.find(needle) != std::string::npos;
        });
}

} // namespace fixture

HWND WINAPI ShopSearchTestGetActiveWindow()
{
    return nullptr;
}

int WINAPI ShopSearchTestMessageBoxW(
    HWND, const wchar_t*, const wchar_t*, UINT)
{
    ++fixture::g_dialogCount;
    return IDOK;
}

int main()
{
    using namespace fixture;
    try {
        PrepareCode();

        SdgoApi api = {};
        api.size = sizeof(api);
        api.version = SDGO_API_VERSION;
        api.Symbol = &Symbol;
        api.HookCode = &HookCode;
        api.Log = &Log;

        Check(
            !sdgo::shop_localization::BindSearchHooks(
                nullptr, nullptr, &s_searchEnabled,
                &s_interfaceEnabled),
            "null API is rejected before hooks");
        SdgoApi invalid = api;
        invalid.HookCode = nullptr;
        Check(
            !sdgo::shop_localization::BindSearchHooks(
                &invalid, nullptr, &s_searchEnabled,
                &s_interfaceEnabled) &&
                g_hookCount == 0,
            "invalid API is rejected before hooks");

        g_missingSymbol = true;
        Check(
            !sdgo::shop_localization::BindSearchHooks(
                &api, nullptr, &s_searchEnabled,
                &s_interfaceEnabled) &&
                g_hookCount == 0,
            "missing symbol is rejected before hooks");
        g_missingSymbol = false;
        Check(
            sdgo::shop_localization::BindSearchHooks(
                &api, nullptr, &s_searchEnabled,
                &s_interfaceEnabled) &&
                g_hookCount == 3,
            "submit, converter and scoped All hooks bind");

        Edit edit = {};
        Shop shop = {};
        shop.edit = &edit;
        shop.searchButton =
            reinterpret_cast<void*>(0x12345678);

        std::wstring query = L"GUNDAM";
        PointString(&edit.text, query);
        g_searchCaller(&shop, 0xABC, shop.searchButton);
        Check(
            g_nativeSearchCalls == 1 &&
                g_nativeWideCalls == 1 &&
                std::strcmp(
                    g_submitPacket.packet.keyword,
                    "native-English") == 0,
            "English uses the original CP874 converter");
        CheckPacketFields(
            g_submitPacket, shop, shop.pageStart,
            "submit packet canaries remain intact");

        query = L"\u0E01\u0E31\u0E19\u0E14\u0E31\u0E21";
        PointString(&edit.text, query);
        g_searchCaller(&shop, 0xABC, shop.searchButton);
        Check(
            g_nativeWideCalls == 2 &&
                std::strcmp(
                    g_submitPacket.packet.keyword,
                    "native-Thai") == 0,
            "Thai remains on the original CP874 path");

        query.assign(36, L'\u673A');
        PointString(&edit.text, query);
        const auto expected =
            sdgo::shop_search::PrepareQuery(
                std::u16string_view(
                    reinterpret_cast<const char16_t*>(
                        query.data()),
                    query.size()));
        const auto wideBeforeChinese = g_nativeWideCalls;
        g_searchCaller(&shop, 0xABC, shop.searchButton);
        Check(
            expected.disposition ==
                    sdgo::shop_search::QueryDisposition::Encoded &&
                expected.bytes.size() == 100 &&
                g_nativeWideCalls == wideBeforeChinese &&
                std::memcmp(
                    g_submitPacket.packet.keyword,
                    expected.bytes.data(), 100) == 0 &&
                g_submitPacket.packet.keyword[100] == '\0',
            "36-unit Chinese query produces 100 bytes plus NUL");
        CheckPacketFields(
            g_submitPacket, shop, shop.pageStart,
            "maximum submit packet canaries remain intact");

        SendPage(&shop, 16);
        Check(
            std::memcmp(
                g_pagePacket.packet.keyword,
                expected.bytes.data(), 100) == 0 &&
                g_pagePacket.packet.keyword[100] == '\0',
            "pagination uses the same scoped Chinese encoding");
        CheckPacketFields(
            g_pagePacket, shop, 16,
            "page packet canaries remain intact");

        const auto outsideBefore = g_nativeWideCalls;
        Check(
            std::strcmp(
                g_outsideCaller(874, query.c_str()),
                "native-CP874") == 0 &&
                g_nativeWideCalls == outsideBefore + 1,
            "Chinese outside verified callers bypasses bridge");
        Check(
            std::strcmp(
                g_submitCaller(936, query.c_str()),
                "native-other-code-page") == 0,
            "non-CP874 call at submit site bypasses bridge");

        s_searchEnabled = false;
        Check(
            std::strcmp(
                g_pageCaller(874, query.c_str()),
                "native-CP874") == 0,
            "disabled search bridge preserves native conversion");
        s_searchEnabled = true;

        NativeString all = {};
        NativeAssign(&all, nullptr, L"All", 3);
        NativeString output = {};
        void* returned = g_rankAllCaller(&all, &output);
        Check(
            returned == &output &&
                TextCopy(output) == L"\u5168\u90E8",
            "rank All is replaced through the native out parameter");
        returned = g_paymentAllCaller(&all, &output);
        Check(
            returned == &output &&
                TextCopy(output) == L"\u5168\u90E8",
            "payment All is replaced through the native out parameter");
        returned = g_outsideResourceCaller(&all, &output);
        Check(
            returned == &output && TextCopy(output) == L"All",
            "All outside two shop callers remains native");

        s_interfaceEnabled = false;
        g_rankAllCaller(&all, &output);
        Check(
            TextCopy(output) == L"All",
            "interface switch disables only scoped All replacement");
        s_searchEnabled = false;
        s_interfaceEnabled = true;
        g_rankAllCaller(&all, &output);
        Check(
            TextCopy(output) == L"\u5168\u90E8",
            "All replacement is independent from name search switch");
        s_searchEnabled = true;

        query.assign(37, L'\u673A');
        PointString(&edit.text, query);
        const auto searchBeforeReject = g_nativeSearchCalls;
        const auto wideBeforeReject = g_nativeWideCalls;
        const auto dialogsBeforeReject = g_dialogCount;
        g_searchCaller(&shop, 0xABC, shop.searchButton);
        Check(
            g_nativeSearchCalls == searchBeforeReject &&
                g_nativeWideCalls == wideBeforeReject &&
                g_dialogCount == dialogsBeforeReject + 1 &&
                LogContains("37 UTF-16 units"),
            "overlong Chinese is rejected before CP874 fallback");
        Check(
            g_submitCaller(874, query.c_str())[0] == '\0' &&
                g_nativeWideCalls == wideBeforeReject,
            "converter defense returns empty instead of question marks");

        query.assign(4097, L'\u673A');
        PointString(&edit.text, query);
        const auto dialogsBeforeHuge = g_dialogCount;
        g_searchCaller(&shop, 0xABC, shop.searchButton);
        Check(
            g_nativeSearchCalls == searchBeforeReject &&
                g_dialogCount == dialogsBeforeHuge + 1 &&
                LogContains("4097 UTF-16 units"),
            "native query over read bound is rejected");

        std::memset(&edit.text, 0, sizeof(edit.text));
        edit.text.size = 5;
        edit.text.capacity = 4;
        const auto dialogsBeforeInvalid = g_dialogCount;
        g_searchCaller(&shop, 0xABC, shop.searchButton);
        Check(
            g_nativeSearchCalls == searchBeforeReject &&
                g_dialogCount == dialogsBeforeInvalid + 1 &&
                LogContains("invalid query"),
            "invalid native string layout is rejected");

        Check(
            std::all_of(
                g_logs.begin(), g_logs.end(),
                [](const std::string& value) {
                    return value.find("query text was not logged") !=
                        std::string::npos ||
                        value.find("bridge ready") !=
                        std::string::npos;
                }),
            "logs contain status and length only");
        std::puts("SHOP SEARCH DLL PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
