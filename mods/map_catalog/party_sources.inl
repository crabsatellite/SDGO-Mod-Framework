// Included inside the map client implementation after native/Lua bindings.
// Monotonic milliseconds are exactly representable by a Lua number for the
// lifetime of a Windows session. Party movies start timing after actor setup.
int __cdecl LuaMissionClockMs(lua_State* L) {
    g_luaPushNumber(L, static_cast<double>(GetTickCount64()));
    return 1;
}

// Returning at most sixteen numbers fits Lua 5.0's guaranteed C callback stack.
int __cdecl LuaPartySources(lua_State* L) {
    struct Source { uint32_t object, npc; } sources[8]{};
    uint32_t count = 0;
    __try {
        auto* mode = static_cast<uint8_t*>(g_activeMissionMode);
        auto* app = g_gameApp ? g_gameApp() : nullptr;
        auto* service = app ? *reinterpret_cast<uint8_t**>(app + 0x4A4) : nullptr;
        auto* document = g_unitManager ? *g_unitManager : nullptr;
        if (!mode || !app || !service || !document || !g_objectForPilot || !g_findObject ||
            *reinterpret_cast<uint32_t*>(app + 0x4A0) != 22 ||
            *reinterpret_cast<uint8_t**>(service + 0x0C) != mode) return 0;
        auto* room = *reinterpret_cast<uint8_t**>(mode + kModeRoom);
        if (!room) return 0;
        const uint32_t localPilot = U32(room + kRoomLocalPlayer);
        const uint32_t localId = localPilot ? g_objectForPilot(document, localPilot) : 0;
        auto* local = localId ? g_findObject(document, localId) : nullptr;
        if (!local || U32(local + 4) != localId) return 0;
        const uint32_t team = U32(local + 0x20);
        // CBaseRoomData keeps AI participant records in a separate vector,
        // also used by mission_ai and the native AI battle modes. Never scan
        // the document's NPC list: it also contains mission enemies.
        constexpr size_t spans[][2] = {{kRoomHumansBegin, kRoomHumansEnd}, {0x3C, 0x40}};
        for (unsigned group = 0; group < 2; ++group) {
          auto* first = *reinterpret_cast<uint8_t**>(room + spans[group][0]);
          auto* last = *reinterpret_cast<uint8_t**>(room + spans[group][1]);
          const uintptr_t begin = reinterpret_cast<uintptr_t>(first);
          const uintptr_t end = reinterpret_cast<uintptr_t>(last);
          if ((begin == 0) != (end == 0) || end < begin ||
              (end - begin) % kHumanRecordBytes != 0 ||
              (end - begin) / kHumanRecordBytes > 12) return 0;
          for (auto* record = first; record != last; record += kHumanRecordBytes) {
            const uint32_t pilot = U32(record);
            if (!pilot || ((pilot & 0x80000000u) != 0) != (group == 1)) continue;
            const uint32_t id = g_objectForPilot(document, pilot);
            auto* unit = id ? g_findObject(document, id) : nullptr;
            if (!unit || U32(unit + 4) != id || U32(unit + 0x2B68) != pilot ||
                U32(unit + 0x20) != team || *reinterpret_cast<int32_t*>(unit + 0x38) <= 0) continue;
            uint32_t npc = 0;
            if (pilot & 0x80000000u) {
                const auto* definition = *reinterpret_cast<const uint8_t**>(unit + 0x320C);
                if (!definition) continue;
                npc = U32(definition);
                const uint8_t* native = nullptr;
                if (!npc || !g_find || !g_data || !g_find(g_data(), &native, npc, 0x29, 0) || !native)
                    continue;
            }
            bool duplicate = false;
            for (uint32_t i = 0; i < count; ++i) duplicate |= sources[i].object == id;
            if (duplicate) continue;
            if (count == 8) return 0;
            sources[count++] = {id, npc};
          }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_api->Log(g_mod, SDGO_LOG_WARN, "party cinematic source snapshot was unavailable");
        return 0;
    }
    for (uint32_t i = 0; i < count; ++i) {
        g_luaPushNumber(L, static_cast<double>(sources[i].object));
        g_luaPushNumber(L, static_cast<double>(sources[i].npc));
    }
    g_api->Log(g_mod, SDGO_LOG_INFO, "party cinematic source snapshot: %lu living room participants",
               static_cast<unsigned long>(count));
    return static_cast<int>(count * 2);
}
