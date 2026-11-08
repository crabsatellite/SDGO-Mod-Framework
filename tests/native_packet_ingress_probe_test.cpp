#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <xmmintrin.h>

#include "../mods/game_test_client/native_packet_ingress_probe.h"

namespace
{

using IngressFn = int(__cdecl*)(
    std::uint32_t,
    std::uint32_t,
    std::uint32_t,
    void*,
    std::uint32_t);

constexpr std::uint16_t kEntryControlWord = 0x077F;
constexpr std::uint16_t kLogControlWord = 0x0B7F;
constexpr std::uint16_t kPostControlWord = 0x0F7F;
constexpr std::uint32_t kEntryMxcsr = 0x3F80;
constexpr std::uint32_t kLogMxcsr = 0x5F80;
constexpr std::uint32_t kPostMxcsr = 0x7F80;
constexpr DWORD kEntryLastError = 0x11223344;
constexpr DWORD kLogLastError = 0x55667788;
constexpr DWORD kPostLastError = 0x99AABBCC;

struct ExpectedCall
{
    std::uint32_t arg0;
    std::uint32_t arg1;
    std::uint32_t sourceHandle;
    void* packet;
    std::uint32_t size;
    int result;
};

IngressFn g_captured = nullptr;
ExpectedCall g_expected = {};
SdgoMod* g_mod = reinterpret_cast<SdgoMod*>(1);
std::vector<std::string> g_logs;
unsigned g_checks = 0;
unsigned g_failures = 0;
unsigned g_originalCalls = 0;

std::uint16_t ReadControlWord()
{
    std::uint16_t value = 0;
    __asm fnstcw value
    return value;
}

void WriteControlWord(std::uint16_t value)
{
    __asm fldcw value
}

void Check(bool condition, const char* reason)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", reason);
    }
}

bool Contains(const std::string& text, const char* expected)
{
    return text.find(expected) != std::string::npos;
}

int __cdecl FakeOriginal(
    std::uint32_t arg0,
    std::uint32_t arg1,
    std::uint32_t sourceHandle,
    void* packet,
    std::uint32_t size)
{
    const DWORD entryLastError = GetLastError();
    const std::uint16_t entryControlWord = ReadControlWord();
    const std::uint32_t entryMxcsr = _mm_getcsr();
    ++g_originalCalls;

    Check(arg0 == g_expected.arg0, "arg0 reaches the original unchanged");
    Check(arg1 == g_expected.arg1, "arg1 reaches the original unchanged");
    Check(
        sourceHandle == g_expected.sourceHandle,
        "source handle reaches the original unchanged");
    Check(
        packet == g_expected.packet,
        "packet pointer reaches the original unchanged");
    Check(size == g_expected.size, "native size reaches the original unchanged");
    Check(
        entryLastError == kEntryLastError,
        "logging restores the native entry LastError before the original");
    Check(
        entryControlWord == kEntryControlWord,
        "logging restores the native x87 control word before the original");
    Check(
        entryMxcsr == kEntryMxcsr,
        "logging restores the native MXCSR before the original");

    SetLastError(kPostLastError);
    WriteControlWord(kPostControlWord);
    _mm_setcsr(kPostMxcsr);
    return g_expected.result;
}

void FakeLog(SdgoMod* mod, int level, const char* format, ...)
{
    Check(mod == g_mod, "log retains the installed mod identity");
    Check(level == SDGO_LOG_INFO, "selected packets use the info log level");

    char text[2048] = {};
    va_list arguments;
    va_start(arguments, format);
    const int length =
        std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    Check(
        length >= 0 && static_cast<std::size_t>(length) < sizeof(text),
        "formatted packet observation fits the test capture");
    g_logs.emplace_back(text);

    // Instrumentation is allowed to disturb all three states internally. The
    // bridge must restore the native entry state before tail-jumping.
    SetLastError(kLogLastError);
    WriteControlWord(kLogControlWord);
    _mm_setcsr(kLogMxcsr);
}

