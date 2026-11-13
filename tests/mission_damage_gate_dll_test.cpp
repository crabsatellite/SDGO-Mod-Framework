#ifdef NDEBUG
#undef NDEBUG
#endif

#include <windows.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "sdgo/map_boundary_contact.h"
#include "sdgo/mission_damage_gate_format.h"
#include "sdgo/sdgo_api.h"

namespace {
void* g_body = nullptr;
void* g_part = nullptr;
void* g_loss = nullptr;
std::wstring g_directory;
uint32_t g_broadcasts = 0;
uint32_t g_noticeAttacker = 0;
uint32_t g_noticeTarget = 0;
uint32_t g_noticeHealth = 0;

__declspec(naked) void ContinueNative() {
    __asm { mov eax, edx }
    __asm { ret }
}

__declspec(naked) std::uint32_t Invoke(void*, void*, std::uint32_t) {
    __asm { push ebp }
    __asm { mov eax, [esp + 8] }
    __asm { mov ebp, [esp + 12] }
    __asm { mov edx, [esp + 16] }
    __asm { call eax }
    __asm { pop ebp }
    __asm { ret }
}

int HookSite(SdgoMod*, const char* name, void* detour, void** original) {
    if (std::strcmp(name, "map_catalog_server.damage_gate_body") == 0) g_body = detour;
    else if (std::strcmp(name, "map_catalog_server.damage_gate_part") == 0) g_part = detour;
    else if (std::strcmp(name, "map_catalog_server.damage_gate_loss") == 0) g_loss = detour;
    else return 0;
    *original = reinterpret_cast<void*>(&ContinueNative);
    return 1;
}

const wchar_t* Directory(SdgoMod*) { return g_directory.c_str(); }
void Log(SdgoMod*, int, const char*, ...) {}

template<class T>
void Put(void* base, std::ptrdiff_t offset, T value) {
    std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(value));
}

