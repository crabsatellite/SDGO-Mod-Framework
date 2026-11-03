#include "sdgo/native_script_registration.h"
#include <windows.h>
#include <cstring>
#include <stdexcept>

namespace sdgo::scripts {
namespace {
constexpr int kGlobals = -10001;
bool IsOriginalRoot(const NativeWideString* path) {
    if (!path || path->length != 12 || path->capacity < path->length) return false;
    const auto* text = path->capacity < 8 ? path->value.small : path->value.pointer;
    if (!text || text[12] != 0) return false;
    return std::memcmp(text, L"scp\\0000.scp", 12 * sizeof(wchar_t)) == 0
        || std::memcmp(text, L"scp/0000.scp", 12 * sizeof(wchar_t)) == 0;
}
Result Denied(const char* reason) { Result result; result.status = Status::BoundaryUnavailable; result.reason = reason; return result; }
}

bool NativeLuaCalls::complete() const {
    return state && getTop && setTop && pushString && rawGet && type && toPointer && runBuffer;
}

bool BindNativeLuaCalls(const SdgoApi* api, SdgoMod* mod, NativeLuaCalls& out, std::string& missing) {
    out = {}; missing.clear();
    if (!api || api->size < offsetof(SdgoApi, Symbol) + sizeof(api->Symbol) || !api->Symbol) { missing = "verified framework symbol API unavailable"; return false; }
    const char* names[] = {"lua_tinker.state", "lua.gettop", "lua.settop", "lua.pushstring", "lua.rawget", "lua.type", "lua.topointer", "lua.run_buffer"};
    void* values[8] = {};
    for (unsigned i = 0; i < 8; ++i) { values[i] = api->Symbol(mod, names[i]); if (!values[i]) { if (!missing.empty()) missing += ", "; missing += names[i]; } }
    if (!missing.empty()) return false;
    out.state = reinterpret_cast<decltype(out.state)>(values[0]); out.getTop = reinterpret_cast<decltype(out.getTop)>(values[1]);
    out.setTop = reinterpret_cast<decltype(out.setTop)>(values[2]); out.pushString = reinterpret_cast<decltype(out.pushString)>(values[3]);
    out.rawGet = reinterpret_cast<decltype(out.rawGet)>(values[4]); out.type = reinterpret_cast<decltype(out.type)>(values[5]);
    out.toPointer = reinterpret_cast<decltype(out.toPointer)>(values[6]); out.runBuffer = reinterpret_cast<decltype(out.runBuffer)>(values[7]);
    return out.complete();
}

NativeRegistration::NativeRegistration(NativeLuaCalls calls, OriginalScriptLoader original,
    std::uint32_t gameMainThreadId, CloseCandidateClient closeClient, void* closeContext)
    : calls_(calls), original_(original), mainThread_(gameMainThreadId), close_(closeClient), closeContext_(closeContext) {}

Result NativeRegistration::onOriginalLoad(void* engine, NativeWideString* path, const Candidate& candidate) {
    if (registry_.poisoned()) { Result result; result.status = Status::FatalClientState; result.reason = "candidate client is quarantined"; result.clientMustClose = true; return result; }
    if (!original_) return Denied("original script loader is not bound");
    const bool root = IsOriginalRoot(path);
    if (root && (!mainThread_ || GetCurrentThreadId() != mainThread_)) return Denied("root loader is not on the verified game main thread");
    if (!root || inRoot_) { original_(engine, path); return Denied("not the outer original root completion"); }
    inRoot_ = true;
    try { original_(engine, path); } catch (...) { inRoot_ = false; throw; }
    inRoot_ = false;
    if (!calls_.complete() || !close_ || !engine) return Denied("verified native Lua calls / candidate close callback unavailable");
    // A81EF0 consumes *engine; the native getter exposes the same original state.
    void* engineVm = nullptr; std::memcpy(&engineVm, engine, sizeof(engineVm));
    if (!engineVm || calls_.state(nullptr) != engineVm) return Denied("root engine and native VM getter disagree");
    currentVm_ = engineVm; boundary_ = true;
    try { auto result = registry_.activate(candidate, *this); boundary_ = false; currentVm_ = nullptr; return result; }
    catch (...) { boundary_ = false; currentVm_ = nullptr; throw; }
}

bool NativeRegistration::atRootCompletionBoundary() const {
    return boundary_ && GetCurrentThreadId() == mainThread_ && calls_.state(nullptr) == currentVm_;
}
std::uintptr_t NativeRegistration::identity() const { return reinterpret_cast<std::uintptr_t>(currentVm_); }
int NativeRegistration::stackTop() const { return calls_.getTop(currentVm_); }
void NativeRegistration::restoreStack(int top) { calls_.setTop(currentVm_, top); }
int NativeRegistration::globalType(const std::string& name) {
    const int top = stackTop(); calls_.pushString(currentVm_, name.c_str()); calls_.rawGet(currentVm_, kGlobals);
    const int result = calls_.type(currentVm_, -1); restoreStack(top); return result;
}
std::uintptr_t NativeRegistration::functionIdentity(const std::string& name) {
    const int top = stackTop(); calls_.pushString(currentVm_, name.c_str()); calls_.rawGet(currentVm_, kGlobals);
    const auto result = reinterpret_cast<std::uintptr_t>(calls_.toPointer(currentVm_, -1)); restoreStack(top); return result;
}
bool NativeRegistration::execute(const Program& program, std::string& error) {
    if (!atRootCompletionBoundary()) { error = "native VM boundary changed"; return false; }
    const int status = calls_.runBuffer(currentVm_, program.bytes.data(), static_cast<std::uint32_t>(program.bytes.size()), program.resource.c_str());
    if (status) { error = "original load/protected-call status " + std::to_string(status); return false; }
    return true;
}
void NativeRegistration::failClosed(const std::string& reason) noexcept { close_(closeContext_, reason.c_str()); }
} // namespace sdgo::scripts
