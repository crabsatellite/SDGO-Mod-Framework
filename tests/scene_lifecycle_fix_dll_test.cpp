// Execute the production x86 DLL against exact native calling conventions.
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <stdexcept>

#include "sdgo/scene_lifecycle_observer.h"
#include "sdgo/sdgo_api.h"

namespace {

constexpr DWORD kAllocationException = 0xE1234501u;
constexpr DWORD kCapacityException = 0xE1234502u;
constexpr std::uint32_t kMaximumListSize = 0x3FFFFFFEu;

struct Node {
    Node* next = nullptr;
    Node* previous = nullptr;
    void* scene = nullptr;
};
static_assert(sizeof(Node) == 12, "native scene list node ABI");

struct Owner {
    std::array<std::uint8_t, 0x18> prefix{};
    Node* sentinel = nullptr;
    std::uint32_t count = 0;
};
static_assert(offsetof(Owner, sentinel) == 0x18, "native owner list offset");
static_assert(offsetof(Owner, count) == 0x1C, "native owner count offset");

struct Scene {
    std::array<std::uint8_t, 0xE4> prefix{};
    Owner* owner = nullptr;
};
static_assert(offsetof(Scene, owner) == 0xE4, "native scene owner offset");

enum class AllocationMode {
    success,
    return_null,
    raise_exception,
};

enum class Event : std::uint8_t {
    get_owner,
    allocate,
    remove,
    set_owner,
    length_error,
};

using Detour = void(__fastcall*)(Owner*, void*, Scene*);

std::array<std::uint8_t, 0x6B> g_reparentBody{
    0x55,0x8B,0xEC,0x53,0x56,0x8B,0x75,0x08,0x8B,0xD9,0x8B,0xCE,0xE8,0x2F,0x4E,0xFF,
    0xFF,0x3B,0xC3,0x74,0x50,0x85,0xC0,0x74,0x08,0x56,0x8B,0xC8,0xE8,0x9F,0xF9,0xFF,
    0xFF,0x57,0x8B,0x7B,0x18,0x8B,0x4F,0x04,0x8D,0x45,0x08,0x50,0x51,0x57,0x8D,0x4B,
    0x18,0xE8,0x9A,0x18,0xD6,0xFF,0x8B,0x4B,0x1C,0xBA,0xFE,0xFF,0xFF,0x3F,0x2B,0xD1,
    0x83,0xFA,0x01,0x73,0x0A,0x68,0x48,0xEF,0xE2,0x00,0xE8,0x20,0x56,0x08,0x00,0x41,
    0x89,0x4B,0x1C,0x89,0x47,0x04,0x8B,0x48,0x04,0x89,0x01,0x8B,0x4D,0x08,0x53,0xE8,
    0xEC,0x4D,0xFF,0xFF,0x5F,0x5E,0x5B,0x5D,0xC2,0x04,0x00};

unsigned g_checks = 0;
unsigned g_hooks = 0;
unsigned g_getOwnerCalls = 0;
unsigned g_removeCalls = 0;
unsigned g_allocateCalls = 0;
unsigned g_setOwnerCalls = 0;
unsigned g_lengthErrorCalls = 0;
AllocationMode g_allocationMode = AllocationMode::success;
Node* g_spareNode = nullptr;
Node* g_retiredNode = nullptr;
bool g_retireRemovedNode = false;
Detour g_detour = nullptr;
std::array<Event, 8> g_events{};
std::size_t g_eventCount = 0;
Owner* g_expectedDestination = nullptr;
Node* g_expectedNext = nullptr;
Node* g_expectedPrevious = nullptr;
Scene* g_expectedScene = nullptr;
Owner* g_expectedOldOwner = nullptr;
Node* g_expectedOldSentinel = nullptr;
Node* g_expectedOldNode = nullptr;

void Check(bool condition, const char* message) {
    ++g_checks;
    if (!condition) throw std::runtime_error(message);
}

void Record(Event event) {
    Check(g_eventCount < g_events.size(), "native call trace has bounded storage");
    g_events[g_eventCount++] = event;
}

void CheckEvents(
    std::initializer_list<Event> expected,
    const char* message) {
    bool matches = expected.size() == g_eventCount;
    std::size_t index = 0;
    for (Event event : expected) {
        matches = matches && g_events[index++] == event;
    }
    Check(matches, message);
}

void ResetCalls() {
    g_getOwnerCalls = 0;
    g_removeCalls = 0;
    g_allocateCalls = 0;
    g_setOwnerCalls = 0;
    g_lengthErrorCalls = 0;
    g_allocationMode = AllocationMode::success;
    g_spareNode = nullptr;
    g_retiredNode = nullptr;
    g_retireRemovedNode = false;
    g_eventCount = 0;
    g_expectedDestination = nullptr;
    g_expectedNext = nullptr;
    g_expectedPrevious = nullptr;
    g_expectedScene = nullptr;
    g_expectedOldOwner = nullptr;
    g_expectedOldSentinel = nullptr;
    g_expectedOldNode = nullptr;
}

Node* AllocateNodePage() {
    return static_cast<Node*>(VirtualAlloc(
        nullptr,
        4096,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE));
}

void ReleaseNodePage(Node* node) {
    if (node) VirtualFree(node, 0, MEM_RELEASE);
}

void InitializeEmpty(Owner& owner, Node& sentinel) {
    sentinel.next = &sentinel;
    sentinel.previous = &sentinel;
    sentinel.scene = nullptr;
    owner.sentinel = &sentinel;
    owner.count = 0;
}

void InitializeOwned(
    Owner& owner,
    Node& sentinel,
    Node* node,
    Scene& scene) {
    sentinel.next = node;
    sentinel.previous = node;
    sentinel.scene = nullptr;
    node->next = &sentinel;
    node->previous = &sentinel;
    node->scene = &scene;
    owner.sentinel = &sentinel;
    owner.count = 1;
    scene.owner = &owner;
}

void ExpectAllocation(
    Owner& destination,
    Node& next,
    Node& previous,
    Scene& scene,
    Owner* oldOwner = nullptr,
    Node* oldSentinel = nullptr,
    Node* oldNode = nullptr) {
    g_expectedDestination = &destination;
    g_expectedNext = &next;
    g_expectedPrevious = &previous;
    g_expectedScene = &scene;
    g_expectedOldOwner = oldOwner;
    g_expectedOldSentinel = oldSentinel;
    g_expectedOldNode = oldNode;
}

void Retire(Node* node) {
    if (!node) return;
    DWORD ignored = 0;
    Check(
        VirtualProtect(
            node,
            4096,
            PAGE_NOACCESS,
            &ignored) != 0,
        "retired native node becomes inaccessible");
    g_retiredNode = node;
}

void* __fastcall GetOwner(Scene* scene, void*) {
    ++g_getOwnerCalls;
    Record(Event::get_owner);
    return scene->owner;
}

void __fastcall Remove(Owner* owner, void*, Scene* scene) {
    ++g_removeCalls;
    Record(Event::remove);
    Node* const sentinel = owner->sentinel;
    for (Node* node = sentinel->next;
         node != sentinel;
         node = node->next) {
        if (node->scene != scene) continue;
        node->previous->next = node->next;
        node->next->previous = node->previous;
        --owner->count;
        if (g_retireRemovedNode) Retire(node);
        return;
    }
}

void* __fastcall AllocateNode(
    void* list,
    void*,
    Node* next,
    Node* previous,
    Scene* const* value) {
    ++g_allocateCalls;
    Record(Event::allocate);
    Check(
        g_expectedDestination &&
            list ==
                static_cast<void*>(
                    reinterpret_cast<std::uint8_t*>(
                        g_expectedDestination) +
                    offsetof(Owner, sentinel)) &&
            next == g_expectedNext &&
            previous == g_expectedPrevious &&
            value &&
            *value == g_expectedScene,
        "native allocator receives exact thiscall list, sentinel, tail and scene arguments");
    if (g_expectedOldOwner) {
        Check(
            g_expectedScene->owner == g_expectedOldOwner &&
                g_expectedOldOwner->count == 1 &&
                g_expectedOldOwner->sentinel ==
                    g_expectedOldSentinel &&
                g_expectedOldSentinel->next ==
                    g_expectedOldNode &&
                g_expectedOldSentinel->previous ==
                    g_expectedOldNode &&
                g_expectedOldNode->next ==
                    g_expectedOldSentinel &&
                g_expectedOldNode->previous ==
                    g_expectedOldSentinel &&
                g_expectedOldNode->scene ==
                    g_expectedScene,
            "destination allocation occurs while the old registration is intact");
    } else {
        Check(
            !g_expectedScene->owner,
            "unowned scene remains unowned during destination allocation");
    }
    if (g_allocationMode == AllocationMode::raise_exception) {
        RaiseException(
            kAllocationException,
            0,
            0,
            nullptr);
        return nullptr;
    }
    if (g_allocationMode == AllocationMode::return_null) {
        return nullptr;
    }
    Check(g_spareNode != nullptr, "success allocator has an owned native node page");
    g_spareNode->next = next;
    g_spareNode->previous = previous;
    g_spareNode->scene = *value;
    return g_spareNode;
}

void __fastcall SetOwner(Scene* scene, void*, Owner* owner) {
    ++g_setOwnerCalls;
    Record(Event::set_owner);
    scene->owner = owner;
}

void __cdecl LengthError(const char* message) {
    ++g_lengthErrorCalls;
    Record(Event::length_error);
    Check(
        message &&
            std::strcmp(
                message,
                "list<T> too long") == 0,
        "capacity failure retains the native length-error message");
    RaiseException(
        kCapacityException,
        0,
        0,
        nullptr);
}

void __fastcall OriginalReparent(
    Owner*,
    void*,
    Scene*) {
    throw std::runtime_error("detour must not delegate to the unsafe native order");
}

void Log(SdgoMod*, int, const char*, ...) {
}

bool g_missingAllocator = false;

void* Symbol(SdgoMod*, const char* name) {
    if (!std::strcmp(name, "scene_lifecycle.reparent")) {
        return g_reparentBody.data();
    }
    if (!std::strcmp(name, "scene_lifecycle.get_owner")) {
        return reinterpret_cast<void*>(&GetOwner);
    }
    if (!std::strcmp(name, "scene_lifecycle.remove")) {
        return reinterpret_cast<void*>(&Remove);
    }
    if (!std::strcmp(name, "scene_lifecycle.allocate_node")) {
        return g_missingAllocator
                   ? nullptr
                   : reinterpret_cast<void*>(&AllocateNode);
    }
    if (!std::strcmp(name, "scene_lifecycle.set_owner")) {
        return reinterpret_cast<void*>(&SetOwner);
    }
    if (!std::strcmp(name, "scene_lifecycle.length_error")) {
        return reinterpret_cast<void*>(&LengthError);
    }
    return nullptr;
}

int HookCode(
    SdgoMod*,
    const char* name,
    void* detour,
    void** original) {
    Check(
        !std::strcmp(
            name,
            "scene_lifecycle.reparent"),
        "only the verified reparent function is hooked");
    ++g_hooks;
    g_detour = reinterpret_cast<Detour>(detour);
    *original = reinterpret_cast<void*>(&OriginalReparent);
    return 1;
}

DWORD g_caughtException = 0;

int ExceptionFilter(EXCEPTION_POINTERS* pointers) {
    g_caughtException =
        pointers && pointers->ExceptionRecord
            ? pointers->ExceptionRecord->ExceptionCode
            : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}

bool CallWithException(
    Owner* owner,
    Scene* scene) {
    g_caughtException = 0;
    __try {
        g_detour(owner, nullptr, scene);
        return false;
    } __except (ExceptionFilter(GetExceptionInformation())) {
        return true;
    }
}

bool DestroySceneWithException(Scene* scene) {
    g_caughtException = 0;
    __try {
        if (scene->owner) {
            Remove(scene->owner, nullptr, scene);
        }
        return false;
    } __except (ExceptionFilter(GetExceptionInformation())) {
        return true;
    }
}

void DestroyOwner(Owner& owner) {
    Node* const sentinel = owner.sentinel;
    Node* node = sentinel->next;
    while (node != sentinel) {
        Node* const next = node->next;
        SetOwner(
            static_cast<Scene*>(node->scene),
            nullptr,
            nullptr);
        sentinel->next = next;
        next->previous = sentinel;
        --owner.count;
        Retire(node);
        node = next;
    }
    sentinel->previous = sentinel;
}

void VerifyUnchangedOwned(
    const Owner& owner,
    const Node& sentinel,
    const Node* node,
    const Scene& scene) {
    Check(
        owner.count == 1 &&
            owner.sentinel == &sentinel &&
            sentinel.next == node &&
            sentinel.previous == node &&
            node->next == &sentinel &&
            node->previous == &sentinel &&
            node->scene == &scene &&
            scene.owner == &owner,
        "allocation failure preserves the exact old registration and owner");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        HMODULE const module = LoadLibraryW(argv[1]);
        Check(module != nullptr, "actual production DLL loads");
        auto const entry = reinterpret_cast<SdgoModLoadFn>(
            GetProcAddress(module, "SdgoModLoad"));
        auto const status =
            reinterpret_cast<SdgoSceneLifecycleGetStatusV1Fn>(
                GetProcAddress(
                    module,
                    SDGO_SCENE_LIFECYCLE_STATUS_EXPORT));
        Check(
            entry &&
                status,
            "loader and read-only status export are present");

        SdgoApi api{};
        api.size = sizeof(api);
        api.version = SDGO_API_VERSION;
        api.Symbol = &Symbol;
        api.HookCode = &HookCode;
        api.Log = &Log;
        g_missingAllocator = true;
        Check(
            !entry(&api, nullptr) &&
                !g_hooks,
            "missing native allocator refuses activation before hooking");
        g_missingAllocator = false;
        g_reparentBody.back() ^= 0x01u;
        Check(
            !entry(&api, nullptr) &&
                !g_hooks,
            "a changed native return immediate rejects activation");
        g_reparentBody.back() ^= 0x01u;
        Check(
            entry(&api, nullptr) &&
                g_hooks == 1 &&
                g_detour,
            "verified production detour binds exactly once");
        Check(
            !entry(&api, nullptr) &&
                g_hooks == 1,
            "another load cannot duplicate the lifecycle hook");

        {
            ResetCalls();
            Owner owner{};
            Node sentinel{};
            Scene scene{};
            Node* const node = AllocateNodePage();
            Check(node != nullptr, "same-owner fixture node page allocated");
            InitializeOwned(owner, sentinel, node, scene);
            g_detour(&owner, nullptr, &scene);
            Check(
                g_getOwnerCalls == 1 &&
                    !g_allocateCalls &&
                    !g_removeCalls &&
                    !g_setOwnerCalls,
                "same node remains an exact no-op");
            CheckEvents(
                {Event::get_owner},
                "same-owner trace stops after the native owner lookup");
            VerifyUnchangedOwned(
                owner,
                sentinel,
                node,
                scene);
            ReleaseNodePage(node);
        }

        {
            ResetCalls();
            Owner oldOwner{}, newOwner{};
            Node oldSentinel{}, newSentinel{};
            Scene scene{}, existingScene{};
            Node* const oldNode = AllocateNodePage();
            Node* const newNode = AllocateNodePage();
            Node* const existingNode = AllocateNodePage();
            Check(
                oldNode &&
                    newNode &&
                    existingNode,
                "transfer fixture node pages allocated");
            InitializeOwned(
                oldOwner,
                oldSentinel,
                oldNode,
                scene);
            InitializeOwned(
                newOwner,
                newSentinel,
                existingNode,
                existingScene);
            g_spareNode = newNode;
            g_retireRemovedNode = true;
            ExpectAllocation(
                newOwner,
                newSentinel,
                *existingNode,
                scene,
                &oldOwner,
                &oldSentinel,
                oldNode);
            g_detour(
                &newOwner,
                nullptr,
                &scene);
            Check(
                g_allocateCalls == 1 &&
                    g_removeCalls == 1 &&
                    g_setOwnerCalls == 1,
                "successful transfer allocates, removes and assigns once");
            CheckEvents(
                {
                    Event::get_owner,
                    Event::allocate,
                    Event::remove,
                    Event::set_owner,
                },
                "successful transfer allocates before old removal and owner assignment");
            Check(
                oldOwner.count == 0 &&
                    oldSentinel.next == &oldSentinel &&
                    oldSentinel.previous == &oldSentinel,
                "old list is retired only after destination allocation");
            Check(
                newOwner.count == 2 &&
                    newSentinel.next == existingNode &&
                    newSentinel.previous == newNode &&
                    newNode->next == &newSentinel &&
                    newNode->previous == existingNode &&
                    newNode->scene == &scene &&
                    existingNode->next == newNode &&
                    existingNode->previous == &newSentinel &&
                    existingNode->scene == &existingScene &&
                    scene.owner == &newOwner,
                "successful transfer appends to a nonempty native list and assigns the owner");
            DestroyOwner(newOwner);
            Check(
                !scene.owner &&
                    !existingScene.owner &&
                    newOwner.count == 0 &&
                    newSentinel.next == &newSentinel &&
                    newSentinel.previous == &newSentinel,
                "node retirement clears the scene owner before releasing the node");
            Check(
                !DestroySceneWithException(&scene),
                "scene destruction is safe after its registry node retires");
            ReleaseNodePage(oldNode);
            ReleaseNodePage(newNode);
            ReleaseNodePage(existingNode);
        }

        {
            ResetCalls();
            Owner newOwner{};
            Node newSentinel{};
            Scene scene{};
            Node* const newNode = AllocateNodePage();
            Check(newNode != nullptr, "unowned transfer node page allocated");
            InitializeEmpty(newOwner, newSentinel);
            g_spareNode = newNode;
            ExpectAllocation(
                newOwner,
                newSentinel,
                newSentinel,
                scene);
            g_detour(&newOwner, nullptr, &scene);
            Check(
                !g_removeCalls &&
                    g_allocateCalls == 1 &&
                    g_setOwnerCalls == 1 &&
                    newOwner.count == 1 &&
                    scene.owner == &newOwner,
                "a scene without an old owner links directly into the destination");
            CheckEvents(
                {
                    Event::get_owner,
                    Event::allocate,
                    Event::set_owner,
                },
                "unowned transfer allocates before assigning its first owner");
            ReleaseNodePage(newNode);
        }

        {
            ResetCalls();
            Owner oldOwner{}, newOwner{};
            Node oldSentinel{}, newSentinel{};
            Scene scene{};
            Node* const oldNode = AllocateNodePage();
            Check(oldNode != nullptr, "null-allocation old node page allocated");
            InitializeOwned(oldOwner, oldSentinel, oldNode, scene);
            InitializeEmpty(newOwner, newSentinel);
            g_allocationMode = AllocationMode::return_null;
            ExpectAllocation(
                newOwner,
                newSentinel,
                newSentinel,
                scene,
                &oldOwner,
                &oldSentinel,
                oldNode);
            g_detour(&newOwner, nullptr, &scene);
            Check(
                g_allocateCalls == 1 &&
                    !g_removeCalls &&
                    !g_setOwnerCalls &&
                    newOwner.count == 0,
                "null allocator result performs no ownership commit");
            CheckEvents(
                {Event::get_owner, Event::allocate},
                "null allocation returns before old removal");
            VerifyUnchangedOwned(
                oldOwner,
                oldSentinel,
                oldNode,
                scene);
            ReleaseNodePage(oldNode);
        }

        {
            ResetCalls();
            Owner oldOwner{}, newOwner{};
            Node oldSentinel{}, newSentinel{};
            Scene scene{};
            Node* const oldNode = AllocateNodePage();
            Check(oldNode != nullptr, "throwing-allocation old node page allocated");
            InitializeOwned(oldOwner, oldSentinel, oldNode, scene);
            InitializeEmpty(newOwner, newSentinel);
            g_allocationMode = AllocationMode::raise_exception;
            ExpectAllocation(
                newOwner,
                newSentinel,
                newSentinel,
                scene,
                &oldOwner,
                &oldSentinel,
                oldNode);
            Check(
                CallWithException(
                    &newOwner,
                    &scene) &&
                    g_caughtException == kAllocationException,
                "native allocator exception propagates unchanged");
            Check(
                g_allocateCalls == 1 &&
                    !g_removeCalls &&
                    !g_setOwnerCalls &&
                    newOwner.count == 0,
                "allocator exception occurs before old removal or destination commit");
            CheckEvents(
                {Event::get_owner, Event::allocate},
                "allocator exception propagates directly from the precommit allocation");
            VerifyUnchangedOwned(
                oldOwner,
                oldSentinel,
                oldNode,
                scene);
            ReleaseNodePage(oldNode);
        }

        {
            ResetCalls();
            Owner oldOwner{}, newOwner{};
            Node oldSentinel{}, newSentinel{};
            Scene scene{};
            Node* const oldNode = AllocateNodePage();
            Check(oldNode != nullptr, "capacity fixture old node page allocated");
            InitializeOwned(oldOwner, oldSentinel, oldNode, scene);
            InitializeEmpty(newOwner, newSentinel);
            newOwner.count = kMaximumListSize;
            Check(
                CallWithException(
                    &newOwner,
                    &scene) &&
                    g_caughtException == kCapacityException,
                "native capacity exception propagates unchanged");
            Check(
                g_lengthErrorCalls == 1 &&
                    !g_allocateCalls &&
                    !g_removeCalls &&
                    !g_setOwnerCalls &&
                    newOwner.count == kMaximumListSize,
                "capacity failure occurs before allocation or old removal");
            CheckEvents(
                {Event::get_owner, Event::length_error},
                "capacity failure reaches the native length error before allocation");
            VerifyUnchangedOwned(
                oldOwner,
                oldSentinel,
                oldNode,
                scene);
            ReleaseNodePage(oldNode);
        }

        SdgoSceneLifecycleStatusV1 snapshot{
            sizeof(snapshot),
            SDGO_SCENE_LIFECYCLE_STATUS_VERSION};
        Check(
            status(&snapshot) &&
                snapshot.process_id == GetCurrentProcessId() &&
                snapshot.reparent_calls == 6 &&
                snapshot.allocation_attempts == 4 &&
                snapshot.transfers == 2 &&
                snapshot.same_owner == 1 &&
                snapshot.from_unowned == 1 &&
                snapshot.allocator_null == 1 &&
                snapshot.capacity_errors == 1,
            "read-only status reports exact production detour outcomes");
        snapshot.version = 2;
        Check(
            !status(&snapshot),
            "status rejects an unknown ABI version");
        Check(
            !status(nullptr),
            "status rejects null output");

        FreeLibrary(module);
        std::printf(
            "PASS %u scene lifecycle production DLL checks\n",
            g_checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(
            stderr,
            "FAIL %s\n",
            error.what());
        return 1;
    }
}
