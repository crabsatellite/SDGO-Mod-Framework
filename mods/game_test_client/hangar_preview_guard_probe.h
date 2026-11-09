#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sdgo/hangar_preview_guard_observer.h"
#include "sdgo/sdgo_api.h"

class HangarPreviewGuardFaultProbe;

namespace hangar_preview_guard_fault_probe_detail {
using IndexUpload = unsigned char(__thiscall*)(void*, int, int);

inline void* vertex24CreateTrampoline = nullptr;
inline void* vertex24CreateResume = nullptr;
inline void* vertex32CreateTrampoline = nullptr;
inline void* vertex32CreateResume = nullptr;
inline void* vertex24LockTrampoline = nullptr;
inline void* vertex24LockAfter = nullptr;
inline void* vertex32LockTrampoline = nullptr;
inline void* vertex32LockAfter = nullptr;
inline void* lock24 = nullptr;
inline void* lock32 = nullptr;
inline void* indexCreateTrampoline = nullptr;
inline void* indexCreateAfter = nullptr;
inline IndexUpload indexUploadTrampoline = nullptr;
inline void* modelLoadTrampoline = nullptr;
inline void* modelLoadAfter = nullptr;
inline const DWORD defaultMxcsr = 0x1F80;

unsigned __stdcall Decide(unsigned path) noexcept;
int __fastcall VertexLockInjected(
    void* wrapper, unsigned kind, int start, int count, char mode) noexcept;
unsigned char __fastcall IndexUploadInjected(
    void* wrapper, unsigned kind, int start, int count) noexcept;
void Vertex24CreateBridge();
void Vertex32CreateBridge();
void Vertex24LockBridge();
void Vertex32LockBridge();
void IndexCreateBridge();
void IndexUploadBridge();
void ModelLoadBridge();
} // namespace hangar_preview_guard_fault_probe_detail

// Development-only fault injection for the sealed preview_guard GameTest
// page group. The production Mod exposes only its read-only status snapshot;
// every injection is additionally gated by that Mod's thread-local
// active_preview or active_parser predicate.
class HangarPreviewGuardFaultProbe {
public:
    struct Snapshot {
        unsigned kind = 0;
        unsigned hits = 0;
        unsigned armed = 0;
        unsigned seen_vertex24 = 0;
        unsigned seen_vertex32 = 0;
        unsigned seen_index = 0;
        unsigned seen_model = 0;
        unsigned last_unit = 0;
        unsigned actual_locks = 0;
        unsigned balancing_unlocks = 0;
        unsigned errors = 0;
    };

private:
    friend unsigned __stdcall
    hangar_preview_guard_fault_probe_detail::Decide(unsigned) noexcept;
    friend int __fastcall
    hangar_preview_guard_fault_probe_detail::VertexLockInjected(
        void*, unsigned, int, int, char) noexcept;
    friend unsigned char __fastcall
    hangar_preview_guard_fault_probe_detail::IndexUploadInjected(
        void*, unsigned, int, int) noexcept;

    static_assert(sizeof(void*) == 4, "native preview probes require x86");

    enum Path : unsigned {
        kVertex24Create = 1,
        kVertex24Lock = 2,
        kVertex32Create = 3,
        kVertex32Lock = 4,
        kIndexCreate = 5,
        kIndexUpload = 6,
        kModelLoad = 7,
    };

    using GetStatus = SdgoHangarPreviewGuardGetStatusV1Fn;
    using VertexLock = int(__thiscall*)(void*, int, int, char);
    using ComLock =
        long(__stdcall*)(void*, unsigned, unsigned, void**, unsigned);
    using ComUnlock = long(__stdcall*)(void*);

    struct IndexProxy {
        void** table = nullptr;
        void* real = nullptr;
        HangarPreviewGuardFaultProbe* owner = nullptr;
        void* wrapper = nullptr;
        unsigned kind = 0;
        unsigned restore_on_unlock = 0;
    };

    inline static HangarPreviewGuardFaultProbe* active_ = nullptr;
    inline static GetStatus getStatus_ = nullptr;

