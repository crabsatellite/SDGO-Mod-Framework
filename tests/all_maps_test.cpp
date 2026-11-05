#include "sdgo/all_maps.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    using namespace sdgo::all_maps;

    int failures = 0;
    const auto check = [&](bool okay, const char* text) {
        if (!okay) {
            ++failures;
            std::printf("FAIL %s\n", text);
        }
    };

    for (uint32_t mode : {1u, 3u, 6u, 7u, 9u}) {
        check(PvpMode(mode), "native PvP mode classified");
    }
    for (uint32_t mode : {0u, 5u, 8u, 10u, 11u, 12u, 13u, 14u, 255u}) {
        check(
            !PvpMode(mode),
            "missions and special AI formats remain separate");
    }

    std::vector<Entry> entries;
    for (size_t index = 0; index < 8; ++index) {
        Entry entry{};
        entry.map = static_cast<uint32_t>(index + 1);
        entry.modes[index] = 1;
        entries.push_back(entry);
    }
    entries.push_back(entries[2]);
    entries.push_back({0, {1}});
    entries.push_back({256, {1}});
    check(
        Pool(entries) == std::vector<uint32_t>({1, 2, 3, 4, 6}),
        "union contains each PvP map once and excludes non-PvP rows");

    std::array<uint8_t, 38> row{};
    uint32_t map = 112;
    std::memcpy(row.data() + 8, &map, sizeof(map));
    row[0xE] = 1;
    const auto grid = ServerEntry(row.data());
    check(
        grid.map == 112 && grid.modes[2] && PvpEntry(grid),
        "native grid-only record is a complete-pool member");

    row.fill(0);
    map = 139;
    std::memcpy(row.data() + 8, &map, sizeof(map));
    row[0x10] = 1;
    check(
        !PvpEntry(ServerEntry(row.data())),
        "native mission-only map is not introduced into PvP All Map");
    row[0xC] = 1;
    check(
        PvpEntry(ServerEntry(row.data())),
        "a terrain shared by missions and PvP remains eligible for PvP");
    check(
        !PvpEntry(ServerEntry(nullptr)),
        "an unavailable native row produces no map");

    std::printf("All Map policy: %d failures\n", failures);
    return failures ? 1 : 0;
}
