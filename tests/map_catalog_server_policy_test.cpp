#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstdint>
#include <string>

#include "mods/map_catalog_server/policy.h"

using sdgo::map_catalog_server::AcceleratedDeadline;
using sdgo::map_catalog_server::MissionAllowlist;
using sdgo::map_catalog_server::ParseMissionIds;

int main() {
    MissionAllowlist ids{};
    assert(ParseMissionIds(" 90003, 90001;90002,90001 ", ids));
    assert(ids.count == 3);
    assert(ids.ids[0] == 90001 && ids.ids[1] == 90002 && ids.ids[2] == 90003);
    assert(ids.Contains(90002));
    assert(!ids.Contains(90004));
    assert(AcceleratedDeadline(102, 90002, ids) == 99);
    assert(AcceleratedDeadline(102, 77, ids) == 102);
    assert(AcceleratedDeadline(2, 90002, ids) == 0);

    for (const char* invalid : std::array<const char*, 8>{nullptr, "0", "1,,2", "1;", "-1", "+1", "1 x", "4294967296"}) {
        MissionAllowlist rejected{};
        assert(!ParseMissionIds(invalid, rejected));
    }
    MissionAllowlist empty{};
    assert(ParseMissionIds("   ", empty) && empty.count == 0);
    return 0;
}