    inline static void* indexProxyTable_[13]{};
    inline static thread_local IndexProxy vertexProxy_{};

    volatile LONG kind_ = 0;
    volatile LONG hits_ = 0;
    volatile LONG armed_ = 0;
    volatile LONG seenVertex24_ = 0;
    volatile LONG seenVertex32_ = 0;
    volatile LONG seenIndex_ = 0;
    volatile LONG seenModel_ = 0;
    volatile LONG lastUnit_ = 0;
    volatile LONG actualLocks_ = 0;
    volatile LONG balancingUnlocks_ = 0;
    volatile LONG errors_ = 0;

    static LONG ReadCounter(volatile LONG* value) noexcept {
        return InterlockedCompareExchange(value, 0, 0);
    }

    static void Count(volatile LONG* value) noexcept {
        InterlockedIncrement(value);
    }

    template<class T>
    static T Read(const void* source, std::size_t offset = 0) noexcept {
        T value{};
        if (source)
            std::memcpy(
                &value, static_cast<const std::uint8_t*>(source) + offset,
                sizeof(value));
        return value;
    }

    template<class T>
    static void Write(void* destination, std::size_t offset, T value) noexcept {
        if (destination)
            std::memcpy(
                static_cast<std::uint8_t*>(destination) + offset, &value,
                sizeof(value));
    }

    static bool ReadProductionStatus(
        Path path, SdgoHangarPreviewGuardStatusV1* value) noexcept {
        if (!value) return false;
        value->size = sizeof(*value);
        value->version = 1;
        if (!getStatus_ || !getStatus_(value)) {
            if (active_) Count(&active_->errors_);
            return false;
        }
        return path == kModelLoad
            ? value->active_preview != 0
            : value->active_parser != 0;
    }

    static bool Matches(Path path, unsigned kind) noexcept {
        switch (path) {
        case kVertex24Create: return kind == 1 || kind == 2;
        case kVertex24Lock: return kind == 3 || kind == 4;
        case kVertex32Create: return kind == 5 || kind == 6;
        case kVertex32Lock: return kind == 7 || kind == 8;
        case kIndexCreate: return kind == 9 || kind == 10;
        case kIndexUpload: return kind == 11 || kind == 12;
        case kModelLoad: return kind == 13;
        default: return false;
        }
    }

    static void ObservePath(
        HangarPreviewGuardFaultProbe* self, Path path) noexcept {
        switch (path) {
        case kVertex24Create:
        case kVertex24Lock:
            Count(&self->seenVertex24_);
            break;
        case kVertex32Create:
        case kVertex32Lock:
            Count(&self->seenVertex32_);
            break;
        case kIndexCreate:
        case kIndexUpload:
            Count(&self->seenIndex_);
            break;
        case kModelLoad:
            Count(&self->seenModel_);
            break;
        default:
            Count(&self->errors_);
            break;
        }
    }

    static unsigned __stdcall Decide(unsigned rawPath) noexcept {
        const DWORD error = GetLastError();
        auto* self = active_;
        const auto path = static_cast<Path>(rawPath);
        unsigned result = 0;
        SdgoHangarPreviewGuardStatusV1 production{};
        if (self && ReadProductionStatus(path, &production)) {
            ObservePath(self, path);
            const unsigned kind =
                static_cast<unsigned>(ReadCounter(&self->kind_));
            if (production.active_failure == 0 &&
                Matches(path, kind) &&
                InterlockedCompareExchange(&self->armed_, 0, 1) == 1) {
                Count(&self->hits_);
                InterlockedExchange(
                    &self->lastUnit_,
                    static_cast<LONG>(production.active_unit));
                result = kind;
            }
        }
        SetLastError(error);
        return result;
    }

