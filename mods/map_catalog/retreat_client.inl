#if !defined(_M_IX86)
#error map_catalog retreat hooks require the 32-bit GOnline ABI
#endif

using SuccessCleanup = int(__stdcall*)(const std::uint8_t*);

SuccessCleanup g_successCleanup = nullptr;
void* g_retreatFxOriginal = nullptr;
void* g_retreatFxSkip = nullptr;
void* g_retreatAudioOriginal = nullptr;
void* g_retreatAudioSkip = nullptr;
thread_local sdgo::map_catalog::retreat_client::DispatchContext* g_retreatContext = nullptr;
constexpr DWORD kRetreatDefaultMxcsr = 0x1F80;
constexpr std::size_t kPendingRetreatEffects = 16;
struct PendingRetreatEffect {
    std::uint32_t mission = 0;
    std::uint32_t phase = 0;
    std::uint32_t listedId = 0;
    sdgo::map_catalog::retreat_client::Position position{};
};
SRWLOCK g_retreatEffectLock = SRWLOCK_INIT;
std::array<PendingRetreatEffect, kPendingRetreatEffects> g_retreatEffects{};
std::size_t g_retreatEffectCount = 0;
bool g_retreatFxFault = false;
SRWLOCK g_missionSuccessLock = SRWLOCK_INIT;
SdgoMapMissionSuccessV1 g_missionSuccess{
    sizeof(SdgoMapMissionSuccessV1), SDGO_MAP_MISSION_SUCCESS_VERSION};

void PublishMissionSuccess(SdgoMapMissionSuccessV1& receipt, int nativeResult) {
    if (!receipt.mission_id || !receipt.phase || !receipt.clear_id) return;
    AcquireSRWLockExclusive(&g_missionSuccessLock);
    receipt.original_result = static_cast<uint32_t>(nativeResult);
    receipt.sequence = g_missionSuccess.sequence + 1;
    if (!receipt.sequence) receipt.sequence = 1;
    g_missionSuccess = receipt;
    ReleaseSRWLockExclusive(&g_missionSuccessLock);
}

SdgoMapMissionSuccessV1 MissionSuccessSnapshot() {
    AcquireSRWLockShared(&g_missionSuccessLock);
    auto value = g_missionSuccess;
    ReleaseSRWLockShared(&g_missionSuccessLock);
    value.hook_active = g_successCleanup ? 1u : 0u;
    return value;
}

bool LoadRetreats(const std::wstring& path, std::string& error) {
    g_retreats.clear();
    error.clear();
    FILE* stream = nullptr;
    const errno_t openError = _wfopen_s(&stream, path.c_str(), L"rb");
    if (openError != 0 || !stream) {
        if (openError == ENOENT) return true;  // optional
        error = "retreat declaration could not be opened";
        return false;
    }
    std::vector<std::uint8_t> raw;
    std::uint8_t buffer[4096];
    std::size_t read = 0;
    const std::size_t maximum = sdgo::mission_retreat::kHeaderBytes +
        static_cast<std::size_t>(sdgo::mission_retreat::kRowBytes) *
            sdgo::mission_retreat::kMaxRows;
    while ((read = std::fread(buffer, 1, sizeof(buffer), stream)) > 0) {
        raw.insert(raw.end(), buffer, buffer + read);
        if (raw.size() > maximum) break;
    }
    const bool fileError = std::ferror(stream) != 0;
    std::fclose(stream);
    if (fileError) {
        error = "retreat declaration could not be read";
        return false;
    }
    if (raw.size() > maximum) {
        error = "retreat declaration is too large";
        return false;
    }
    return sdgo::mission_retreat::Parse(raw.data(), raw.size(), g_retreats, error);
}

bool ValidateRetreats(std::string& error) {
    for (const auto& entry : g_retreats) {
        if (!CatalogContains(kGameModeType, entry.missionId)) {
            error = "retreat declaration references a mission outside catalog.bin";
            return false;
        }
    }
    return true;
}

