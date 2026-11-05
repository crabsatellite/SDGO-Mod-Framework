// Public fixtures are neutral programs. Private recovered chunks are optional inputs.
#include "sdgo/native_script_registration.h"
#include "sdgo/unit_program_format.h"
#include "native_lua_image.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <cwchar>
#include <thread>
#include <memory>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

using namespace sdgo::scripts;
namespace {
lua_State* g_state = nullptr;
int g_loads = 0, g_originals = 0, g_closeRequests = 0, g_failOnLoad = 0, g_failures = 0;
bool g_originalFinished = false;
NativeLuaImage* g_nativeImage = nullptr;
bool g_missingSymbol = false;
struct Engine { lua_State* state; };
void Check(bool ok, const char* message) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", message); if (!ok) ++g_failures; }
int Dump(lua_State*, const void* p, std::size_t n, void* context) {
    auto& out = *static_cast<std::vector<std::uint8_t>*>(context); auto* first = static_cast<const std::uint8_t*>(p); out.insert(out.end(), first, first + n); return 0;
}
Program Compile(const std::string& resource, const char* text, std::vector<std::string> exports) {
    Program p; p.resource = resource; p.exports = std::move(exports); int top = lua_gettop(g_state);
    if (luaL_loadbuffer(g_state, text, std::strlen(text), "=neutral registration fixture") != 0 || lua_dump(g_state, Dump, &p.bytes) != 1) throw std::runtime_error("fixture compilation failed");
    lua_settop(g_state, top); p.sha256 = Sha256(p.bytes); return p;
}
void* __cdecl State(void*) { return g_state; }
int __cdecl Top(void* state) { return lua_gettop(static_cast<lua_State*>(state)); }
void __cdecl SetTop(void* state, int top) { lua_settop(static_cast<lua_State*>(state), top); }
void __cdecl Push(void* state, const char* s) { lua_pushstring(static_cast<lua_State*>(state), s); }
void __cdecl RawGet(void* state, int index) { lua_rawget(static_cast<lua_State*>(state), index); }
int __cdecl Type(void* state, int index) { return lua_type(static_cast<lua_State*>(state), index); }
const void* __cdecl Pointer(void* state, int index) { return lua_topointer(static_cast<lua_State*>(state), index); }
int __cdecl Run(void* state, const void* bytes, std::uint32_t size, const char* name) {
    ++g_loads; if (!g_originalFinished) throw std::runtime_error("registration ran before original root returned");
    if (g_failOnLoad && g_loads == g_failOnLoad) return 3;
    auto* L = static_cast<lua_State*>(state); int result = luaL_loadbuffer(L, static_cast<const char*>(bytes), size, name); if (!result) result = lua_pcall(L, 0, LUA_MULTRET, 0); return result;
}
void __cdecl Close(void*, const char*) { ++g_closeRequests; }
void __fastcall Original(void* engine, void*, NativeWideString*) { ++g_originals; Check(engine != nullptr, "original loader receives its engine"); g_originalFinished = true; }
NativeLuaCalls Calls() {
    NativeLuaCalls calls{State, Top, SetTop, Push, RawGet, Type, Pointer, Run};
    if (g_nativeImage) {
        calls.state = g_nativeImage->at<decltype(calls.state)>(0xa97ec0);
        calls.getTop = g_nativeImage->at<decltype(calls.getTop)>(0xa647a0);
        calls.setTop = g_nativeImage->at<decltype(calls.setTop)>(0xa647c0);
        calls.rawGet = g_nativeImage->at<decltype(calls.rawGet)>(0xa64ed0);
        calls.type = g_nativeImage->at<decltype(calls.type)>(0xa64900);
        calls.toPointer = g_nativeImage->at<decltype(calls.toPointer)>(0xa64c00);
    }
    return calls;
}
void* Symbol(SdgoMod*, const char* name) {
    auto calls = Calls();
    if (g_missingSymbol && std::string(name) == "lua.rawget") return nullptr;
    const char* names[] = {"lua_tinker.state", "lua.gettop", "lua.settop", "lua.pushstring", "lua.rawget", "lua.type", "lua.topointer", "lua.run_buffer"};
    void* values[] = {reinterpret_cast<void*>(calls.state), reinterpret_cast<void*>(calls.getTop), reinterpret_cast<void*>(calls.setTop), reinterpret_cast<void*>(calls.pushString), reinterpret_cast<void*>(calls.rawGet), reinterpret_cast<void*>(calls.type), reinterpret_cast<void*>(calls.toPointer), reinterpret_cast<void*>(calls.runBuffer)};
    for (unsigned i = 0; i < 8; ++i) if (std::string(name) == names[i]) return values[i];
    return nullptr;
}
int __cdecl WrongReadbackType(void* state, int index) { return g_loads ? LUA_TNUMBER : Type(state, index); }
NativeWideString Path(const wchar_t* path) { NativeWideString result = {}; result.value.pointer = path; result.length = static_cast<std::uint32_t>(std::wcslen(path)); result.capacity = 64; return result; }
void Reset() {
    if (g_state) lua_close(g_state); g_state = lua_open(); if (!g_state) throw std::runtime_error("Lua allocation failed");
    if (g_nativeImage) Calls().state(g_state);
    g_loads = g_originals = g_closeRequests = g_failOnLoad = 0; g_originalFinished = false;
    const char* source = "function original_fn() return 17 end"; if (luaL_loadbuffer(g_state, source, std::strlen(source), "=original fixture") || lua_pcall(g_state, 0, 0, 0)) throw std::runtime_error("original fixture setup failed");
}
NativeRegistration Adapter() {
    SdgoApi api = {}; api.size = sizeof(api); api.process_initial_thread_id = GetCurrentThreadId();
    return {Calls(), reinterpret_cast<OriginalScriptLoader>(&Original), ProcessInitialThreadId(&api), Close, nullptr};
}
int GlobalType(const char* name) { lua_getglobal(g_state, name); int type = lua_type(g_state, -1); lua_pop(g_state, 1); return type; }
int OriginalApiChild(const char* input) {
    wchar_t executable[32768] = {}; if (!GetModuleFileNameW(nullptr, executable, 32768)) throw std::runtime_error("fixture executable identity unavailable");
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, nullptr, 0); if (!length) throw std::runtime_error("fixture input path encoding");
    std::vector<wchar_t> path(static_cast<std::size_t>(length)); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, path.data(), length);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --native-api-child \"" + path.data() + L"\"";
    STARTUPINFOW startup = {}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES; startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE); startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) throw std::runtime_error("owned native replay child could not start");
    int result = 1;
    try {
        void* memory = VirtualAllocEx(process.hProcess, reinterpret_cast<void*>(0x400000), 0xc97000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        SIZE_T written = 0;
        if (memory != reinterpret_cast<void*>(0x400000) || !WriteProcessMemory(process.hProcess, memory, NativeLuaImage::kReservationCookie, sizeof(NativeLuaImage::kReservationCookie), &written) || written != sizeof(NativeLuaImage::kReservationCookie)) throw std::runtime_error("owned child source range reservation failed");
        if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) throw std::runtime_error("owned native replay child did not resume");
        if (WaitForSingleObject(process.hProcess, 30000) != WAIT_OBJECT_0) throw std::runtime_error("owned native replay child timed out");
        DWORD code = 1; if (!GetExitCodeProcess(process.hProcess, &code)) throw std::runtime_error("owned replay child result unavailable"); result = static_cast<int>(code);
    } catch (...) { TerminateProcess(process.hProcess, 124); WaitForSingleObject(process.hProcess, 5000); CloseHandle(process.hThread); CloseHandle(process.hProcess); throw; }
    CloseHandle(process.hThread); CloseHandle(process.hProcess); return result;
}
void Tests() {
    SdgoApi api = {}; api.size = sizeof(api); api.Symbol = Symbol; NativeLuaCalls bound; std::string missing;
    api.process_initial_thread_id = GetCurrentThreadId();
    Check(ProcessInitialThreadId(&api) == GetCurrentThreadId(), "process initial thread is read from the optional core ABI tail");
    api.size = static_cast<uint32_t>(offsetof(SdgoApi, process_initial_thread_id));
    Check(ProcessInitialThreadId(&api) == 0 && ProcessInitialThreadId(nullptr) == 0, "older/missing core never infers initial thread from current caller");
    api.size = sizeof(api); api.process_initial_thread_id = 0;
    Check(ProcessInitialThreadId(&api) == 0, "dynamic loader's unavailable identity remains zero");
    Check(BindNativeLuaCalls(&api, nullptr, bound, missing) && bound.complete(), "consumer resolves the complete verified framework symbol set");
    g_missingSymbol = true; Check(!BindNativeLuaCalls(&api, nullptr, bound, missing) && !bound.complete() && missing == "lua.rawget", "missing verified symbol cannot leave stale partial bindings"); g_missingSymbol = false;
    Reset(); auto p = Compile("scp/alpha_one.scp", "function alpha_One(x) return original_fn()+x end", {"alpha_One"}); Candidate alpha{"alpha", "v1", {p}};
    auto root = Path(L"scp\\0000.scp"), other = Path(L"scp\\0012.scp"); Engine engine{g_state}; auto adapter = Adapter();
    NativeRegistration unavailable(Calls(), reinterpret_cast<OriginalScriptLoader>(&Original), 0, Close, nullptr);
    const auto noThread = unavailable.onOriginalLoad(&engine, &root, alpha);
    Check(noThread.status == Status::BoundaryUnavailable && !g_originals && !g_loads, "unavailable initial thread refuses root registration before touching the native VM");
    lua_pushnumber(g_state, 99); int top = lua_gettop(g_state);
    auto r = adapter.onOriginalLoad(&engine, &other, alpha); Check(r.status == Status::BoundaryUnavailable && g_loads == 0 && g_originals == 1, "non-root loader never activates content");
    r = adapter.onOriginalLoad(&engine, &root, alpha); Check(r.active() && r.status == Status::Registered && g_loads == 1 && r.exports.size() == 1, "actual dumped closure registers after original root on same VM");
    Check(lua_gettop(g_state) == top, "original VM stack survives registration and export readback");
    lua_getglobal(g_state, "alpha_One"); lua_pushnumber(g_state, 2); Check(lua_pcall(g_state, 1, 1, 0) == 0 && lua_tonumber(g_state, -1) == 19, "registered function executes against preserved original globals"); lua_settop(g_state, top);
    r = adapter.onOriginalLoad(&engine, &root, alpha); Check(r.status == Status::AlreadyRegistered && g_loads == 1, "same owner revision verifies actual function identity without reexecution");
    auto beta = Candidate{"beta", "v1", {Compile("scp/beta_one.scp", "function beta_One() return 8 end", {"beta_One"})}};
    r = adapter.onOriginalLoad(&engine, &root, beta); Check(r.active() && GlobalType("alpha_One") == LUA_TFUNCTION && GlobalType("original_fn") == LUA_TFUNCTION, "separate owners coexist without overwriting original functions");
    auto changed = alpha; changed.revision = "v2"; r = adapter.onOriginalLoad(&engine, &root, changed); Check(r.status == Status::RestartRequired && g_loads == 2, "live owner replacement requires a new client VM lifetime");
    Result threaded; std::thread thread([&] { threaded = adapter.onOriginalLoad(&engine, &root, alpha); }); thread.join(); Check(threaded.status == Status::BoundaryUnavailable && g_loads == 2, "wrong thread is rejected before original VM access");

    Reset(); engine.state = g_state; auto collision = Adapter(); lua_pushnumber(g_state, 42); lua_setglobal(g_state, "alpha_One"); r = collision.onOriginalLoad(&engine, &root, alpha); Check(r.status == Status::NameConflict && !r.executionStarted && !g_loads && g_closeRequests == 0, "preexisting scalar export is rejected before any candidate execution");
    Reset(); engine.state = g_state; auto invalid = Adapter(); auto corrupt = alpha; corrupt.programs.push_back(Compile("scp/alpha_two.scp", "function alpha_Two() return 2 end", {"alpha_Two"})); corrupt.programs.back().sha256.assign(64, '0'); r = invalid.onOriginalLoad(&engine, &root, corrupt); Check(r.status == Status::InvalidCandidate && r.reason.find("hash") != std::string::npos && !g_loads && GlobalType("alpha_One") == LUA_TNIL, "entire candidate hash preflight precedes first chunk execution");
    auto sideEffect = Candidate{"alpha", "bad", {Compile("scp/alpha_bad.scp", "function alpha_One() end original_fn=12", {"alpha_One"})}}; r = invalid.onOriginalLoad(&engine, &root, sideEffect); Check(r.status == Status::InvalidCandidate && !g_loads && GlobalType("original_fn") == LUA_TFUNCTION, "top-level non-publication code cannot change original globals");
    auto capture = Candidate{"alpha", "capture", {Compile("scp/alpha_capture.scp", "local x=7 function alpha_One() return x end", {"alpha_One"})}}; Check(!Validate(capture).valid, "unimplemented captured registration closures are explicitly refused");
    auto undeclared = alpha; undeclared.programs[0].exports = {"alpha_Other"}; Check(!Validate(undeclared).valid, "declared exports must equal real chunk publications");
    auto traversal = alpha; traversal.programs[0].resource = "scp/alpha_../one.scp"; Check(!Validate(traversal).valid, "non-canonical resource paths are refused");
    auto truncated = alpha; truncated.programs[0].bytes.pop_back(); truncated.programs[0].sha256 = Sha256(truncated.programs[0].bytes); Check(!Validate(truncated).valid, "authenticated truncated chunks are refused by structural decoder");

    Reset(); engine.state = g_state; auto multiple = Adapter(); std::vector<Candidate> owners{alpha, beta};
    r = multiple.onOriginalLoadBatch(&engine, &root, owners);
    Check(r.active() && r.exports.size() == 2 && g_loads == 2 && g_originals == 1,
          "multiple owners register after one original root call in the same VM");
    r = multiple.onOriginalLoadBatch(&engine, &root, owners);
    Check(r.status == Status::AlreadyRegistered && !r.executionStarted && g_loads == 2,
          "same multi-owner revision reads back every existing function without reexecution");
    Reset(); engine.state = g_state; auto batchConflict = Adapter();
    lua_pushnumber(g_state, 42); lua_setglobal(g_state, "beta_One");
    r = batchConflict.onOriginalLoadBatch(&engine, &root, owners);
    Check(r.status == Status::NameConflict && !g_loads && GlobalType("alpha_One") == LUA_TNIL,
          "later owner name conflicts are found before the first owner executes");
    Reset(); engine.state = g_state; auto duplicateOwner = Adapter();
    r = duplicateOwner.onOriginalLoadBatch(&engine, &root, {alpha, alpha});
    Check(r.status == Status::InvalidCandidate && !g_loads,
          "duplicate owner declarations cannot partially publish a batch");
    Reset(); engine.state = g_state; auto multiFailure = Adapter(); g_failOnLoad = 2;
    r = multiFailure.onOriginalLoadBatch(&engine, &root, owners);
    Check(r.status == Status::FatalClientState && r.clientMustClose && g_closeRequests == 1 && g_loads == 2,
          "failure after another owner executed quarantines the whole candidate client");
    Check(GlobalType("alpha_One") == LUA_TFUNCTION && GlobalType("beta_One") == LUA_TNIL,
          "multi-owner partial state is retained as failure evidence rather than claimed rollback");

    Reset(); engine.state = g_state; auto fatal = Adapter(); Candidate batch = alpha; batch.programs.push_back(Compile("scp/alpha_two.scp", "function alpha_Two() return 2 end", {"alpha_Two"})); g_failOnLoad = 2;
    r = fatal.onOriginalLoad(&engine, &root, batch); Check(r.status == Status::FatalClientState && r.clientMustClose && g_closeRequests == 1 && g_loads == 2, "partial execution closes/quarantines the candidate client");
    Check(GlobalType("alpha_One") == LUA_TFUNCTION && GlobalType("alpha_Two") == LUA_TNIL, "partial VM side effects are retained as failure evidence, never claimed rolled back");
    r = fatal.onOriginalLoad(&engine, &root, alpha); Check(r.status == Status::FatalClientState && g_loads == 2 && g_closeRequests == 1, "poisoned client cannot perform another registration");
    Reset(); engine.state = nullptr; auto wrong = Adapter(); r = wrong.onOriginalLoad(&engine, &root, alpha); Check(r.status == Status::BoundaryUnavailable && !g_loads, "root engine identity must equal original VM state getter");
    Reset(); engine.state = g_state; auto badCalls = Calls(); badCalls.type = WrongReadbackType; NativeRegistration readback(badCalls, reinterpret_cast<OriginalScriptLoader>(&Original), GetCurrentThreadId(), Close, nullptr);
    r = readback.onOriginalLoad(&engine, &root, alpha); Check(r.status == Status::FatalClientState && r.clientMustClose && g_closeRequests == 1 && GlobalType("alpha_One") == LUA_TFUNCTION, "post-execution type mismatch closes the client and does not claim rollback");
    Reset(); engine.state = g_state; auto changedExport = Adapter(); r = changedExport.onOriginalLoad(&engine, &root, alpha); Check(r.active(), "arrange previously accepted owner"); lua_pushnumber(g_state, 3); lua_setglobal(g_state, "alpha_One"); r = changedExport.onOriginalLoad(&engine, &root, alpha); Check(r.status == Status::FatalClientState && r.clientMustClose && g_closeRequests == 1 && g_loads == 1, "changed previously registered export poisons the client without reexecution");
}
}