    static long __stdcall ProxyLock(
        void* object, unsigned offset, unsigned bytes, void** mapped,
        unsigned flags) noexcept {
        const DWORD incomingError = GetLastError();
        auto* proxy = static_cast<IndexProxy*>(object);
        auto* self = proxy ? proxy->owner : nullptr;
        if (!proxy || !self || !proxy->real) {
            if (self) Count(&self->errors_);
            if (mapped) *mapped = nullptr;
            SetLastError(incomingError);
            return static_cast<long>(0x80004005u);
        }
        if (proxy->kind == 11) {
            if (mapped) *mapped = nullptr;
            SetLastError(incomingError);
            return static_cast<long>(0x80004005u);
        }
        auto** table = Read<void**>(proxy->real);
        auto lock = table ? reinterpret_cast<ComLock>(table[11]) : nullptr;
        if ((proxy->kind != 4 && proxy->kind != 8 &&
             proxy->kind != 12) || !lock) {
            Count(&self->errors_);
            if (mapped) *mapped = nullptr;
            SetLastError(incomingError);
            return static_cast<long>(0x80004005u);
        }
        Count(&self->actualLocks_);
        SetLastError(incomingError);
        const long result =
            lock(proxy->real, offset, bytes, mapped, flags);
        const DWORD nativeError = GetLastError();
        const bool succeeded =
            proxy->kind == 12 ? result == 0 : result >= 0;
        if (succeeded) {
            if (mapped)
                *mapped = nullptr;
            else
                Count(&self->errors_);
        } else if (proxy->restore_on_unlock && proxy->wrapper &&
                   Read<void*>(proxy->wrapper, 4) == proxy) {
            Write<void*>(proxy->wrapper, 4, proxy->real);
        }
        SetLastError(nativeError);
        return result;
    }

    static long __stdcall ProxyUnlock(void* object) noexcept {
        const DWORD incomingError = GetLastError();
        auto* proxy = static_cast<IndexProxy*>(object);
        auto* self = proxy ? proxy->owner : nullptr;
        auto** table =
            proxy && proxy->real ? Read<void**>(proxy->real) : nullptr;
        auto unlock =
            table ? reinterpret_cast<ComUnlock>(table[12]) : nullptr;
        if (!self || !unlock) {
            if (self) Count(&self->errors_);
            SetLastError(incomingError);
            return static_cast<long>(0x80004005u);
        }
        if (proxy->restore_on_unlock) {
            if (proxy->wrapper &&
                Read<void*>(proxy->wrapper, 4) == proxy)
                Write<void*>(proxy->wrapper, 4, proxy->real);
            else
                Count(&self->errors_);
        }
        Count(&self->balancingUnlocks_);
        SetLastError(incomingError);
        const long result = unlock(proxy->real);
        const DWORD nativeError = GetLastError();
        SetLastError(nativeError);
        return result;
    }

    // EDX carries the consumed success-null kind. The bridge supplies the
    // production after-Lock site as a synthetic return address above the three
    // original native arguments, so this helper reproduces the skipped
    // __thiscall's RET 12.
    static int __fastcall VertexLockInjected(
        void* wrapper, unsigned kind, int start, int count,
        char mode) noexcept {
        const DWORD incomingError = GetLastError();
        auto* self = active_;
        auto lock = reinterpret_cast<VertexLock>(
            kind == 4 ? hangar_preview_guard_fault_probe_detail::lock24
                      : kind == 8
                          ? hangar_preview_guard_fault_probe_detail::lock32
                          : nullptr);
        if (!self || !wrapper || !lock) {
            if (self) Count(&self->errors_);
            SetLastError(incomingError);
            return -1;
        }
        void* real = Read<void*>(wrapper, 4);
        if (!real) {
            Count(&self->errors_);
            SetLastError(incomingError);
            return -1;
        }

        auto& proxy = vertexProxy_;
        proxy = IndexProxy{
            indexProxyTable_, real, self, wrapper, kind, 1};
        Write<void*>(wrapper, 4, &proxy);
        SetLastError(incomingError);
        const int result = lock(wrapper, start, count, mode);
        const DWORD nativeError = GetLastError();
        void* installed = Read<void*>(wrapper, 4);
        if (result < 0) {
            if (installed == &proxy)
                Write<void*>(wrapper, 4, real);
            else if (installed != real)
                Count(&self->errors_);
        } else if (installed != &proxy) {
            Count(&self->errors_);
        }
        SetLastError(nativeError);
        return result;
    }