void CaptureRetreatContext(
    const std::uint8_t* packet,
    sdgo::map_catalog::retreat_client::DispatchContext& context) {
    using namespace sdgo::map_catalog::retreat_client;
    context.count = 0;
    if (!packet || !g_gameApp || !g_unitManager || !g_objectForPilot || !g_findObject) return;
    const std::uint16_t opcode = *reinterpret_cast<const std::uint16_t*>(packet);
    const std::uint8_t count = packet[14];
    if (!IsAuthoritativeCleanup(opcode, count)) return;

    std::uint8_t* app = g_gameApp();
    std::uint8_t* service = app ? *reinterpret_cast<std::uint8_t**>(app + 0x4A4) : nullptr;
    auto* mode = service ? *reinterpret_cast<std::uint8_t**>(service + 0x0C) : nullptr;
    std::uint32_t mission = 0;
    std::uint32_t phase = 0;
    std::uint8_t* document = *g_unitManager;
    if (!document || !ActiveMissionContext(mode, mission, phase)) return;
    const std::uint32_t clearId = U32(packet + 6);
    const std::uint32_t packetPhase = U32(packet + 10);
    const std::uint8_t* clear = nullptr;
    constexpr int kMissionClearType = 0x2F;
    if (!clearId || packetPhase != phase ||
        !g_find(g_data(), &clear, clearId, kMissionClearType, 0) || !clear ||
        clear[5] != 1 || U32(clear + 41) != mission || U32(clear + 45) != phase) return;
    context.mission = mission;
    context.phase = phase;

    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t listedId =
            *reinterpret_cast<const std::uint32_t*>(packet + kPacketIdsOffset + i * 4);
        if (!listedId) continue;
        const std::uint32_t objectId = g_objectForPilot(document, listedId);
        std::uint8_t* unit = objectId ? CanonicalObject(document, objectId) : nullptr;
        if (!unit || *reinterpret_cast<std::uint32_t*>(unit + 0x2B68) != listedId) continue;
        Candidate candidate{
            listedId,
            *reinterpret_cast<std::uint32_t*>(unit + 0x3238),
            unit,
            *reinterpret_cast<std::int32_t*>(unit + 0x38),
            {}};
        if (!Eligible(g_retreats, mission, phase, candidate)) continue;
        std::uint8_t* node = *reinterpret_cast<std::uint8_t**>(unit + 8);
        if (node) {
            const auto world = *reinterpret_cast<const BoundaryPosition*>(node + 0x88);
            const float height = *reinterpret_cast<const float*>(unit + 0xC88);
            candidate.position = {
                world.x, world.y + height * 8.333333f, world.z, height * 0.25f};
            candidate.hasPosition = std::isfinite(candidate.position.x) &&
                std::isfinite(candidate.position.y) &&
                std::isfinite(candidate.position.z) &&
                std::isfinite(candidate.position.scale);
        }
        Add(context, candidate);
    }
}

void QueueRetreatEffects(const sdgo::map_catalog::retreat_client::DispatchContext& context) noexcept {
    if (!context.mission || !context.phase) return;
    AcquireSRWLockExclusive(&g_retreatEffectLock);
    for (std::size_t i = 0; i < context.count; ++i) {
        const auto& unit = context.units[i];
        if (!unit.message || !unit.hasPosition) continue;
        if (g_retreatEffectCount == g_retreatEffects.size()) {
            for (std::size_t j = 1; j < g_retreatEffectCount; ++j)
                g_retreatEffects[j - 1] = g_retreatEffects[j];
            --g_retreatEffectCount;
        }
        g_retreatEffects[g_retreatEffectCount++] = {
            context.mission, context.phase, unit.listedId, unit.position};
    }
    ReleaseSRWLockExclusive(&g_retreatEffectLock);
}

std::size_t DrainRetreatEffects(
    std::array<PendingRetreatEffect, kPendingRetreatEffects>& effects) noexcept {
    AcquireSRWLockExclusive(&g_retreatEffectLock);
    const std::size_t count = g_retreatEffectCount;
    std::copy_n(g_retreatEffects.begin(), count, effects.begin());
    g_retreatEffectCount = 0;
    ReleaseSRWLockExclusive(&g_retreatEffectLock);
    return count;
}

bool ShowRetreatEffect(const PendingRetreatEffect& effect) {
    if (!g_luaLoad || !g_luaPcall || g_retreatFxFault) return false;
    char script[384]{};
    const _locale_t locale = _create_locale(LC_NUMERIC, "C");
    if (!locale) return false;
    const int length = _sprintf_s_l(
        script, sizeof(script),
        "CreateEffectA('0416.zfx',vector3(%.3f,%.3f,%.3f),"
        "vector3(%.3f,%.3f,%.3f),vector3(0,0,1),vector3(0,1,0))",
        locale,
        effect.position.x, effect.position.y, effect.position.z,
        effect.position.scale, effect.position.scale, effect.position.scale);
    _free_locale(locale);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(script)) return false;
    EnsureMissionScripts();
    lua_State* L = g_scriptVm;
    if (!L) return false;
    const int top = g_luaGetTop(L);
    Chunk chunk{script, static_cast<std::size_t>(length)};
    int status = g_luaLoad(L, ReadChunk, &chunk, "=map_catalog/retreat_fx.lua");
    if (status == 0) status = g_luaPcall(L, 0, 0, 0);
    if (status != 0) {
        const char* message = g_luaToString(L, -1);
        g_api->Log(g_mod, SDGO_LOG_ERROR, "allied retreat effect failed: %s",
                   message ? message : "unknown Lua error");
        g_retreatFxFault = true;
        g_luaSetTop(L, top);
        return false;
    }
    g_luaSetTop(L, top);
    return true;
}

