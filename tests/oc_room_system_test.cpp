// Room-system choice from a room title (OC Mods in "room" mode): !oc, !g1, !g2
// and the default, through the core room-command service and the older-core
// fallback alike.
#include "ai_test_support.h"
#include "sdgo/oc_room_system.h"

#include <cstring>

namespace oc = sdgo::oc_custom;

namespace {
int Service(const char* text, uint32_t length, const char* name, uint32_t flags, SdgoRoomCommand* out) {
    return sdgo::room_commands::Parse(text, length, name, flags, out);
}

void Expect(const char* title, oc::RoomSystem expected) {
    SdgoApi api{};
    api.size = sizeof(api);
    api.FindRoomCommand = &Service;
    const auto length = static_cast<uint32_t>(std::strlen(title));
    CHECK(oc::RoomSystemOfTitle(&api, title, length) == expected);
    api.size = static_cast<uint32_t>(offsetof(SdgoApi, FindRoomCommand));
    CHECK(oc::RoomSystemOfTitle(&api, title, length) == expected);
}
} // namespace

int main() {
    SCENARIO("each command selects its system and no command keeps G改 2.0");
    Expect("gt1a2b !ai e2 a1 !oc", oc::RoomSystem::oc);
    Expect("gt1a2b !ai e2 a1 !g1", oc::RoomSystem::g1);
    Expect("gt1a2b !ai e2 a1 !g2", oc::RoomSystem::parts);
    Expect("gt1a2b !ai e2 a1", oc::RoomSystem::parts);
    Expect("!OC practice", oc::RoomSystem::oc);
    Expect("!G1", oc::RoomSystem::g1);

    SCENARIO("!oc wins over !g1 in either order");
    Expect("room !g1 !oc", oc::RoomSystem::oc);
    Expect("room !oc !g1", oc::RoomSystem::oc);

    SCENARIO("malformed or unknown commands keep the default");
    for (const char* title : {"room!oc", "!oc3", "!ocx", "!g", "!g3", "!g0", "!g1x", "x!g1", "", "!"})
        Expect(title, oc::RoomSystem::parts);

    SCENARIO("names for logs and labels");
    CHECK(std::strcmp(oc::RoomSystemName(oc::RoomSystem::oc), "oc") == 0);
    CHECK(std::strcmp(oc::RoomSystemName(oc::RoomSystem::g1), "g1") == 0);
    CHECK(std::strcmp(oc::RoomSystemName(oc::RoomSystem::parts), "g2") == 0);
    CHECK(std::wcscmp(oc::RoomSystemLabel(oc::RoomSystem::g1), L"G改 1.0") == 0);
    return ai_test::Finish();
}