    // EDX carries the already-consumed fault kind. The original return address
    // and two native arguments remain on the stack, so this __fastcall helper
    // returns directly to C32A29 with RET 8.
    static unsigned char __fastcall IndexUploadInjected(
        void* wrapper, unsigned kind, int start, int count) noexcept {
        const DWORD incomingError = GetLastError();
        auto* self = active_;
        if (!self || !wrapper ||
            !hangar_preview_guard_fault_probe_detail::indexUploadTrampoline) {
            if (self) Count(&self->errors_);
            SetLastError(incomingError);
            return 0;
        }
        void* real = Read<void*>(wrapper, 4);
        if (!real) {
            Count(&self->errors_);
            SetLastError(incomingError);
            return hangar_preview_guard_fault_probe_detail::
                indexUploadTrampoline(wrapper, start, count);
        }

        IndexProxy proxy{
            indexProxyTable_, real, self, wrapper, kind, 0};
        Write<void*>(wrapper, 4, &proxy);
        SetLastError(incomingError);
        const unsigned char result =
            hangar_preview_guard_fault_probe_detail::
                indexUploadTrampoline(wrapper, start, count);
        const DWORD nativeError = GetLastError();
        if (Read<void*>(wrapper, 4) != &proxy)
            Count(&self->errors_);
        Write<void*>(wrapper, 4, real);
        SetLastError(nativeError);
        return result;
    }

    static void* Symbol(
        const SdgoApi* api, SdgoMod* mod, const char* name) noexcept {
        return api->Symbol(mod, name);
    }

public:
    bool Install(const SdgoApi* api, SdgoMod* mod) noexcept {
        if (!api || !mod || active_ ||
            api->size < offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) ||
            !api->Symbol || !api->HookCode || !api->HookSite) {
            Count(&errors_);
            return false;
        }
        HMODULE production = GetModuleHandleW(L"hangar_preview_guard.dll");
        auto getStatus = production
            ? reinterpret_cast<GetStatus>(GetProcAddress(
                  production, SDGO_HANGAR_PREVIEW_GUARD_STATUS_EXPORT))
            : nullptr;
        if (!getStatus) {
            Count(&errors_);
            return false;
        }

        hangar_preview_guard_fault_probe_detail::vertex24LockAfter =
            Symbol(api, mod, "test.preview_guard.vertex24_lock_after");
        hangar_preview_guard_fault_probe_detail::vertex32LockAfter =
            Symbol(api, mod, "test.preview_guard.vertex32_lock_after");
        hangar_preview_guard_fault_probe_detail::lock24 =
            Symbol(api, mod, "test.preview_guard.lock24");
        hangar_preview_guard_fault_probe_detail::lock32 =
            Symbol(api, mod, "test.preview_guard.lock32");
        hangar_preview_guard_fault_probe_detail::indexCreateAfter =
            Symbol(api, mod, "test.preview_guard.index_create_after");
        hangar_preview_guard_fault_probe_detail::modelLoadAfter =
            Symbol(api, mod, "test.preview_guard.model_load_after");
        if (!hangar_preview_guard_fault_probe_detail::vertex24LockAfter ||
            !hangar_preview_guard_fault_probe_detail::vertex32LockAfter ||
            !hangar_preview_guard_fault_probe_detail::lock24 ||
            !hangar_preview_guard_fault_probe_detail::lock32 ||
            !hangar_preview_guard_fault_probe_detail::indexCreateAfter ||
            !hangar_preview_guard_fault_probe_detail::modelLoadAfter) {
            Count(&errors_);
            return false;
        }

        indexProxyTable_[11] = reinterpret_cast<void*>(&ProxyLock);
        indexProxyTable_[12] = reinterpret_cast<void*>(&ProxyUnlock);
        getStatus_ = getStatus;
        active_ = this;