void RetreatFxTick(std::uint8_t* mode) {
    std::array<PendingRetreatEffect, kPendingRetreatEffects> effects{};
    const std::size_t count = DrainRetreatEffects(effects);
    if (!count) return;
    std::uint32_t mission = 0;
    std::uint32_t phase = 0;
    if (!ActiveMissionContext(mode, mission, phase)) return;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& effect = effects[i];
        if (effect.mission != mission || effect.phase != phase) continue;
        const bool shown = ShowRetreatEffect(effect);
        g_api->Log(g_mod, shown ? SDGO_LOG_INFO : SDGO_LOG_WARN,
                   "allied retreat receipt: mission=%lu phase=%lu pilot=%lu effect=%u",
                   static_cast<unsigned long>(mission),
                   static_cast<unsigned long>(phase),
                   static_cast<unsigned long>(effect.listedId), shown ? 1u : 0u);
    }
}

int __stdcall SuccessCleanupDetour(const std::uint8_t* packet) {
    const DWORD incomingError = GetLastError();
    sdgo::map_catalog::retreat_client::DispatchContext context{};
    SdgoMapMissionSuccessV1 receipt{
        sizeof(SdgoMapMissionSuccessV1), SDGO_MAP_MISSION_SUCCESS_VERSION};
    __try {
        CaptureRetreatContext(packet, context);
        if (context.mission && context.phase) {
            receipt.mission_id = context.mission;
            receipt.phase = context.phase;
            receipt.clear_id = U32(packet + 6);
            receipt.event_order = U32(packet + 10);
            receipt.count = packet[14];
            for (uint32_t i = 0; i < receipt.count && i < 10; ++i)
                receipt.pilot_ids[i] = U32(packet + 15 + i * 4);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        context.count = 0;
        receipt.mission_id = 0;
    }

    auto* previous = g_retreatContext;
    int result = 0;
    __try {
        g_retreatContext = &context;
        SetLastError(incomingError);
        result = g_successCleanup(packet);
        const DWORD nativeError = GetLastError();
        QueueRetreatEffects(context);
        PublishMissionSuccess(receipt, result);
        SetLastError(nativeError);
    } __finally {
        const DWORD nativeError = GetLastError();
        g_retreatContext = previous;
        SetLastError(nativeError);
    }
    return result;
}

BOOL __stdcall ShouldSuppressRetreatPresentation(const void* unit, const void* message) noexcept {
    const DWORD error = GetLastError();
    BOOL suppress = FALSE;
    __try {
        const auto type = message
            ? *reinterpret_cast<const std::uint16_t*>(message) : std::uint16_t{};
        suppress = sdgo::map_catalog::retreat_client::ArmPresentation(
            g_retreatContext, unit, message, type) ? TRUE : FALSE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        suppress = FALSE;
    }
    SetLastError(error);
    return suppress;
}

#define SDGO_RETREAT_SITE_BRIDGE(Name, NativeTarget, SkipTarget) \
    __declspec(naked) void Name() { \
        __asm { pushfd } \
        __asm { pushad } \
        __asm { mov eax, esp } \
        __asm { sub esp, 0x218 } \
        __asm { and esp, 0xfffffff0 } \
        __asm { mov [esp + 0x200], eax } \
        __asm { fxsave [esp] } \
        __asm { fninit } \
        __asm { ldmxcsr dword ptr [kRetreatDefaultMxcsr] } \
        __asm { cld } \
        __asm { mov edx, [esp + 0x200] } \
        __asm { push dword ptr [edx + 4] } \
        __asm { push dword ptr [edx + 0x10] } \
        __asm { call ShouldSuppressRetreatPresentation } \
        __asm { mov edx, [esp + 0x200] } \
        __asm { test eax, eax } \
        __asm { jz native_path } \
        __asm { mov eax, dword ptr [SkipTarget] } \
        __asm { jmp selected_path } \
    native_path: \
        __asm { mov eax, dword ptr [NativeTarget] } \
    selected_path: \
        __asm { mov [edx + 0x0C], eax } \
        __asm { fxrstor [esp] } \
        __asm { mov esp, [esp + 0x200] } \
        __asm { popad } \
        __asm { popfd } \
        __asm { jmp dword ptr [esp - 24] } \
    }

SDGO_RETREAT_SITE_BRIDGE(RetreatFxSite, g_retreatFxOriginal, g_retreatFxSkip)
SDGO_RETREAT_SITE_BRIDGE(RetreatAudioSite, g_retreatAudioOriginal, g_retreatAudioSkip)

#undef SDGO_RETREAT_SITE_BRIDGE
