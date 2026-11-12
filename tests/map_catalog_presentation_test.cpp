#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../mods/map_catalog/presentation.h"

namespace m = sdgo::map_catalog;

void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void Put(m::Entry& entry, size_t offset, uint32_t value) {
    std::memcpy(entry.bytes.data() + offset, &value, sizeof(value));
}

int main(int argc, char** argv) {
    m::Catalog catalog;
    m::Entry npc{m::kNpcTemplateType, 8001, 0, std::vector<uint8_t>(598)};
    Put(npc, 0, npc.id);
    Put(npc, 390, 7001); // inherited stock-mission reference, absent in this package
    Put(npc, 394, 7002);
    npc.bytes[398] = 's';
    Check(!m::DeclaresUnitCutin(catalog, npc), "inherited missing speech must not enable the native mode switch");
    catalog.records.push_back({m::kUnitType, 7001, 0, {}});
    Check(!m::DeclaresUnitCutin(catalog, npc), "an unrelated grid kind cannot satisfy speech ownership");
    catalog.records.push_back({m::kSpeechType, 7001, 0, std::vector<uint8_t>(506)});
    Check(!m::DeclaresUnitCutin(catalog, npc), "missing inherited ending speech also prevents accidental opt-in");
    Put(npc, 394, 0);
    Check(m::DeclaresUnitCutin(catalog, npc), "authored speech plus illustration retains native cut-in eligibility");
    npc.bytes[398] = 0;
    Check(!m::DeclaresUnitCutin(catalog, npc), "speech without a unit illustration remains ordinary dialogue");
    if (argc > 1) {
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[1], -1, nullptr, 0);
        Check(length > 0, "catalog path is UTF-8");
        std::wstring path(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[1], -1, path.data(), length);
        std::string error;
        Check(catalog.Load(path, error), error.c_str());
        unsigned enabled = 0, inherited = 0;
        for (const auto& entry : catalog.records) {
            if (m::DeclaresUnitCutin(catalog, entry)) ++enabled;
            else if (entry.kind == m::kNpcTemplateType && entry.bytes.size() == 598 &&
                     m::U32(entry.bytes.data() + 390) && entry.bytes[398]) ++inherited;
        }
        std::printf("Catalog presentation: %u authored cut-ins; %u inherited references excluded\n", enabled, inherited);
    }
    std::puts("PASS imported NPC presentation ownership");
}
