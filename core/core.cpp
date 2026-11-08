// sdgo/core.dll - host identity, verified symbols, hook ownership and mod loading.

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "hooks.h"
#include "log.h"
#include "ini_string.h"
#include "sdgo/sdgo_api.h"
#include "sdgo/room_commands.h"
#include "symbols.h"
#include "runtime_state.h"
#include "sdgo/mod_dependencies.h"

struct ModDependency {std::string id,minimum,capability;int capabilityVersion=1;};

struct SdgoMod
{
    std::string id;
    std::string name;
    std::string version;
    std::string category;
    std::string feature;
    std::wstring directory;
    std::wstring manifest;
    std::wstring dllPath;
    std::vector<std::string> hosts;
    std::vector<std::string> required;
    std::vector<ModDependency> requiredMods,optionalMods,conflicts;
    std::vector<std::string> configurationFiles;
    std::string configurationRevision;
    bool eligible=false,attempted=false,declarationValid=true;
    sdgo::SymbolTable symbols;
    int api = 0;
    int order = 100;
    bool enabled = true;
    HMODULE module = nullptr;
    bool active = false;
};

namespace
{
    SdgoApi g_api = {};
    sdgo::SymbolTable g_symbols;
    std::vector<SdgoMod*> g_mods; // process lifetime; never freed
    HMODULE g_host = nullptr;
    std::string g_hostKey;
    std::wstring g_root; // <game dir>\sdgo
    bool g_started = false;
    std::wstring g_executable;
    SRWLOCK g_runtimeLock=SRWLOCK_INIT;

    void WriteRuntimeState() noexcept {
        try {
        std::vector<sdgo::runtime::ModState> states;
        for(const auto* mod:g_mods)states.push_back({mod->id,mod->version,mod->configurationRevision,mod->configurationFiles,mod->enabled,mod->active});
        sdgo::runtime::Write(g_root,g_executable,g_hostKey,states);
        }catch(...){/* Status diagnostics must not interrupt a native process. */}
    }

    std::string Narrow(const std::wstring& text)
    {
        std::string out;
        for (wchar_t c : text)
            out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        return out;
    }

    std::wstring Widen(const char* text)
    {
        std::wstring out;
        for (; text && *text; ++text)
            out.push_back(static_cast<unsigned char>(*text));
        return out;
    }

    std::wstring IniString(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* def)
    {
        return sdgo::ReadIniString(path, section, key, def);
    }

    std::vector<std::string> SplitList(const std::wstring& text)
    {
        std::vector<std::string> items;
        std::string current;
        for (wchar_t c : text + L",") {
            if (c == L',') {
                size_t first = current.find_first_not_of(" \t");
                size_t last = current.find_last_not_of(" \t");
                if (first != std::string::npos)
                    items.push_back(current.substr(first, last - first + 1));
                current.clear();
            } else {
                current.push_back(c < 0x80 ? static_cast<char>(c) : '?');
            }
        }
        return items;
    }

    std::string ComputeHostKey(HMODULE host)
    {
        wchar_t path[MAX_PATH * 2] = {};
        GetModuleFileNameW(host, path, MAX_PATH * 2);
        std::wstring stem = path;
        size_t slash = stem.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            stem = stem.substr(slash + 1);
        size_t dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos)
            stem = stem.substr(0, dot);

        auto base = reinterpret_cast<BYTE*>(host);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        char suffix[32];
        sprintf_s(suffix, "-%08X-%08X", nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
        return Narrow(stem) + suffix;
    }

    const char* ModName(SdgoMod* mod) { return mod ? mod->id.c_str() : "?"; }

    const sdgo::Symbol* FindSymbol(SdgoMod* mod, const char* name)
    {
        return mod ? mod->symbols.FindWithFallback(name, g_symbols) : g_symbols.Find(name);
    }

    // ---- API exposed to mods ------------------------------------------------

