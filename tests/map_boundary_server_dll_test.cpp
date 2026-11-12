#ifdef NDEBUG
#undef NDEBUG
#endif
#include <windows.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/map_boundary_format.h"
#include "sdgo/map_boundary_contact.h"

namespace {
alignas(16) uint8_t proc[128]{};
std::vector<uint8_t> room(0x32900);
std::wstring directory;
void* handler = nullptr;
unsigned ordinary = 0, locked = 0, unlocked = 0, broadcasts = 0;
uint32_t lastTarget = 0, lastHealth = 0;
template<class T> void Put(void* base, size_t offset, T value) {
    std::memcpy(static_cast<uint8_t*>(base) + offset, &value, sizeof(value));
}
template<class T> T Get(const void* base, size_t offset) {
    T value{}; std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value)); return value;
}
void __fastcall Original(void*, void*, uint32_t, const uint8_t*) { ++ordinary; }
int __fastcall Find(void*, void*, const uint32_t* source, uint32_t* index) {
    *index = (*source == 101 || *source == 102) ? 1u : 0u; return *index ? 1 : 0;
}
uint8_t* __fastcall Lock(void* self, void*, uint32_t index, uint16_t tag) {
    assert(self == proc && index == 1 && tag == 0x0492); ++locked; return room.data();
}
void __fastcall Unlock(void* self, void*, uint32_t index) {
    assert(self == proc && index == 1); ++unlocked;
}
int __fastcall Broadcast(void* self, void*, uint8_t* packet, uint32_t size, uint32_t* recipients) {
    assert(self == room.data() && size == 13 && Get<uint16_t>(packet, 0) == 0x0793);
    lastTarget = sdgo::map_boundary_contact::NoticeTarget(Get<uint32_t>(packet, 6));
    lastHealth = sdgo::map_boundary_contact::DecodeHealth(packet + 10);
    assert(lastTarget && lastHealth);
    *recipients = 2; ++broadcasts; return 1;
}
void* Symbol(SdgoMod*, const char* name) {
    if (!std::strcmp(name, "battle.waitroom_proc")) return proc;
    if (!std::strcmp(name, "items.pilot_map_find")) return reinterpret_cast<void*>(&Find);
    if (!std::strcmp(name, "items.lock_room")) return reinterpret_cast<void*>(&Lock);
    if (!std::strcmp(name, "items.unlock_room")) return reinterpret_cast<void*>(&Unlock);
    if (!std::strcmp(name, "battle.room_random_broadcast")) return reinterpret_cast<void*>(&Broadcast);
    return nullptr;
}
int HookCode(SdgoMod*, const char* name, void* hook, void** original) {
    assert(!std::strcmp(name, "map_catalog_server.map_contact_request"));
    handler = hook; *original = reinterpret_cast<void*>(&Original); return 1;
}
int HookSite(SdgoMod*, const char*, void*, void**) { assert(false); return 0; }
void Log(SdgoMod*, int, const char*, ...) {}
const wchar_t* Directory(SdgoMod*) { return directory.c_str(); }

void Pilot(unsigned slot, uint32_t id, uint32_t health = 1000) {
    auto* pilot = room.data() + 0x3CE4 + slot * 0x1F54;
    Put(pilot, 0, id); Put<uint32_t>(pilot, 9, 1);
    Put<uint32_t>(pilot, 0x344, 1000); Put(pilot, 0x374, health);
}
uint32_t Health(unsigned slot) { return Get<uint32_t>(room.data(), 0x3CE4 + slot * 0x1F54 + 0x374); }
void Request(uint32_t source, uint32_t target, uint32_t actualTarget = 0) {
    uint8_t request[14]{};
    Put<uint16_t>(request, 0, 0x0492); Put(request, 2, source); Put(request, 6, target); Put(request, 10, actualTarget);
    using Fn = void(__thiscall*)(void*, uint32_t, const uint8_t*);
    reinterpret_cast<Fn>(handler)(proc, 0, request);
}
void WriteRules(const std::filesystem::path& path) {
    using namespace sdgo::map_boundary;
    std::vector<uint8_t> raw(kHeaderBytes + kRowBytes);
    std::memcpy(raw.data(), kMagic, 8);
    Put<uint32_t>(raw.data(), 8, 1); Put(raw.data(), 12, kHeaderBytes); Put(raw.data(), 16, kRowBytes);
    Put<uint32_t>(raw.data(), 20, 1); Put(raw.data(), 24, kRowBytes);
    auto* row = raw.data() + kHeaderBytes;
    Put<uint32_t>(row, 0, 90001); Put<uint32_t>(row, 4, 4); Put<uint32_t>(row, 8, kAreaEnabled | kDamageEnabled);
    Put<float>(row, 16, -4600); Put<float>(row, 20, 2100); Put<float>(row, 24, 4600); Put<float>(row, 28, -6200);
    Put<uint32_t>(row, 32, kEdgeTop); Put<uint32_t>(row, 36, 50); Put<uint32_t>(row, 40, 2000);
    Put(raw.data(), 28, Fnv1a(row, kRowBytes));
    std::ofstream stream(path, std::ios::binary); stream.write(reinterpret_cast<const char*>(raw.data()), raw.size());
}
}