        if (!api->HookSite(
                mod, "test.preview_guard.vertex24_create",
                reinterpret_cast<void*>(
                    &hangar_preview_guard_fault_probe_detail::
                        Vertex24CreateBridge),
                &hangar_preview_guard_fault_probe_detail::
                    vertex24CreateTrampoline) ||
            !api->HookSite(
                mod, "test.preview_guard.vertex32_create",
                reinterpret_cast<void*>(
                    &hangar_preview_guard_fault_probe_detail::
                        Vertex32CreateBridge),
                &hangar_preview_guard_fault_probe_detail::
                    vertex32CreateTrampoline) ||
            !api->HookSite(
                mod, "test.preview_guard.vertex24_lock",
                reinterpret_cast<void*>(
                    &hangar_preview_guard_fault_probe_detail::
                        Vertex24LockBridge),
                &hangar_preview_guard_fault_probe_detail::
                    vertex24LockTrampoline) ||
            !api->HookSite(
                mod, "test.preview_guard.vertex32_lock",
                reinterpret_cast<void*>(
                    &hangar_preview_guard_fault_probe_detail::
                        Vertex32LockBridge),
                &hangar_preview_guard_fault_probe_detail::
                    vertex32LockTrampoline) ||
            !api->HookSite(
                mod, "test.preview_guard.index_create",
                reinterpret_cast<void*>(
                    &hangar_preview_guard_fault_probe_detail::
                        IndexCreateBridge),
                &hangar_preview_guard_fault_probe_detail::
                    indexCreateTrampoline) ||
            !api->HookCode(
                mod, "test.preview_guard.index_upload",
                reinterpret_cast<void*>(
                    &hangar_preview_guard_fault_probe_detail::
                        IndexUploadBridge),
                reinterpret_cast<void**>(
                    &hangar_preview_guard_fault_probe_detail::
                        indexUploadTrampoline)) ||
            !api->HookSite(
                mod, "test.preview_guard.model_load",
                reinterpret_cast<void*>(
                    &hangar_preview_guard_fault_probe_detail::
                        ModelLoadBridge),
                &hangar_preview_guard_fault_probe_detail::
                    modelLoadTrampoline)) {
            active_ = nullptr;
            getStatus_ = nullptr;
            Count(&errors_);
            return false;
        }
        static constexpr std::uint8_t vertex24Prefix[]{
            0xFF, 0xD2, 0x89, 0x85, 0xC8, 0xFC, 0xFF, 0xFF};
        static constexpr std::uint8_t vertex32Prefix[]{
            0xFF, 0xD1, 0x89, 0x85, 0xC8, 0xFC, 0xFF, 0xFF};
        auto* vertex24Trampoline = static_cast<std::uint8_t*>(
            hangar_preview_guard_fault_probe_detail::
                vertex24CreateTrampoline);
        auto* vertex32Trampoline = static_cast<std::uint8_t*>(
            hangar_preview_guard_fault_probe_detail::
                vertex32CreateTrampoline);
        if (!vertex24Trampoline || !vertex32Trampoline ||
            std::memcmp(
                vertex24Trampoline, vertex24Prefix,
                sizeof(vertex24Prefix)) != 0 ||
            std::memcmp(
                vertex32Trampoline, vertex32Prefix,
                sizeof(vertex32Prefix)) != 0) {
            active_ = nullptr;
            getStatus_ = nullptr;
            Count(&errors_);
            return false;
        }
        // The two-byte indirect CALL is shorter than MinHook's patch. Resume
        // at the copied MOV in the trampoline, which then jumps back after the
        // entire relocated instruction span.
        hangar_preview_guard_fault_probe_detail::vertex24CreateResume =
            vertex24Trampoline + 2;
        hangar_preview_guard_fault_probe_detail::vertex32CreateResume =
            vertex32Trampoline + 2;
        return true;
    }

    void Arm(unsigned kind) noexcept {
        if (kind == 0) {
            InterlockedExchange(&armed_, 0);
            InterlockedExchange(&kind_, 0);
            return;
        }
        if (kind < 1 || kind > 13) {
            InterlockedExchange(&armed_, 0);
            InterlockedExchange(
                &kind_, static_cast<LONG>(kind));
            Count(&errors_);
            return;
        }
        InterlockedExchange(&armed_, 0);
        InterlockedExchange(
            &kind_, static_cast<LONG>(kind));
        InterlockedExchange(&armed_, 1);
    }

    Snapshot State() const noexcept {
        Snapshot value{};
        value.kind = static_cast<unsigned>(
            ReadCounter(const_cast<volatile LONG*>(&kind_)));
        value.hits = static_cast<unsigned>(
            ReadCounter(const_cast<volatile LONG*>(&hits_)));
        value.armed = static_cast<unsigned>(
            ReadCounter(const_cast<volatile LONG*>(&armed_)));
        value.seen_vertex24 = static_cast<unsigned>(
            ReadCounter(const_cast<volatile LONG*>(&seenVertex24_)));
        value.seen_vertex32 = static_cast<unsigned>(
            ReadCounter(const_cast<volatile LONG*>(&seenVertex32_)));
        value.seen_index = static_cast<unsigned>(
            ReadCounter(const_cast<volatile LONG*>(&seenIndex_)));
        value.seen_model = static_cast<unsigned>(
            ReadCounter(const_cast<volatile LONG*>(&seenModel_)));
        value.last_unit = static_cast<unsigned>(
            ReadCounter(const_cast<volatile LONG*>(&lastUnit_)));
        value.actual_locks = static_cast<unsigned>(
            ReadCounter(const_cast<volatile LONG*>(&actualLocks_)));
        value.balancing_unlocks = static_cast<unsigned>(
            ReadCounter(const_cast<volatile LONG*>(&balancingUnlocks_)));
        value.errors = static_cast<unsigned>(
            ReadCounter(const_cast<volatile LONG*>(&errors_)));
        return value;
    }
};