int FakeHookCode(
    SdgoMod* mod,
    const char* name,
    void* detour,
    void** original)
{
    Check(mod == g_mod, "hook retains the installed mod identity");
    Check(
        name && std::strcmp(name, "test.native_packet_ingress") == 0,
        "hook uses the verified ingress symbol");
    Check(detour != nullptr, "hook supplies an ingress detour");
    Check(original != nullptr, "hook supplies original storage");
    if (!detour || !original)
        return 0;

    g_captured = reinterpret_cast<IngressFn>(detour);
    const IngressFn fixture = &FakeOriginal;
    static_assert(
        sizeof(fixture) == sizeof(void*),
        "this ABI test must be compiled for x86");
    std::memcpy(original, &fixture, sizeof(fixture));
    return 1;
}

void Invoke(
    std::uint32_t arg0,
    std::uint32_t arg1,
    std::uint32_t sourceHandle,
    void* packet,
    std::uint32_t size,
    int expectedResult)
{
    g_expected = {
        arg0,
        arg1,
        sourceHandle,
        packet,
        size,
        expectedResult};
    const unsigned callsBefore = g_originalCalls;

    SetLastError(kEntryLastError);
    WriteControlWord(kEntryControlWord);
    _mm_setcsr(kEntryMxcsr);
    std::uintptr_t stackBefore = 0;
    std::uintptr_t stackAfter = 0;
    __asm mov stackBefore, esp
    const int result =
        g_captured(arg0, arg1, sourceHandle, packet, size);
    __asm mov stackAfter, esp
    const DWORD postLastError = GetLastError();
    const std::uint16_t postControlWord = ReadControlWord();
    const std::uint32_t postMxcsr = _mm_getcsr();

    Check(
        g_originalCalls == callsBefore + 1,
        "the original ingress runs exactly once");
    Check(result == expectedResult, "the original return value is unchanged");
    Check(stackBefore == stackAfter, "the five-argument cdecl stack is balanced");
    Check(
        postLastError == kPostLastError,
        "the original post-call LastError reaches its caller");
    Check(
        postControlWord == kPostControlWord,
        "the original post-call x87 state reaches its caller");
    Check(
        postMxcsr == kPostMxcsr,
        "the original post-call MXCSR reaches its caller");
}

} // namespace