template<class T>
T Get(const void* base, std::ptrdiff_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

int __fastcall Broadcast(void* room, void*, uint8_t* packet, uint32_t size,
                         uint32_t* recipients) {
    assert(room && packet && size == 13 && recipients);
    assert(Get<uint16_t>(packet, 0) == 0x0793);
    g_noticeAttacker = Get<uint32_t>(packet, 2);
    g_noticeTarget = sdgo::map_boundary_contact::NoticeTarget(Get<uint32_t>(packet, 6));
    g_noticeHealth = sdgo::map_boundary_contact::DecodeHealth(packet + 10);
    *recipients = 2;
    ++g_broadcasts;
    return 1;
}

void* Symbol(SdgoMod*, const char* name) {
    return name && std::strcmp(name, "battle.room_random_broadcast") == 0
        ? reinterpret_cast<void*>(&Broadcast) : nullptr;
}

void Put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

void WriteRules(const std::filesystem::path& path) {
    using namespace sdgo::mission_damage_gate;
    std::vector<std::uint8_t> bytes(kHeaderBytes + kRowBytes);
    std::memcpy(bytes.data(), "SDGODMG1", 8);
    Put32(bytes, 8, 1);
    Put32(bytes, 12, static_cast<std::uint32_t>(kHeaderBytes));
    Put32(bytes, 16, static_cast<std::uint32_t>(kRowBytes));
    Put32(bytes, 20, 1);
    Put32(bytes, 24, static_cast<std::uint32_t>(kRowBytes));
    Put32(bytes, 32, 88072);
    Put32(bytes, 36, 8);
    Put32(bytes, 40, 85303);
    Put32(bytes, 44, 97639);
    Put32(bytes, 48, 1);
    Put32(bytes, 28, Checksum(bytes.data() + kHeaderBytes, kRowBytes));
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    assert(stream.good());
}

void Mission(std::vector<std::uint8_t>& room) {
    Put<std::uint32_t>(room.data(), 0x21, 1);
    Put<std::uint8_t>(room.data(), 5, 5);
    Put<std::uint32_t>(room.data(), 0x1B, 88072);
    Put<std::uint32_t>(room.data(), 0x316A4, 2);
    Put<std::uint8_t>(room.data(), 0x316F0, 8);
    Put<std::uint32_t>(room.data(), 0x2EEB4, 12345);
}

void* Pilot(std::vector<std::uint8_t>& room, std::uint32_t slot,
            std::uint32_t entity, std::uint32_t* npcData, std::uint32_t health) {
    auto* pilot = room.data() + 0x3CE4 + slot * 0x1F54;
    Put<std::uint32_t>(pilot, 0, entity);
    Put<std::uint8_t>(pilot, 0x33C, 0);
    Put<std::uint32_t>(pilot, 0x340 + 0x34, health);
    Put<void*>(pilot, 0x1CEC, npcData);
    return pilot;
}

void Hit(void* hook, std::vector<std::uint8_t>& room, bool part,
         std::uint32_t slot, void* attacker, std::uint32_t expectedDamage,
         std::uint32_t expectedForce) {
    std::vector<std::uint8_t> storage(part ? 0x80 : 0x600);
    auto* frame = storage.data() + (part ? 0x30 : 0x500);
    Put<void*>(frame, part ? -0x14 : -0x480, room.data());
    Put<std::uint32_t>(frame, part ? 0x1C : 0x20, 777);
    Put<std::uint32_t>(frame, part ? 0x24 : 0x2C, 1);
    Put<std::uint32_t>(frame, part ? 0x30 : 0x34, slot);
    const auto attackerSlot = static_cast<uint32_t>(
        (static_cast<uint8_t*>(attacker) - room.data() - 0x3CE4) / 0x1F54);
    Put<std::uint32_t>(frame, part ? 0x08 : 0x0C, attackerSlot);
    Put<void*>(frame, part ? 0x0C : 0x10, attacker);
    SetLastError(0x1234);
    assert(Invoke(hook, frame, 91) == 91);
    assert(GetLastError() == 0x1234);
    assert(Get<std::uint32_t>(frame, part ? 0x1C : 0x20) == expectedDamage);
    assert(Get<std::uint32_t>(frame, part ? 0x24 : 0x2C) == expectedForce);
}

void Loss(std::vector<std::uint8_t>& room, void* pilot, void* unit) {
    std::vector<std::uint8_t> storage(0x100);
    auto* frame = storage.data() + 0x50;
    Put<void*>(frame, -0x34, room.data());
    Put<void*>(frame, 0x08, pilot);
    Put<void*>(frame, 0x10, unit);
    Put<std::uint32_t>(frame, 0x14, 500);
    Put<std::int32_t>(frame, 0x18, 1);
    SetLastError(0x5678);
    assert(Invoke(g_loss, frame, 92) == 92);
    assert(GetLastError() == 0x5678);
}

SdgoApi Api() {
    SdgoApi api{};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.host_key = "BattleServer-548A891D-0046C000";
    api.Symbol = &Symbol;
    api.HookSite = &HookSite;
    api.ModDirectory = &Directory;
    api.Log = &Log;
    return api;
}
}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const auto temp = std::filesystem::temp_directory_path() /
        std::filesystem::path(L"sdgo-damage-gate-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(temp);
    g_directory = temp.wstring();
    WriteRules(temp / L"damage-gates.bin");
    assert(WritePrivateProfileStringW(L"settings", L"ImmediateTransitionMissionIds", L"",
                                      (temp / L"config.ini").c_str()));

    const auto library = LoadLibraryW(std::filesystem::absolute(argv[1]).c_str());
    assert(library);
    using Entry = int(__cdecl*)(const SdgoApi*, SdgoMod*);
    const auto entry = reinterpret_cast<Entry>(GetProcAddress(library, "SdgoModLoad"));
    auto api = Api();
    assert(entry && entry(&api, reinterpret_cast<SdgoMod*>(1)) == 1);
    assert(g_body && g_part && g_loss);

    std::vector<std::uint8_t> room(0x32900);
    Mission(room);
    std::uint32_t targetData = 85303;
    std::uint32_t guardData = 97639;
    std::uint32_t attackerData = 0;
    auto* attacker = Pilot(room, 0, 101, &attackerData, 1000);
    auto* secondAttacker = Pilot(room, 1, 102, &attackerData, 1000);
    auto* target = static_cast<std::uint8_t*>(
        Pilot(room, 12, 0x80000001u, &targetData, 50000));
    auto* guard = static_cast<std::uint8_t*>(
        Pilot(room, 13, 0x80000002u, &guardData, 100));

    Hit(g_body, room, false, 12, attacker, 0, 0);
    Hit(g_part, room, true, 12, attacker, 0, 0);
    assert(g_broadcasts == 1 && g_noticeAttacker == 101 &&
           g_noticeTarget == 0x80000001u && g_noticeHealth == 50000);
    Hit(g_body, room, false, 12, secondAttacker, 0, 0);
    assert(g_broadcasts == 2 && g_noticeAttacker == 102 &&
           g_noticeTarget == 0x80000001u && g_noticeHealth == 50000);
    Hit(g_body, room, false, 13, attacker, 777, 1);  // observes the exact guard entity
    Loss(room, guard, guard + 0x340);       // HP is still nonzero
    using Count = std::uint32_t(__cdecl*)();
    const auto blocked = reinterpret_cast<Count>(
        GetProcAddress(library, "SdgoMapCatalogServerDamageBlockedV1"));
    const auto defeats = reinterpret_cast<Count>(
        GetProcAddress(library, "SdgoMapCatalogServerGuardDefeatsV1"));
    assert(blocked && defeats && blocked() == 3 && defeats() == 0);

    Put<std::uint32_t>(guard, 0x340 + 0x34, 0);
    Loss(room, guard, guard + 0x340 + 0x658);  // wrong selected form unit
    assert(defeats() == 0);
    Loss(room, guard, guard + 0x340);
    assert(defeats() == 1);
    Hit(g_body, room, false, 12, attacker, 777, 1);
    assert(g_broadcasts == 2);
    assert(Get<std::uint32_t>(target, 0x340 + 0x34) == 50000);

    FreeLibrary(library);

    // An absent declaration disables the feature, but an existing empty declaration is malformed.
    std::ofstream(temp / L"damage-gates.bin", std::ios::binary | std::ios::trunc).close();
    g_body = g_part = g_loss = nullptr;
    const auto malformed = LoadLibraryW(std::filesystem::absolute(argv[1]).c_str());
    assert(malformed);
    const auto malformedEntry = reinterpret_cast<Entry>(GetProcAddress(malformed, "SdgoModLoad"));
    api = Api();
    assert(malformedEntry && malformedEntry(&api, reinterpret_cast<SdgoMod*>(1)) == 0);
    assert(!g_body && !g_part && !g_loss);
    FreeLibrary(malformed);

    std::filesystem::remove_all(temp);
    std::puts("loaded damage-gate DLL: hit wrappers, seen death, selected native HP and malformed policy passed");
    return 0;
}