namespace hangar_preview_guard_fault_probe_detail {

inline unsigned __stdcall Decide(unsigned path) noexcept {
    return HangarPreviewGuardFaultProbe::Decide(path);
}

inline int __fastcall VertexLockInjected(
    void* wrapper, unsigned kind, int start, int count, char mode) noexcept {
    return HangarPreviewGuardFaultProbe::VertexLockInjected(
        wrapper, kind, start, count, mode);
}

inline unsigned char __fastcall IndexUploadInjected(
    void* wrapper, unsigned kind, int start, int count) noexcept {
    return HangarPreviewGuardFaultProbe::IndexUploadInjected(
        wrapper, kind, start, count);
}

#define SDGO_PREVIEW_PROBE_SAVE_CONTEXT                                      \
    __asm pushfd                                                             \
    __asm pushad                                                             \
    __asm mov ebx, esp                                                       \
    __asm sub esp, 210h                                                      \
    __asm and esp, 0FFFFFFF0h                                                \
    __asm mov [esp + 200h], ebx                                              \
    __asm fxsave [esp]                                                       \
    __asm fninit                                                             \
    __asm ldmxcsr dword ptr [defaultMxcsr]                                   \
    __asm cld

#define SDGO_PREVIEW_PROBE_RESTORE_CONTEXT                                   \
    __asm fxrstor [esp]                                                      \
    __asm mov esp, [esp + 200h]                                              \
    __asm popad                                                              \
    __asm popfd

__declspec(naked) inline void Vertex24CreateBridge() {
    SDGO_PREVIEW_PROBE_SAVE_CONTEXT
    __asm push 1
    __asm call Decide
    __asm cmp eax, 1
    __asm je inject_failure
    __asm cmp eax, 2
    __asm je inject_success_null
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm jmp dword ptr [vertex24CreateTrampoline]
inject_failure:
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm mov edx, [esp + 20]
    __asm test edx, edx
    __asm jz no_failure_output
    __asm mov dword ptr [edx], 0
no_failure_output:
    __asm mov eax, 80004005h
    __asm add esp, 28
    __asm jmp dword ptr [vertex24CreateResume]
inject_success_null:
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm mov edx, [esp + 20]
    __asm test edx, edx
    __asm jz no_success_output
    __asm mov dword ptr [edx], 0
no_success_output:
    __asm xor eax, eax
    __asm add esp, 28
    __asm jmp dword ptr [vertex24CreateResume]
}

__declspec(naked) inline void Vertex32CreateBridge() {
    SDGO_PREVIEW_PROBE_SAVE_CONTEXT
    __asm push 3
    __asm call Decide
    __asm cmp eax, 5
    __asm je inject_failure
    __asm cmp eax, 6
    __asm je inject_success_null
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm jmp dword ptr [vertex32CreateTrampoline]
inject_failure:
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm mov edx, [esp + 20]
    __asm test edx, edx
    __asm jz no_failure_output
    __asm mov dword ptr [edx], 0
no_failure_output:
    __asm mov eax, 80004005h
    __asm add esp, 28
    __asm jmp dword ptr [vertex32CreateResume]
inject_success_null:
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm mov edx, [esp + 20]
    __asm test edx, edx
    __asm jz no_success_output
    __asm mov dword ptr [edx], 0
no_success_output:
    __asm xor eax, eax
    __asm add esp, 28
    __asm jmp dword ptr [vertex32CreateResume]
}

__declspec(naked) inline void Vertex24LockBridge() {
    SDGO_PREVIEW_PROBE_SAVE_CONTEXT
    __asm push 2
    __asm call Decide
    __asm cmp eax, 3
    __asm je inject_failure
    __asm cmp eax, 4
    __asm je inject_success_null
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm jmp dword ptr [vertex24LockTrampoline]
inject_failure:
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm or eax, -1
    __asm add esp, 12
    __asm jmp dword ptr [vertex24LockAfter]
inject_success_null:
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm mov edx, 4
    __asm push dword ptr [vertex24LockAfter]
    __asm jmp VertexLockInjected
}

__declspec(naked) inline void Vertex32LockBridge() {
    SDGO_PREVIEW_PROBE_SAVE_CONTEXT
    __asm push 4
    __asm call Decide
    __asm cmp eax, 7
    __asm je inject_failure
    __asm cmp eax, 8
    __asm je inject_success_null
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm jmp dword ptr [vertex32LockTrampoline]
inject_failure:
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm or eax, -1
    __asm add esp, 12
    __asm jmp dword ptr [vertex32LockAfter]
inject_success_null:
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm mov edx, 8
    __asm push dword ptr [vertex32LockAfter]
    __asm jmp VertexLockInjected
}

__declspec(naked) inline void IndexCreateBridge() {
    SDGO_PREVIEW_PROBE_SAVE_CONTEXT
    __asm push 5
    __asm call Decide
    __asm cmp eax, 9
    __asm je inject_failure
    __asm cmp eax, 10
    __asm je inject_success_null
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm jmp dword ptr [indexCreateTrampoline]
inject_failure:
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm mov dword ptr [ecx + 4], 0
    __asm mov al, 0
    __asm add esp, 8
    __asm jmp dword ptr [indexCreateAfter]
inject_success_null:
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm mov dword ptr [ecx + 4], 0
    __asm mov al, 1
    __asm add esp, 8
    __asm jmp dword ptr [indexCreateAfter]
}

__declspec(naked) inline void IndexUploadBridge() {
    SDGO_PREVIEW_PROBE_SAVE_CONTEXT
    __asm push 6
    __asm call Decide
    __asm cmp eax, 11
    __asm je inject_lock_failure
    __asm cmp eax, 12
    __asm je inject_success_null
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm jmp dword ptr [indexUploadTrampoline]
inject_lock_failure:
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm mov edx, 11
    __asm jmp IndexUploadInjected
inject_success_null:
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm mov edx, 12
    __asm jmp IndexUploadInjected
}

__declspec(naked) inline void ModelLoadBridge() {
    SDGO_PREVIEW_PROBE_SAVE_CONTEXT
    __asm push 7
    __asm call Decide
    __asm cmp eax, 13
    __asm je inject_failure
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm jmp dword ptr [modelLoadTrampoline]
inject_failure:
    SDGO_PREVIEW_PROBE_RESTORE_CONTEXT
    __asm xor eax, eax
    __asm add esp, 8
    __asm jmp dword ptr [modelLoadAfter]
}

#undef SDGO_PREVIEW_PROBE_SAVE_CONTEXT
#undef SDGO_PREVIEW_PROBE_RESTORE_CONTEXT

} // namespace hangar_preview_guard_fault_probe_detail