int main(int argc, char** argv) {
    std::unique_ptr<NativeLuaImage> native;
    try {
        if (argc == 3 && std::string(argv[1]) == "--unit-program-bundle") {
            sdgo::unit_catalog::Catalog catalog;
            sdgo::unit_catalog::Entry entry; entry.kind = 10; entry.id = 90001; entry.bytes.resize(1871);
            std::memcpy(entry.bytes.data(), &entry.id, 4);
            const wchar_t name[] = L"example_Init";
            std::memcpy(entry.bytes.data() + 1771, name, sizeof(name));
            catalog.records.push_back(entry);
            std::ifstream input(argv[2], std::ios::binary);
            if (!input) throw std::runtime_error("program container fixture missing");
            std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(input), {});
            sdgo::unit_catalog::ProgramBundle bundle; std::string error;
            Check(bundle.Parse(bytes, std::string(64, 'a'), catalog, error), error.empty() ? "Python-produced sidecar is consumed by the native parser" : error.c_str());
            Check(bundle.candidates.size() == 1, "one actual owner is retained");
            auto badRevision = bytes;
            const auto revisionStart = 64u + 4u + sdgo::unit_catalog::U32(bytes.data() + 64) + 4u;
            badRevision[revisionStart] = 'z';
            Check(!bundle.Parse(badRevision, std::string(64, 'a'), catalog, error) && error.find("revision") != std::string::npos,
                  "native sidecar revision independently requires a SHA-256");
            Check(!bundle.Parse(bytes, std::string(64, 'b'), catalog, error) && bundle.candidates.empty(), "different emitted catalog hash refuses and clears a previous parsed candidate");
            catalog.records[0].bytes[100] ^= 1;
            Check(!bundle.Parse(bytes, std::string(64, 'a'), catalog, error), "changed imported weapon refuses the initializer binding");
            catalog.records[0].bytes[100] ^= 1;
            bytes.push_back(0);
            Check(!bundle.Parse(bytes, std::string(64, 'a'), catalog, error), "extra native sidecar bytes are refused");
            return g_failures ? 1 : 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--native-api") return OriginalApiChild(argv[2]);
        if (argc == 3 && std::string(argv[1]) == "--native-api-child") { native = std::make_unique<NativeLuaImage>(argv[2], true); g_nativeImage = native.get(); }
        if (argc >= 8 && std::string(argv[1]) == "--private-candidate") {
            if ((argc - 4) % 4) throw std::runtime_error("candidate expects file/resource/hash/comma-separated-exports tuples");
            Reset(); Candidate c; c.owner = argv[2]; c.revision = argv[3];
            for (int i = 4; i < argc; i += 4) {
                Program p; p.resource = argv[i + 1]; p.sha256 = argv[i + 2]; std::ifstream file(argv[i], std::ios::binary); if (!file) throw std::runtime_error("candidate member unavailable"); p.bytes.assign(std::istreambuf_iterator<char>(file), {});
                std::string names = argv[i + 3]; std::size_t start = 0, end = 0;
                do { end = names.find(',', start); p.exports.push_back(names.substr(start, end == std::string::npos ? end : end - start)); start = end == std::string::npos ? end : end + 1; } while (end != std::string::npos);
                c.programs.push_back(std::move(p));
            }
            Engine engine{g_state}; auto adapter = Adapter(); auto root = Path(L"scp\\0000.scp"); auto r = adapter.onOriginalLoad(&engine, &root, c); Check(r.active(), r.reason.empty() ? "whole private candidate verified and registered" : r.reason.c_str()); Check(g_loads == static_cast<int>(c.programs.size()), "every declared private program executed in the same existing VM"); Check(g_closeRequests == 0, "private registration produced no partial-state failure"); std::printf("programs=%zu exports=%zu active=%s nativeGameClientExecuted=false\n", c.programs.size(), r.exports.size(), r.active() ? "true" : "false");
        } else if (argc >= 5 && std::string(argv[1]) == "--private-chunk") {
            Reset(); Program p; p.resource = "scp/" + std::string(argv[3]) + "_input.scp"; std::ifstream file(argv[2], std::ios::binary); p.bytes.assign(std::istreambuf_iterator<char>(file), {}); if (!file) throw std::runtime_error("private input could not be read"); p.sha256 = Sha256(p.bytes); for (int i = 4; i < argc; ++i) p.exports.emplace_back(argv[i]); Candidate c{argv[3], p.sha256, {p}}; Engine engine{g_state}; auto adapter = Adapter(); auto root = Path(L"scp\\0000.scp"); auto result = adapter.onOriginalLoad(&engine, &root, c); Check(result.active(), result.reason.empty() ? "private compiled chunk real-VM registration" : result.reason.c_str());
            std::printf("result=%d exports=%zu nativeGameClientExecuted=false\n", static_cast<int>(result.status), result.exports.size());
        } else Tests();
    } catch (const std::exception& e) { std::printf("FAIL exception: %s\n", e.what()); ++g_failures; }
    if (g_state) lua_close(g_state); return g_failures ? 1 : 0;
}