    void Api_Log(SdgoMod* mod, int level, const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        sdgo::log::Write(ModName(mod), level, fmt, args);
        va_end(args);
    }

    const sdgo::Symbol* VerifiedSymbol(SdgoMod* mod, const char* name, const char* wantedKind)
    {
        const sdgo::Symbol* symbol = name ? FindSymbol(mod, name) : nullptr;
        if (!symbol) {
            sdgo::log::Writef(ModName(mod), SDGO_LOG_ERROR, "symbol %s is not in the symbol file", name ? name : "(null)");
            return nullptr;
        }
        if (!symbol->verified) {
            sdgo::log::Writef(ModName(mod), SDGO_LOG_ERROR, "symbol %s is not usable: %s", name, symbol->problem.c_str());
            return nullptr;
        }
        if (wantedKind && symbol->kind != wantedKind) {
            sdgo::log::Writef(ModName(mod), SDGO_LOG_ERROR, "symbol %s is kind=%s, expected %s", name,
                              symbol->kind.c_str(), wantedKind);
            return nullptr;
        }
        return symbol;
    }

    void* Api_Symbol(SdgoMod* mod, const char* name)
    {
        const sdgo::Symbol* symbol = VerifiedSymbol(mod, name, nullptr);
        return symbol ? reinterpret_cast<void*>(symbol->address) : nullptr;
    }

