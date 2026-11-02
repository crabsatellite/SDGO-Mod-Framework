// Load the actual shipping Mod DLL, exercising its native calling convention,
// original-update ordering, exact recovery scope and unchanged player/model data.
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "sdgo/sdgo_api.h"

namespace {
using HookFn = void(__fastcall*)(uint8_t*, void*, void*);
HookFn hook = nullptr;
unsigned checks = 0, originals = 0, signals = 0, selections = 0, logs = 0;
bool missingSymbol = false;
std::array<uint8_t, 0x300> move;
std::array<uint8_t, 0xe80> unit;
std::array<uint8_t, 0x150> current, parked;
std::array<uint8_t, 0x10> context, events;
std::array<void*, 0x40> vtable;

void Check(bool ok, const char* reason) { ++checks; if (!ok) throw std::runtime_error(reason); }
template<class T> void Put(void* p, size_t offset, T value) { std::memcpy(static_cast<uint8_t*>(p) + offset, &value, sizeof(value)); }
void __fastcall Original(uint8_t* self, void*, void* input) {
    Check(self == move.data() && input == context.data(), "original native this/context preserved");
    ++originals; SetLastError(0x1234);
}
uint8_t __fastcall Signal(uint8_t* self, void*, void* state) {
    Check(originals == 1 && self == move.data() && state == self + 0x20,
          "native WAIT signal occurs after exactly one original update with its exact receiver");
    ++signals; Put(self, 8, state); SetLastError(17); return 1;
}
void __fastcall Select(uint8_t* self, void*, unsigned state) {
    Check(signals == 1 && self == move.data() && state == 1,
          "native WAIT selection follows its signal and receives the native state ID");
    ++selections; SetLastError(18);
}
void Log(SdgoMod*, int, const char*, ...) { ++logs; SetLastError(19); }
void* Symbol(SdgoMod*, const char* name) {
    Check(!std::strcmp(name, "transform_fix.signal_state"), "only the signed native signal helper is bound");
    return missingSymbol ? nullptr : reinterpret_cast<void*>(&Signal);
}
int Hook(SdgoMod*, const char* name, void* detour, void** original) {
    Check(!std::strcmp(name, "transform_fix.update"), "only native TransformUpdate is detoured");
    hook = reinterpret_cast<HookFn>(detour); *original = reinterpret_cast<void*>(&Original); return 1;
}
void Reset(unsigned id = 24787, unsigned peer = 24788, unsigned target = 24787) {
    move.fill(0); unit.fill(0x5a); current.fill(0x3c); parked.fill(0xa5); context.fill(0); events.fill(0); vtable.fill(nullptr);
    Put(move.data(), 0, vtable.data()); Put(move.data(), 0x21c, unit.data());
    Put(move.data(), 0x2c0, 3u); Put(move.data(), 0x1ac, 0u); Put(move.data(), 0x2b0, target);
    Put(unit.data(), 0x9c4, current.data()); Put(unit.data(), 0xab0, parked.data());
    Put(current.data(), 0, id); Put(parked.data(), 0, peer);
    Put(context.data(), 8, events.data()); Put(events.data(), 4, 1u);
    vtable[0xd8 / 4] = reinterpret_cast<void*>(&Select);
    originals = signals = selections = logs = 0;
}
void Run(bool recover) {
    const auto originalMove = move; const auto originalUnit = unit;
    const auto originalCurrent = current; const auto originalParked = parked;
    const auto originalEvents = events; const auto originalContext = context;
    hook(move.data(), nullptr, context.data());
    Check(originals == 1 && signals == (recover ? 1u : 0u) && selections == signals && logs == signals,
          "shipping DLL performs only the expected recovery, once");
    Check(GetLastError() == 0x1234, "native update last-error state survives recovery diagnostics");
    auto expectedMove = originalMove;
    if (recover) Put(expectedMove.data(), 8, move.data() + 0x20);
    Check(move == expectedMove && unit == originalUnit && current == originalCurrent && parked == originalParked &&
          events == originalEvents && context == originalContext,
          "only the native pending-WAIT pointer changes; all model, stats, input and event data stay intact");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        auto dll = LoadLibraryW(argv[1]);
        auto entry = dll ? reinterpret_cast<SdgoModLoadFn>(GetProcAddress(dll, "SdgoModLoad")) : nullptr;
        Check(entry != nullptr, "actual shipping DLL loads and exports SdgoModLoad");
        SdgoApi api{}; api.size = sizeof(api); api.version = SDGO_API_VERSION;
        api.Log = Log; api.Symbol = Symbol; api.HookCode = Hook;
        missingSymbol = true; Check(entry(&api, nullptr) == 0 && !hook, "unverified native helper refuses activation");
        missingSymbol = false; Check(entry(&api, nullptr) != 0 && hook, "verified shipping Mod installs");
        Reset(); Run(true); // early swap: 24788 -> 24787, null change_ma
        Reset(24787, 24788, 24788); Run(true); // late swap: null change_ms, native Release owns commit
        Reset(24788, 24787, 24787); Run(true);
        Reset(); Put(move.data(), 0x1ac, 41u); Run(false); // resolved transform unchanged
        Reset(); Put(move.data(), 0x2c0, 0u); Run(false);
        Reset(); Put(move.data(), 0x2c0, 1u); Run(false);
        Reset(); Put(move.data(), 0x2c0, 2u); Run(false);
        Reset(); Put(events.data(), 4, 0u); Run(false); // native fallback retained
        Reset(); Put(move.data(), 8, move.data() + 0x20); Run(false); // already queued
        Reset(10021, 10022, 10022); Run(false);
        Reset(24787, 10022, 24788); Run(false);
        Reset(24787, 24787, 24788); Run(false);
        Reset(24787, 24788, 10022); Run(false);
        Reset(); Put(move.data(), 0x21c, static_cast<void*>(nullptr)); Run(false);
        Reset(); Put(unit.data(), 0x9c4, static_cast<void*>(nullptr)); Run(false);
        Reset(); Put(unit.data(), 0xab0, static_cast<void*>(nullptr)); Run(false);
        Reset(); Put(context.data(), 8, static_cast<void*>(nullptr)); Run(false);
        FreeLibrary(dll);
        std::printf("%u actual Mod DLL ABI/scope/data-preservation checks passed\n", checks); return 0;
    } catch (const std::exception& e) { std::printf("FAIL: %s\n", e.what()); return 1; }
}
