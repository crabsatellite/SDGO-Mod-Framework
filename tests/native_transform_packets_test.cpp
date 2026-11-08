#include "../mods/game_test_client/native_transform_packets.h"
#include <cstring>
#include <string>
#include <vector>

namespace {
bool enabled = false;
int failures = 0;
std::vector<std::string> observed;
void Check(bool value, const char* label) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", label);
    if (!value) ++failures;
}
int Setting(SdgoMod*, const char*, int) { return enabled ? 1 : 0; }
void Log(SdgoMod*, int, const char*, ...) {}
void Observe(const char* text) { observed.emplace_back(text); }
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    SdgoApi api{}; api.SettingInt = &Setting; api.Log = &Log;
    SdgoGameTestContextV1 context{}; context.Observe = &Observe;
    apsalus_probe::TransformPackets observer;
    Check(observer.Install(&api, nullptr), "disabled observation needs no provider");
    enabled = true;
    Check(!observer.Install(&api, nullptr), "missing production fanout fails explicitly");
    HMODULE provider = LoadLibraryA(argv[1]);
    if (!provider) return 3;
    const auto emit = reinterpret_cast<void(__cdecl*)(uint16_t, const uint8_t*)>(GetProcAddress(provider, "EmitTransformFixture"));
    if (!emit) return 4;
    Check(observer.Install(&api, nullptr), "production-compatible packet sink registers without another native hook");
    unsigned char notice[19]{0x44, 0x0a}, reply[8]{0x43, 0x0a};
    unsigned pilot = 102, target = 901; unsigned short error = 0x1234;
    std::memcpy(notice + 6, &pilot, 4); std::memcpy(notice + 10, &target, 4);
    std::memcpy(reply + 6, &error, 2);
    const auto original = std::string(reinterpret_cast<const char*>(notice), sizeof(notice));
    SetLastError(0x11223344);
    emit(0x0a44, notice); emit(0x0a43, reply); emit(0xffff, notice); emit(0x0a44, nullptr);
    Check(GetLastError() == 0x11223344, "packet callback preserves caller last-error");
    Check(original == std::string(reinterpret_cast<const char*>(notice), sizeof(notice)), "borrowed packet remains unchanged");
    std::memset(notice, 0, sizeof(notice)); std::memset(reply, 0, sizeof(reply));
    observer.Observe(&context);
    Check(observed.size() == 2 && observed[0].find("pilot=102 target=901") != std::string::npos &&
          observed[1].find("error=4660") != std::string::npos, "only transform packets are copied before native storage is reused");
    observer.Observe(&context);
    Check(observed.size() == 2, "drained observations are not replayed");
    for (unsigned i = 0; i < 40; ++i) emit(0x0a44, notice);
    observer.Observe(&context);
    Check(observed.size() == 35 && observed.back().find("dropped=8") != std::string::npos, "bounded queue reports lost evidence instead of overwriting it");
    FreeLibrary(provider);
    return failures ? 1 : 0;
}