    int Api_HookCode(SdgoMod* mod, const char* name, void* detour, void** original)
    {
        const sdgo::Symbol* symbol = VerifiedSymbol(mod, name, "code");
        if (!symbol || !mod)
            return 0;
        std::string error;
        if (!sdgo::hooks::HookCode(mod->id, reinterpret_cast<void*>(symbol->address), detour, original, error,
                                   true)) {
            sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_ERROR, "hook %s failed: %s", name, error.c_str());
            return 0;
        }
        sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_INFO, "hooked code %s", name);
        return 1;
    }

    int Api_HookSite(SdgoMod* mod, const char* name, void* detour, void** original)
    {
        const sdgo::Symbol* symbol = VerifiedSymbol(mod, name, "site");
        if (!symbol || !mod) return 0;
        std::string error;
        if (!sdgo::hooks::HookCode(mod->id, reinterpret_cast<void*>(symbol->address), detour, original, error, true)) {
            sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_ERROR, "hook site %s failed: %s", name, error.c_str());
            return 0;
        }
        sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_INFO, "hooked site %s", name);
        return 1;
    }

    int Api_HookSlot(SdgoMod* mod, const char* name, void* detour, void** original)
    {
        const sdgo::Symbol* symbol = VerifiedSymbol(mod, name, "slot");
        if (!symbol || !mod)
            return 0;
        std::string error;
        if (!sdgo::hooks::HookSlot(mod->id, reinterpret_cast<void**>(symbol->address),
                                   reinterpret_cast<void*>(symbol->expectedTarget), detour, original, error)) {
            sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_ERROR, "hook %s failed: %s", name, error.c_str());
            return 0;
        }
        sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_INFO, "hooked slot %s", name);
        return 1;
    }

    int Api_HookImport(SdgoMod* mod, const char* dll, const char* function, void* detour, void** original)
    {
        if (!mod || !dll || !function)
            return 0;
        std::string error;
        if (!sdgo::hooks::HookImport(mod->id, g_host, dll, function, detour, original, error)) {
            sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_ERROR, "hook %s!%s failed: %s", dll, function, error.c_str());
            return 0;
        }
        sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_INFO, "hooked import %s!%s", dll, function);
        return 1;
    }

    int Api_HookSystemExport(SdgoMod* mod, const wchar_t* dll, const char* function, void* detour, void** original)
    {
        if (!mod || !dll || !dll[0] || wcspbrk(dll, L"\\/:") || !function || !function[0])
            return 0;
        wchar_t path[MAX_PATH] = {};
        UINT length = GetSystemDirectoryW(path, MAX_PATH);
        if (!length || length >= MAX_PATH || length + 1 + wcslen(dll) >= MAX_PATH)
            return 0;
        wcscat_s(path, L"\\");
        wcscat_s(path, dll);
        // Do not load a DLL on behalf of a mod, or resolve an app-local namesake.
        HMODULE module = GetModuleHandleW(path);
        void* target = module ? reinterpret_cast<void*>(GetProcAddress(module, function)) : nullptr;
        if (!target) {
            sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_ERROR, "system export %s!%s not loaded or missing",
                              Narrow(dll).c_str(), function);
            return 0;
        }
        std::string error;
        if (!sdgo::hooks::HookCode(mod->id, target, detour, original, error)) {
            sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_ERROR, "hook system export %s!%s failed: %s",
                              Narrow(dll).c_str(), function, error.c_str());
            return 0;
        }
        sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_INFO, "hooked system export %s!%s", Narrow(dll).c_str(), function);
        return 1;
    }

    int Api_SettingInt(SdgoMod* mod, const char* key, int def)
    {
        if (!mod || !key)
            return def;
        return static_cast<int>(GetPrivateProfileIntW(L"settings", Widen(key).c_str(), def, mod->manifest.c_str()));
    }

    uint32_t Api_SettingString(SdgoMod* mod, const char* key, const char* def, char* out, uint32_t outSize)
    {
        if (!out || outSize == 0)
            return 0;
        out[0] = '\0';
        if (!mod || !key)
            return 0;
        std::string value = Narrow(IniString(mod->manifest, L"settings", Widen(key).c_str(), Widen(def).c_str()));
        strncpy_s(out, outSize, value.c_str(), _TRUNCATE);
        return static_cast<uint32_t>(strlen(out));
    }

    const wchar_t* Api_ModDirectory(SdgoMod* mod)
    {
        return mod ? mod->directory.c_str() : L"";
    }

    uint32_t Api_ConfigurationRevision(SdgoMod* mod,char* out,uint32_t size) {
        if(!mod||!out||size<65)return 0;
        const auto value=sdgo::runtime::ConfigurationRevision(mod->directory,mod->configurationFiles);
        if(value.empty()){out[0]=0;return 0;}strncpy_s(out,size,value.c_str(),_TRUNCATE);return static_cast<uint32_t>(value.size());
    }
    int Api_ConfigurationApplied(SdgoMod* mod,const char* revision) {
        if(!mod||!revision||strlen(revision)!=64)return 0;
        auto current=sdgo::runtime::ConfigurationRevision(mod->directory,mod->configurationFiles);
        if(current.empty()||current!=revision)return 0;
        AcquireSRWLockExclusive(&g_runtimeLock);
        if(!mod->active){ReleaseSRWLockExclusive(&g_runtimeLock);return 0;}
        mod->configurationRevision.swap(current);WriteRuntimeState();ReleaseSRWLockExclusive(&g_runtimeLock);return 1;
    }

    // ---- mod discovery and loading -----------------------------------------

    SdgoMod* ReadManifest(const std::wstring& directory, const std::wstring& folderName)
    {
        auto* mod = new SdgoMod();
        mod->directory = directory;
        mod->manifest = directory + L"\\mod.ini";
        mod->id = Narrow(IniString(mod->manifest, L"mod", L"id", folderName.c_str()));
        mod->name = Narrow(IniString(mod->manifest, L"mod", L"name", folderName.c_str()));
        mod->version = Narrow(IniString(mod->manifest, L"mod", L"version", L"0"));
        mod->category = Narrow(IniString(mod->manifest, L"mod", L"category", L""));
        mod->feature = Narrow(IniString(mod->manifest, L"mod", L"feature", Widen(mod->id.c_str()).c_str()));
        mod->api = GetPrivateProfileIntW(L"mod", L"api", 0, mod->manifest.c_str());
        mod->order = GetPrivateProfileIntW(L"mod", L"order", 100, mod->manifest.c_str());
        mod->enabled = GetPrivateProfileIntW(L"mod", L"enabled", 1, mod->manifest.c_str()) != 0;
        mod->hosts = SplitList(IniString(mod->manifest, L"mod", L"hosts", L""));
        mod->required = SplitList(IniString(mod->manifest, L"mod", L"requires", L""));
        mod->configurationFiles=sdgo::runtime::ConfigurationFiles(mod->manifest);
        const auto enabledKey=IniString(mod->manifest,L"framework.dependencies",L"enabled_key",L"");
        if(!enabledKey.empty()) {
            const auto file=IniString(mod->manifest,L"framework.dependencies",L"enabled_file",L"mod.ini");
            if(file!=L"mod.ini"&&file!=L"config.ini")mod->declarationValid=false;
            else if(!GetPrivateProfileIntW(L"settings",enabledKey.c_str(),GetPrivateProfileIntW(L"framework.dependencies",L"enabled_default",1,mod->manifest.c_str()),(mod->directory+L"\\"+file).c_str()))mod->enabled=false;
        }
        const int dependencySchema=GetPrivateProfileIntW(L"framework.dependencies",L"schema",0,mod->manifest.c_str());
        if(dependencySchema>1)mod->declarationValid=false;
        const std::pair<const wchar_t*,std::vector<ModDependency>*> groups[]={{L"required",&mod->requiredMods},{L"optional",&mod->optionalMods},{L"conflicts",&mod->conflicts}};
        if(dependencySchema==1)for(const auto& group:groups)for(const auto& id:SplitList(IniString(mod->manifest,L"framework.dependencies",group.first,L""))) {
            const auto section=L"framework.dependency."+Widen(id.c_str());
            const auto when=IniString(mod->manifest,section.c_str(),L"when_key",L"");
            if(!when.empty()) {
                const auto file=IniString(mod->manifest,section.c_str(),L"when_file",L"mod.ini");
                if(file!=L"mod.ini"&&file!=L"config.ini"){mod->declarationValid=false;continue;}
                const auto expected=IniString(mod->manifest,section.c_str(),L"when_value",L"");
                const auto fallback=IniString(mod->manifest,section.c_str(),L"when_default",L"");
                if(IniString(mod->directory+L"\\"+file,L"settings",when.c_str(),fallback.c_str())!=expected)continue;
            }
            group.second->push_back({id,Narrow(IniString(mod->manifest,section.c_str(),L"minimum",L"")),Narrow(IniString(mod->manifest,section.c_str(),L"capability",L"")),static_cast<int>(GetPrivateProfileIntW(section.c_str(),L"capability_version",1,mod->manifest.c_str()))});
        }
        mod->dllPath = directory + L"\\" + IniString(mod->manifest, L"mod", L"dll", (folderName + L".dll").c_str());

        // sdgo.ini [mods] <id>=0 switches a mod off without touching its folder.
        const std::wstring global = g_root + L"\\sdgo.ini";
        if (GetPrivateProfileIntW(L"mods", Widen(mod->id.c_str()).c_str(), 1, global.c_str()) == 0)
            mod->enabled = false;
        // A feature may have several client/server mods. These switches can
        // disable the entire group without overriding a mod's own off switch.
        if (!mod->category.empty() &&
            GetPrivateProfileIntW(L"categories", Widen(mod->category.c_str()).c_str(), 1, global.c_str()) == 0)
            mod->enabled = false;
        if (GetPrivateProfileIntW(L"features", Widen(mod->feature.c_str()).c_str(), 1, global.c_str()) == 0)
            mod->enabled = false;
        return mod;
    }

    bool Eligible(SdgoMod* mod)
    {
        if (!mod->enabled) {
            sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_INFO, "disabled by configuration");
            return false;
        }
        if(!mod->declarationValid){sdgo::log::Writef(mod->id.c_str(),SDGO_LOG_ERROR,"unsupported runtime dependency declaration");return false;}
        if (mod->api < 1 || mod->api > SDGO_API_VERSION) {
            sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_ERROR, "needs API %d, core provides %d", mod->api, SDGO_API_VERSION);
            return false;
        }
        bool hostOk = false;
        for (const std::string& host : mod->hosts)
            hostOk = hostOk || host == "*" || _stricmp(host.c_str(), g_hostKey.c_str()) == 0;
        if (!hostOk) {
            sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_INFO, "not for host %s", g_hostKey.c_str());
            return false;
        }
        const std::wstring fragment = mod->directory + L"\\symbols.fragment.ini";
        if (GetFileAttributesW(fragment.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (!mod->symbols.Load(fragment, g_host, g_hostKey.c_str(), mod->id.c_str(), true)) {
                sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_ERROR,
                                  "symbols.fragment.ini does not match the running host");
                return false;
            }
            std::string error;
            if (!mod->symbols.ValidateOverlay(g_symbols, error)) {
                sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_ERROR,
                                  "symbols.fragment.ini rejected: %s", error.c_str());
                return false;
            }
            sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_INFO, "Mod symbols: %u verified",
                              static_cast<unsigned>(mod->symbols.VerifiedCount()));
        }
        bool symbolsOk = true;
        for (const std::string& name : mod->required) {
            const sdgo::Symbol* symbol = FindSymbol(mod, name.c_str());
            if (!symbol || !symbol->verified) {
                sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_ERROR, "required symbol %s %s", name.c_str(),
                                  symbol ? symbol->problem.c_str() : "missing from symbol file");
                symbolsOk = false;
            }
        }
        return symbolsOk;
    }

    int CallModLoad(SdgoModLoadFn entry, SdgoMod* mod, DWORD* exceptionCode)
    {
        __try {
            return entry(&g_api, mod);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *exceptionCode = GetExceptionCode();
            return 0;
        }
    }

    void LoadMod(SdgoMod* mod)
    {
        if (!mod->eligible)
            return;
        const auto before=sdgo::runtime::ConfigurationRevision(mod->directory,mod->configurationFiles);
        mod->module = LoadLibraryExW(mod->dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!mod->module) {
            sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_ERROR, "LoadLibrary failed (%lu)", GetLastError());
            return;
        }
        auto entry = reinterpret_cast<SdgoModLoadFn>(GetProcAddress(mod->module, "SdgoModLoad"));
        DWORD exceptionCode = 0;
        int ok = entry ? CallModLoad(entry, mod, &exceptionCode) : 0;
        if (!ok) {
            if (!entry)
                sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_ERROR, "no SdgoModLoad export");
            else if (exceptionCode)
                sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_ERROR, "crashed while loading (exception %08lX)", exceptionCode);
            else
                sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_WARN, "declined to load");
            sdgo::hooks::Rollback(mod->id);
            FreeLibrary(mod->module);
            mod->module = nullptr;
            return;
        }
        auto after=sdgo::runtime::ConfigurationRevision(mod->directory,mod->configurationFiles);
        AcquireSRWLockExclusive(&g_runtimeLock);mod->active = true;
        if(before==after)mod->configurationRevision.swap(after);
        ReleaseSRWLockExclusive(&g_runtimeLock);
        sdgo::log::Writef(mod->id.c_str(), SDGO_LOG_INFO, "loaded %s %s", mod->name.c_str(), mod->version.c_str());
    }

    void LoadMods()
    {
        const wchar_t* modRoots[] = {L"mods", L"owner-mods", L"local-mods"};
        for (const wchar_t* modRoot : modRoots) {
            WIN32_FIND_DATAW entry = {};
            const std::wstring root = g_root + L"\\" + modRoot;
            HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &entry);
            if (find == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || entry.cFileName[0] == L'.' ||
                    (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                const std::wstring directory = root + L"\\" + entry.cFileName;
                if (GetFileAttributesW((directory + L"\\mod.ini").c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                if (wcscmp(modRoot, L"owner-mods") == 0) {
                    wchar_t scope[32]{};
                    GetPrivateProfileStringW(L"mod", L"distribution", L"", scope, 32, (directory + L"\\mod.ini").c_str());
                    if (wcscmp(scope, L"owner") != 0) {
                        sdgo::log::Writef("core", SDGO_LOG_ERROR, "owner Mod directory requires explicit owner distribution");
                        continue;
                    }
                }
                SdgoMod* candidate = ReadManifest(directory, entry.cFileName);
                const bool duplicate = std::any_of(g_mods.begin(), g_mods.end(), [candidate](const SdgoMod* existing) {
                    return existing->id == candidate->id;
                });
                if (duplicate) {
                    sdgo::log::Writef(candidate->id.c_str(), SDGO_LOG_ERROR, "duplicate Mod identity; local copy skipped");
                    delete candidate;
                } else {
                    g_mods.push_back(candidate);
                }
            } while (FindNextFileW(find, &entry));
            FindClose(find);
        }

        std::stable_sort(g_mods.begin(), g_mods.end(), [](const SdgoMod* a, const SdgoMod* b) {
            return a->order != b->order ? a->order < b->order : a->id < b->id;
        });
        for(auto* mod:g_mods)mod->eligible=Eligible(mod);
        // The game's own code has not started yet, so the mods' hooks on it (HookCode) are
        // switched on together after the last mod has loaded; system export hooks stay
        // immediate, other threads may run that code. One by one they took about 70 ms each,
        // and GUpdate.exe gives the game 3 s to reach WinMain (it closes the "9YOU SDGO"
        // event the game opens there; without it the game exits silently).
        sdgo::hooks::BeginBatch();
        auto find=[](const std::string& id)->SdgoMod* {for(auto* m:g_mods)if(m->id==id)return m;return nullptr;};
        size_t remaining=g_mods.size();bool progress=true;
        while(remaining&&progress) {
            progress=false;
            for(auto* mod:g_mods) {
                if(mod->attempted)continue;
                bool wait=false,blocked=!mod->eligible;
                for(const auto& rule:mod->conflicts){const auto* peer=find(rule.id);if(peer&&peer->eligible){blocked=true;sdgo::log::Writef(mod->id.c_str(),SDGO_LOG_ERROR,"conflicts with %s",rule.id.c_str());}}
                const std::pair<const std::vector<ModDependency>*,bool> groups[]={{&mod->requiredMods,true},{&mod->optionalMods,false}};
                for(const auto& group:groups)for(const auto& rule:*group.first) {
                    const auto* peer=find(rule.id);
                    if(peer&&peer->eligible&&!peer->attempted){wait=true;continue;}
                    if(!peer||!peer->active){if(group.second){blocked=true;sdgo::log::Writef(mod->id.c_str(),SDGO_LOG_ERROR,"required Mod %s is not active",rule.id.c_str());}continue;}
                    const int capability=rule.capability.empty()?rule.capabilityVersion:GetPrivateProfileIntW(L"framework.capabilities",Widen(rule.capability.c_str()).c_str(),0,peer->manifest.c_str());
                    if(!sdgo::dependency::Compatible(peer->version,rule.minimum)||capability!=rule.capabilityVersion){blocked=true;sdgo::log::Writef(mod->id.c_str(),SDGO_LOG_ERROR,"Mod %s has an incompatible version or capability",rule.id.c_str());}
                }
                if(wait&&!blocked)continue;
                mod->attempted=true;--remaining;progress=true;if(!blocked)LoadMod(mod);
            }
        }
        for(auto* mod:g_mods)if(!mod->attempted)sdgo::log::Writef(mod->id.c_str(),SDGO_LOG_ERROR,"cyclic runtime Mod dependencies");
        size_t enabled = 0;
        std::string error;
        if (sdgo::hooks::EndBatch(&enabled, error))
            sdgo::log::Writef("core", SDGO_LOG_INFO, "%u code hooks switched on", static_cast<unsigned>(enabled));
        else {
            sdgo::log::Writef("core", SDGO_LOG_ERROR, "switching on the code hooks failed (%s)", error.c_str());
            AcquireSRWLockExclusive(&g_runtimeLock);for(auto* mod:g_mods)mod->active=false;ReleaseSRWLockExclusive(&g_runtimeLock);
        }
        AcquireSRWLockExclusive(&g_runtimeLock);WriteRuntimeState();ReleaseSRWLockExclusive(&g_runtimeLock);
    }
}

extern "C" __declspec(dllexport) int __cdecl SdgoCoreInit(const SdgoBoot* boot)
{
    if (g_started || !boot || boot->size < offsetof(SdgoBoot, process_initial_thread_id))
        return g_started ? 1 : 0;
    g_started = true;

    g_host = static_cast<HMODULE>(boot->host_module);
    wchar_t exePath[MAX_PATH * 2] = {};
    GetModuleFileNameW(g_host, exePath, MAX_PATH * 2);
    g_executable=exePath;
    std::wstring gameDir = exePath;
    gameDir = gameDir.substr(0, gameDir.find_last_of(L'\\'));
    g_root = gameDir + L"\\sdgo";

    const std::wstring global = g_root + L"\\sdgo.ini";
    int level = GetPrivateProfileIntW(L"core", L"log_level", SDGO_LOG_INFO, global.c_str());
    sdgo::log::Open(g_root + L"\\logs", level);

    g_hostKey = ComputeHostKey(g_host);
    sdgo::log::Writef("core", SDGO_LOG_INFO, "SDGO core started by %s loader in %s (host %s)",
                      boot->loader ? boot->loader : "?", Narrow(gameDir).c_str(), g_hostKey.c_str());

    if (g_symbols.Load(g_root + L"\\symbols\\" + Widen(g_hostKey.c_str()) + L".ini", g_host, g_hostKey.c_str()))
        sdgo::log::Writef("core", SDGO_LOG_INFO, "symbols: %u of %u verified",
                          static_cast<unsigned>(g_symbols.VerifiedCount()), static_cast<unsigned>(g_symbols.Count()));

    std::string error;
    if (!sdgo::hooks::Initialize(error)) {
        sdgo::log::Writef("core", SDGO_LOG_ERROR, "hook engine unavailable (%s); no mods loaded", error.c_str());
        return 0;
    }

    g_api.size = sizeof(SdgoApi);
    g_api.version = SDGO_API_VERSION;
    g_api.host_key = g_hostKey.c_str();
    g_api.host_base = g_host;
    g_api.Log = Api_Log;
    g_api.Symbol = Api_Symbol;
    g_api.HookCode = Api_HookCode;
    g_api.HookSlot = Api_HookSlot;
    g_api.HookImport = Api_HookImport;
    g_api.SettingInt = Api_SettingInt;
    g_api.SettingString = Api_SettingString;
    g_api.ModDirectory = Api_ModDirectory;
    g_api.ConfigurationRevision=Api_ConfigurationRevision;
    g_api.ConfigurationApplied=Api_ConfigurationApplied;
    g_api.HookSystemExport = Api_HookSystemExport;
    g_api.HookSite = Api_HookSite;
    g_api.FindRoomCommand = sdgo::room_commands::Parse;
    g_api.process_initial_thread_id =
        boot->size >= offsetof(SdgoBoot, process_initial_thread_id) + sizeof(boot->process_initial_thread_id)
            ? boot->process_initial_thread_id : 0;

    LoadMods();
    size_t active = 0;
    for (SdgoMod* mod : g_mods)
        active += mod->active ? 1 : 0;
    sdgo::log::Writef("core", SDGO_LOG_INFO, "%u of %u mods active", static_cast<unsigned>(active),
                      static_cast<unsigned>(g_mods.size()));
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
