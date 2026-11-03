#pragma once
#include "script_registration.h"
#include "sdgo_api.h"

namespace sdgo::scripts {
static_assert(sizeof(void*) == 4, "native script registration requires x86");

// The source adapter owns this ABI. Addresses come from the framework's verified
// symbol profile, never from a content manifest or arbitrary JSON address.
struct NativeLuaCalls {
    void* (__cdecl* state)(void*) = nullptr;
    int (__cdecl* getTop)(void*) = nullptr;
    void (__cdecl* setTop)(void*, int) = nullptr;
    void (__cdecl* pushString)(void*, const char*) = nullptr;
    void (__cdecl* rawGet)(void*, int) = nullptr;
    int (__cdecl* type)(void*, int) = nullptr;
    const void* (__cdecl* toPointer)(void*, int) = nullptr;
    int (__cdecl* runBuffer)(void*, const void*, std::uint32_t, const char*) = nullptr;
    bool complete() const;
};
// This does not install a hook. Hook lua.script_load through SdgoApi::HookCode
// and pass its returned original trampoline to NativeRegistration.
bool BindNativeLuaCalls(const SdgoApi* api, SdgoMod* mod, NativeLuaCalls& out, std::string& missing);

struct NativeWideString {
    union { wchar_t small[8]; const wchar_t* pointer; std::uint8_t storage[16]; } value;
    std::uint32_t length;
    std::uint32_t capacity;
};
static_assert(sizeof(NativeWideString) == 24, "original x86 native wide-string ABI");

using OriginalScriptLoader = void (__thiscall*)(void* engine, NativeWideString* path);
using CloseCandidateClient = void (__cdecl*)(void* context, const char* reason);

// This is the body used by an original-loader hook. It invokes the unmodified
// loader first, and opens the registration gate only while that exact root call
// has completed on the independently identified game main thread and same VM.
// Construct a fresh adapter for a fresh VM lifetime; this API offers no hot unload.
class NativeRegistration final : private Vm {
public:
    NativeRegistration(NativeLuaCalls calls, OriginalScriptLoader original,
        std::uint32_t gameMainThreadId, CloseCandidateClient closeClient, void* closeContext);
    Result onOriginalLoad(void* engine, NativeWideString* path, const Candidate& candidate);
    bool poisoned() const { return registry_.poisoned(); }
private:
    NativeLuaCalls calls_;
    OriginalScriptLoader original_;
    std::uint32_t mainThread_;
    CloseCandidateClient close_;
    void* closeContext_;
    Registry registry_;
    void* currentVm_ = nullptr;
    bool boundary_ = false;
    bool inRoot_ = false;
    bool atRootCompletionBoundary() const override;
    std::uintptr_t identity() const override;
    int stackTop() const override;
    void restoreStack(int top) override;
    int globalType(const std::string& name) override;
    std::uintptr_t functionIdentity(const std::string& name) override;
    bool execute(const Program& program, std::string& error) override;
    void failClosed(const std::string& reason) noexcept override;
};

} // namespace sdgo::scripts