int main()
{
    const DWORD savedLastError = GetLastError();
    const std::uint16_t savedControlWord = ReadControlWord();
    const std::uint32_t savedMxcsr = _mm_getcsr();

    SdgoApi api = {};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Log = &FakeLog;
    api.HookCode = &FakeHookCode;
    NativePacketIngressProbe probe;
    Check(probe.Install(&api, g_mod), "native ingress probe installs");
    Check(g_captured != nullptr, "HookCode captures the five-argument detour");

    SYSTEM_INFO system = {};
    GetSystemInfo(&system);
    auto* guarded = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr,
        static_cast<std::size_t>(system.dwPageSize) * 2,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE));
    Check(guarded != nullptr, "guarded two-page packet allocation succeeds");
    DWORD previousProtection = 0;
    const bool protectedTail =
        guarded &&
        VirtualProtect(
            guarded + system.dwPageSize,
            system.dwPageSize,
            PAGE_NOACCESS,
            &previousProtection) != 0;
    Check(protectedTail, "page following the two-byte packet is inaccessible");

    if (g_captured && protectedTail) {
        auto* shortPacket = guarded + system.dwPageSize - 2;
        const std::uint16_t opcode = 0x0880;
        std::memcpy(shortPacket, &opcode, sizeof(opcode));
        Invoke(
            0x10203040,
            0x50607080,
            0x90A0B0C0,
            shortPacket,
            2,
            0x51A70001);

        Check(g_logs.size() == 1, "the two-byte 0880 is observed once");
        if (!g_logs.empty()) {
            Check(
                Contains(g_logs[0], "sequence=1 opcode=0880 size=2"),
                "the short observation retains sequence, opcode, and native size");
            Check(
                Contains(
                    g_logs[0],
                    "copied_bytes=2 availability_mask=03"),
                "the short observation marks exactly two available bytes");
            Check(
                Contains(
                    g_logs[0],
                    "bytes=80/08/00/00/00/00/00/00"),
                "the short observation zero-fills only unavailable placeholders");
            Check(
                Contains(
                    g_logs[0],
                    "recipient32=unavailable error16_raw=unavailable"),
                "short data never claims recipient or raw16 validity");
        }

        std::uint8_t fullPacket[8] = {};
        const std::uint32_t recipient = 0x44332211;
        const std::uint16_t raw16 = 0x6655;
        std::memcpy(fullPacket, &opcode, sizeof(opcode));
        std::memcpy(fullPacket + 2, &recipient, sizeof(recipient));
        std::memcpy(fullPacket + 6, &raw16, sizeof(raw16));
        Invoke(
            0x01020304,
            0x11121314,
            0x21222324,
            fullPacket,
            sizeof(fullPacket),
            0x51A70002);

        Check(
            g_logs.size() == 2,
            "repeated 0880 invocations produce independent observations");
        if (g_logs.size() >= 2) {
            Check(
                Contains(g_logs[1], "sequence=2 opcode=0880 size=8"),
                "the repeated opcode receives its own sequence");
            Check(
                Contains(
                    g_logs[1],
                    "copied_bytes=8 availability_mask=FF"),
                "the complete prefix marks all eight bytes available");
            Check(
                Contains(
                    g_logs[1],
                    "bytes=80/08/11/22/33/44/55/66"),
                "the complete observation retains every native prefix byte");
            Check(
                Contains(
                    g_logs[1],
                    "recipient32=1144201745/0x44332211"),
                "the complete observation reports the real recipient");
            Check(
                Contains(g_logs[1], "error16_raw=26197/0x6655"),
                "the complete observation reports raw16 without reinterpreting it");
        }

        const std::uint16_t loadingOpcodes[] = {
            0x096F,
            0x087D,
            0x087E,
        };
        for (const std::uint16_t loadingOpcode : loadingOpcodes) {
            std::uint8_t loadingPacket[8] = {};
            loadingPacket[6] = 0x40;
            std::memcpy(
                loadingPacket,
                &loadingOpcode,
                sizeof(loadingOpcode));
            Invoke(
                0x61626364,
                0x71727374,
                0x81828384,
                loadingPacket,
                sizeof(loadingPacket),
                0x51A70010 + loadingOpcode);
        }
        Check(
            g_logs.size() == 5,
            "inventory count, page, and error replies are observed");
        if (g_logs.size() >= 5) {
            Check(
                Contains(
                    g_logs[2],
                    "sequence=3 opcode=096F size=8") &&
                Contains(
                    g_logs[2],
                    "bytes=6F/09/00/00/00/00/40/00"),
                "the inventory page reply retains its native count byte");
            Check(
                Contains(g_logs[3], "sequence=4 opcode=087D size=8"),
                "the inventory count reply is retained");
            Check(
                Contains(g_logs[4], "sequence=5 opcode=087E size=8"),
                "the inventory error reply is retained");
        }

        std::uint8_t ignoredPacket[2] = {};
        const std::uint16_t ignoredOpcode = 0x1234;
        std::memcpy(
            ignoredPacket,
            &ignoredOpcode,
            sizeof(ignoredOpcode));
        Invoke(
            0x31323334,
            0x41424344,
            0x51525354,
            ignoredPacket,
            sizeof(ignoredPacket),
            0x51A70003);
        Check(
            g_logs.size() == 5,
            "an untracked opcode is forwarded without a diagnostic record");
    }

    if (guarded)
        VirtualFree(guarded, 0, MEM_RELEASE);

    WriteControlWord(savedControlWord);
    _mm_setcsr(savedMxcsr);
    SetLastError(savedLastError);
    std::printf(
        "NATIVE PACKET INGRESS PROBE: %u check(s), %u failure(s)\n",
        g_checks,
        g_failures);
    return g_failures ? 1 : 0;
}