int main(int argc, char** argv) {
    assert(argc == 2);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const auto path = std::filesystem::absolute(argv[0]).parent_path() /
        ("boundary-contact-fixture-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directory(path); directory = path.wstring();
    WriteRules(path / "boundaries.bin");
    assert(WritePrivateProfileStringW(L"settings", L"ImmediateTransitionMissionIds", L"", (path / "config.ini").c_str()));
    InitializeCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(proc + 0x40));
    const auto dll = LoadLibraryW(std::filesystem::absolute(argv[1]).c_str()); assert(dll);
    SdgoApi api{}; api.size = sizeof(api); api.version = SDGO_API_VERSION;
    api.host_key = "BattleServer-548A891D-0046C000";
    api.ModDirectory = Directory; api.Log = Log; api.Symbol = Symbol; api.HookCode = HookCode; api.HookSite = HookSite;
    using Entry = int(__cdecl*)(const SdgoApi*, SdgoMod*);
    const auto load = reinterpret_cast<Entry>(GetProcAddress(dll, "SdgoModLoad"));
    assert(load && load(&api, reinterpret_cast<SdgoMod*>(1)) && handler);
    Put<uint32_t>(room.data(), 0x21, 1); Put<uint8_t>(room.data(), 5, 5);
    Put<uint32_t>(room.data(), 0x1B, 90001); Put<uint32_t>(room.data(), 0x316A4, 2);
    Put<uint8_t>(room.data(), 0x316F0, 4); Put<uint32_t>(room.data(), 0x4CE, 101);
    Put<uint32_t>(room.data(), 0x2EEB4, 12345);
    Pilot(0, 101); Pilot(1, 102); Pilot(12, 0x80000000u, 49);
    Pilot(13, 0xC0000007u);
    Pilot(14, 0x80000065u);
    using namespace sdgo::map_boundary_contact;
    Request(101, EncodeRequest(101, 101), 101);
    assert(Health(0) == 950 && Health(1) == 1000 && lastTarget == 101 && lastHealth == 950 && broadcasts == 1);
    Request(101, EncodeRequest(101, 101), 101); assert(Health(0) == 950 && broadcasts == 1);
    Request(102, EncodeRequest(102, 0x80000000u), 0x80000000u); assert(Health(12) == 49 && broadcasts == 1);
    Request(101, EncodeRequest(101, 0x80000000u), 0x80000000u);
    assert(Health(12) == 1 && lastTarget == 0x80000000u && lastHealth == 1 && broadcasts == 2);
    Request(101, kMarker | 102u, 102); assert(Health(1) == 1000 && broadcasts == 2);
    Request(102, EncodeRequest(102, 102), 1); assert(Health(1) == 1000 && broadcasts == 2);
    Request(101, EncodeRequest(101, 0xC0000007u), 0xC0000007u);
    assert(Health(13) == 950 && lastTarget == 0xC0000007u && lastHealth == 950 && broadcasts == 3);
    Pilot(2, kMarker | 102u);
    Request(102, EncodeRequest(102, 102), 102); assert(Health(1) == 1000 && broadcasts == 3);
    Request(101, EncodeRequest(101, 0x80000065u), 0x80000065u);
    assert(Health(0) == 950 && Health(14) == 950 && broadcasts == 4 && lastTarget == 0x80000065u);
    Put<uint32_t>(room.data(), 0x316A4, 6);
    Request(102, EncodeRequest(102, 102), 102); assert(Health(1) == 1000 && broadcasts == 4);
    Put<uint32_t>(room.data(), 0x316A4, 2); Put<uint8_t>(room.data(), 0x316F0, 5);
    Request(102, EncodeRequest(102, 102), 102); assert(Health(1) == 1000 && broadcasts == 4);
    Request(101, 101); Request(101, 0x80000000u); Request(101, 0xC0000007u); assert(ordinary == 3);
    assert(locked >= 4 && unlocked == locked);
    FreeLibrary(dll); DeleteCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(proc + 0x40));
    // The task recycles this owned build fixture with its other test artifacts.
    std::puts("loaded server DLL: authoritative nonlethal HP, host NPC admission, cooldown and native bypass passed");
}
