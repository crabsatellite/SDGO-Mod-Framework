// The shipped scene registry removes an object from its old node before the
// destination list node is allocated. A native allocation exception therefore
// leaves the scene pointing at an owner whose list no longer contains it.
//
// Allocate the destination's native list node first. It remains detached until
// allocation succeeds, after which the original remove, link and owner updates
// are committed in their original order.
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sdgo/scene_lifecycle_observer.h"
#include "sdgo/sdgo_api.h"

namespace {

static_assert(sizeof(void*) == 4, "the admitted scene registry ABI is x86");

constexpr std::size_t kOwnerList = 0x18;
constexpr std::size_t kOwnerCount = 0x1C;
constexpr std::size_t kNodeNext = 0x00;
constexpr std::size_t kNodePrevious = 0x04;
constexpr std::uint32_t kMaximumListSize = 0x3FFFFFFEu;
constexpr std::size_t kReparentBodySize = 0x6B;
constexpr std::uint32_t kReparentBodyHash = 0x2EC6F6CAu;
constexpr char kListTooLong[] = "list<T> too long";

using ReparentFn = void(__thiscall*)(void* owner, void* scene);
using GetOwnerFn = void*(__thiscall*)(void* scene);
using RemoveFn = void(__thiscall*)(void* owner, void* scene);
using AllocateNodeFn = void*(__thiscall*)(
    void* list,
    void* next,
    void* previous,
    void* const* value);
using SetOwnerFn = void(__thiscall*)(void* scene, void* owner);
using LengthErrorFn = void(__cdecl*)(const char* message);

ReparentFn g_original = nullptr;
GetOwnerFn g_getOwner = nullptr;
RemoveFn g_remove = nullptr;
AllocateNodeFn g_allocateNode = nullptr;
SetOwnerFn g_setOwner = nullptr;
LengthErrorFn g_lengthError = nullptr;

volatile LONG g_reparentCalls = 0;
volatile LONG g_allocationAttempts = 0;
volatile LONG g_transfers = 0;
volatile LONG g_sameOwner = 0;
volatile LONG g_fromUnowned = 0;
volatile LONG g_allocatorNull = 0;
volatile LONG g_capacityErrors = 0;

template <class T>
T Read(const void* pointer, std::size_t offset = 0) {
    T value{};
    std::memcpy(
        &value,
        static_cast<const std::uint8_t*>(pointer) + offset,
        sizeof(value));
    return value;
}

template <class T>
void Write(void* pointer, std::size_t offset, T value) {
    std::memcpy(
        static_cast<std::uint8_t*>(pointer) + offset,
        &value,
        sizeof(value));
}

bool OriginalBody(const std::uint8_t* bytes) noexcept {
    if (!bytes) return false;
    std::uint32_t hash = 2166136261u;
    __try {
        for (std::size_t index = 0; index < kReparentBodySize; ++index) {
            hash = (hash ^ bytes[index]) * 16777619u;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return hash == kReparentBodyHash;
}

void __fastcall Reparent(void* newOwner, void*, void* scene) {
    InterlockedIncrement(&g_reparentCalls);
    void* const oldOwner = g_getOwner(scene);
    if (oldOwner == newOwner) {
        InterlockedIncrement(&g_sameOwner);
        return;
    }

    const std::uint32_t count =
        Read<std::uint32_t>(newOwner, kOwnerCount);
    if (count == kMaximumListSize) {
        InterlockedIncrement(&g_capacityErrors);
        g_lengthError(kListTooLong);
        return;
    }

    void* const sentinel = Read<void*>(newOwner, kOwnerList);
    void* const previous = Read<void*>(sentinel, kNodePrevious);
    void* storedScene = scene;
    InterlockedIncrement(&g_allocationAttempts);
    void* const node = g_allocateNode(
        static_cast<std::uint8_t*>(newOwner) + kOwnerList,
        sentinel,
        previous,
        &storedScene);
    if (!node) {
        InterlockedIncrement(&g_allocatorNull);
        return;
    }

    if (oldOwner) {
        g_remove(oldOwner, scene);
    } else {
        InterlockedIncrement(&g_fromUnowned);
    }

    Write<std::uint32_t>(
        newOwner,
        kOwnerCount,
        Read<std::uint32_t>(newOwner, kOwnerCount) + 1u);
    Write<void*>(sentinel, kNodePrevious, node);
    void* const nodePrevious = Read<void*>(node, kNodePrevious);
    Write<void*>(nodePrevious, kNodeNext, node);
    g_setOwner(scene, newOwner);
    InterlockedIncrement(&g_transfers);
}

template <class T>
bool Bind(T& output, const SdgoApi* api, SdgoMod* mod, const char* name) {
    output = reinterpret_cast<T>(api->Symbol(mod, name));
    return output != nullptr;
}

std::uint32_t Counter(volatile LONG* value) {
    return static_cast<std::uint32_t>(
        InterlockedCompareExchange(value, 0, 0));
}

} // namespace

extern "C" __declspec(dllexport)
int __cdecl SdgoSceneLifecycleGetStatusV1(
    SdgoSceneLifecycleStatusV1* output) {
    if (!output ||
        output->size < sizeof(SdgoSceneLifecycleStatusV1) ||
        output->version != SDGO_SCENE_LIFECYCLE_STATUS_VERSION) {
        return 0;
    }
    SdgoSceneLifecycleStatusV1 result{};
    result.size = sizeof(result);
    result.version = SDGO_SCENE_LIFECYCLE_STATUS_VERSION;
    result.process_id = GetCurrentProcessId();
    result.reparent_calls = Counter(&g_reparentCalls);
    result.allocation_attempts = Counter(&g_allocationAttempts);
    result.transfers = Counter(&g_transfers);
    result.same_owner = Counter(&g_sameOwner);
    result.from_unowned = Counter(&g_fromUnowned);
    result.allocator_null = Counter(&g_allocatorNull);
    result.capacity_errors = Counter(&g_capacityErrors);
    std::memcpy(output, &result, sizeof(result));
    return 1;
}

SDGO_MOD_ENTRY {
    if (!api ||
        api->size <
            offsetof(SdgoApi, HookCode) +
                sizeof(api->HookCode) ||
        !api->Symbol ||
        !api->HookCode ||
        !api->Log ||
        g_original) {
        return 0;
    }

    auto* const body = static_cast<const std::uint8_t*>(
        api->Symbol(mod, "scene_lifecycle.reparent"));
    GetOwnerFn getOwner = nullptr;
    RemoveFn remove = nullptr;
    AllocateNodeFn allocateNode = nullptr;
    SetOwnerFn setOwner = nullptr;
    LengthErrorFn lengthError = nullptr;
    if (!OriginalBody(body) ||
        !Bind(
            getOwner,
            api,
            mod,
            "scene_lifecycle.get_owner") ||
        !Bind(
            remove,
            api,
            mod,
            "scene_lifecycle.remove") ||
        !Bind(
            allocateNode,
            api,
            mod,
            "scene_lifecycle.allocate_node") ||
        !Bind(
            setOwner,
            api,
            mod,
            "scene_lifecycle.set_owner") ||
        !Bind(
            lengthError,
            api,
            mod,
            "scene_lifecycle.length_error")) {
        return 0;
    }

    g_getOwner = getOwner;
    g_remove = remove;
    g_allocateNode = allocateNode;
    g_setOwner = setOwner;
    g_lengthError = lengthError;
    ReparentFn original = nullptr;
    if (!api->HookCode(
            mod,
            "scene_lifecycle.reparent",
            reinterpret_cast<void*>(&Reparent),
            reinterpret_cast<void**>(&original))) {
        g_getOwner = nullptr;
        g_remove = nullptr;
        g_allocateNode = nullptr;
        g_setOwner = nullptr;
        g_lengthError = nullptr;
        return 0;
    }
    g_original = original;
    api->Log(
        mod,
        SDGO_LOG_INFO,
        "scene registry reparenting allocates the destination node before "
        "retiring the prior registration");
    return 1;
}
