#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/operator_model.h"
#include "sdgo/acquisition_sources_file.h"
#include "../../support/native_resource_overlay/native_resource_overlay.h"

namespace {
using namespace sdgo::operators;
using LoadFn = uint8_t(__thiscall*)(void*, const wchar_t*, int);
using DataFn = void*(__cdecl*)();
// Find returns AL, not a normalized 32-bit EAX value (native ret 0x10).
using FindFn = uint8_t(__thiscall*)(void*, void**, int, int, char);
using NewFn = void*(__cdecl*)(size_t);
using DeleteFn = void(__cdecl*)(void*);
struct Box { uint32_t type; void* data; };
struct Pair { uint32_t id; Box* box; };
struct InsertResult { void* node; uint8_t inserted; uint8_t padding[3]; };
using MakeFn = void*(__thiscall*)(void*, const Pair*);
using InsertFn = void(__thiscall*)(void*, InsertResult*, void*, char);
struct Entry {
    uint32_t id, category, source, avatar;
    std::wstring name, sprite, dialogue, intro;
    std::array<std::wstring,7> sprites;
    bool fixedName=false,resources=false,enabled=true;
    int missionParts=-1;
};
using ConvertFn = int(WINAPI*)(UINT,DWORD,LPCCH,int,LPWSTR,int);
ConvertFn g_convert = nullptr;
uintptr_t g_nameDecoder = 0;
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
LoadFn g_originalLoad = nullptr;
DataFn g_data = nullptr;
FindFn g_find = nullptr;
NewFn g_new = nullptr;
DeleteFn g_delete = nullptr;
MakeFn g_make = nullptr;
InsertFn g_insert = nullptr;
std::vector<Entry> g_entries;
sdgo::acquisition_sources_file::SourceFile g_acquisitionSources;
bool g_applied = false;
bool g_ready = false;
constexpr std::array<uint32_t, 4> kCategoryTemplateCandidates = {
    1210017, 1210012, 1210006, 1220008
};

bool FixedToken(const char* text, uint32_t* id)
{
    __try {
        if (!text || std::memcmp(text,"@SDGO:",6)) return false;
        uint32_t value=0;
        for (size_t i=6;i<11;++i) {
            if (text[i]<'0' || text[i]>'9') return false;
            value=value*10+static_cast<uint32_t>(text[i]-'0');
        }
        if (text[11]) return false;
        *id=value;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int WINAPI ConvertName(UINT page,DWORD flags,LPCCH text,int count,LPWSTR output,int capacity)
{
    const uintptr_t caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    uint32_t id=0;
    // Only the verified host's 108-byte CP874 decoder may resolve these tokens.
    // Hooking the game IAT preserves shop_labels' ownership of that decoder.
    if (caller>=g_nameDecoder && caller<g_nameDecoder+108 && page==874 && !flags && count==-1 &&
        FixedToken(text,&id)) {
        for (const Entry& entry:g_entries) {
            if (!entry.fixedName || entry.id!=id) continue;
            const int needed=static_cast<int>(entry.name.size()+1);
            if (!capacity) return needed;
            if (capacity<0 || !output) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
            if (capacity<needed) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
            std::memcpy(output,entry.name.c_str(),static_cast<size_t>(needed)*sizeof(wchar_t));
            return needed;
        }
    }
    return g_convert(page,flags,text,count,output,capacity);
}

bool ReadRecord(uint32_t id, int type, uint8_t* out, size_t size)
{
    __try {
        void* row = nullptr;
        if (!g_find(g_data(), &row, static_cast<int>(id), type, 0) || !row) return false;
        std::memcpy(out, row, size);
        return ReadId(out) == id;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadCategoryTemplate(std::array<uint8_t, kCategorySize>* out)
{
    for (const uint32_t id : kCategoryTemplateCandidates) {
        std::array<uint8_t, kCategorySize> candidate = {};
        if (!ReadRecord(id, 0x22, candidate.data(), candidate.size()) ||
            ReadId(candidate.data()) != id || candidate[4] != 0) continue;
        uint32_t ref = 0, option = 0;
        std::memcpy(&ref, candidate.data() + 105, sizeof(ref));
        std::memcpy(&option, candidate.data() + 110, sizeof(option));
        if (ref < 60001 || ref > 60999 || candidate[109] > 2 || option != 0) continue;
        *out = candidate;
        return true;
    }
    return false;
}

bool WriteExisting(uint32_t id, int type, const uint8_t* value, size_t size)
{
    __try {
        void* row = nullptr;
        if (!g_find(g_data(), &row, static_cast<int>(id), type, 0) || !row || ReadId(static_cast<uint8_t*>(row)) != id) return false;
        std::memcpy(row, value, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool RegisterRecord(uint32_t id, uint32_t type, const uint8_t* bytes, size_t size)
{
    // The game owns successful insertions. Its native ClearAllData releases the
    // payload, box and node with this same CRT; no cross-CRT allocations escape.
    void* payload = nullptr;
    Box* box = nullptr;
    __try {
        void* exists = nullptr;
        if (g_find(g_data(), &exists, static_cast<int>(id), static_cast<int>(type), 0))
            return exists && std::memcmp(exists, bytes, size) == 0;
        payload = g_new(size);
        box = static_cast<Box*>(g_new(sizeof(Box)));
        std::memcpy(payload, bytes, size);
        box->type = type;
        box->data = payload;
        const Pair pair = {id, box};
        void* node = g_make(g_data(), &pair);
        InsertResult result = {};
        g_insert(g_data(), &result, node, 0);
        if (result.inserted) return true;
        g_delete(payload);
        g_delete(box);
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Native allocation failure must not let an ABI exception cross into
        // the loader hook. Insertions are bounded by the validated catalog.
        return false;
    }
}

void Apply()
{
    std::map<uint32_t, std::array<uint8_t, kOperatorSize>> source;
    for (const Entry& e : g_entries) {
        for (uint32_t id : {e.source, e.avatar}) {
            if (source.count(id)) continue;
            auto& row = source[id];
            if (!ReadRecord(id, 0x2C, row.data(), row.size())) {
                g_api->Log(g_mod, SDGO_LOG_ERROR, "operator source %lu unavailable", static_cast<unsigned long>(id));
                return;
            }
        }
    }
    std::array<uint8_t, kCategorySize> categoryTemplate = {};
    if (!ReadCategoryTemplate(&categoryTemplate)) {
        g_api->Log(g_mod, SDGO_LOG_ERROR, "native operator item template unavailable");
        return;
    }
    for (const Entry& e : g_entries) {
        std::array<uint8_t,kOperatorSize> nativeExisting{};
        if(!e.enabled && ReadRecord(e.id,0x2C,nativeExisting.data(),nativeExisting.size()))continue;
        auto row = source.at(e.source);
        auto category = categoryTemplate;
        if (!PatchOperator(row.data(), row.size(), e.id, e.name, source.at(e.avatar).data(), e.sprite)) continue;
        if(e.enabled && e.missionParts>=0)row[kMissionPartsOffset]=static_cast<uint8_t>(e.missionParts);
        if(e.resources) {
            if(!Text(row.data()+105,50,e.dialogue) || !Text(row.data()+933,50,e.intro))continue;
            bool valid=true;
            for(size_t i=0;i<e.sprites.size();++i)
                valid=Text(row.data()+kAvatarOffsets[i],50,e.sprites[i])&&valid;
            if(!valid)continue;
        }
        std::array<uint8_t, kCategorySize> registered = {};
        const bool exists = ReadRecord(e.category, 0x22, registered.data(), registered.size());
        uint32_t ref = 0;
        if (exists) std::memcpy(&ref, registered.data() + 105, 4);
        if (!exists && !PatchCategory(category.data(), category.size(), e.category, e.id, e.name)) continue;
        if ((exists && ref != e.id) || (!exists && !RegisterRecord(e.category, 0x22, category.data(), category.size()))) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "operator item %lu conflicts with an existing record", static_cast<unsigned long>(e.category));
            continue;
        }
        std::array<uint8_t, kOperatorSize> existing = {};
        const bool success = ReadRecord(e.id, 0x2C, existing.data(), existing.size())
            ? WriteExisting(e.id, 0x2C, row.data(), row.size())
            : RegisterRecord(e.id, 0x2C, row.data(), row.size());
        g_api->Log(g_mod, success ? SDGO_LOG_INFO : SDGO_LOG_ERROR, "operator %lu catalog %s",
                   static_cast<unsigned long>(e.id), success ? "applied" : "failed");
    }
}

uint8_t __fastcall Load(void* self, void*, const wchar_t* filename, int type)
{
    const uint8_t result = g_originalLoad(self, filename, type);
    if (type == 0x2C && result && !g_applied) {
        g_applied = true;
        Apply();
    }
    return result;
}

bool ReadConfig(const wchar_t* dir)
{
    const std::wstring file = std::wstring(dir) + L"\\display.ini";
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    const unsigned version=GetPrivateProfileIntW(L"catalog",L"Version",0,file.c_str());
    if(version!=1 && version!=2)return false;
    std::vector<wchar_t> sections(65536);
    const DWORD count = GetPrivateProfileSectionNamesW(sections.data(), static_cast<DWORD>(sections.size()), file.c_str());
    if (count >= sections.size() - 2) return false;
    std::map<uint32_t, bool> seen;
    for (const wchar_t* section = sections.data(); *section; section += wcslen(section) + 1) {
        if (wcsncmp(section, L"operator:", 9)) continue;
        wchar_t* end = nullptr;
        const unsigned long id = wcstoul(section + 9, &end, 10);
        if (!end || *end || id < 60001 || id > 60999 || seen.count(id) || g_entries.size() >= 256) return false;
        seen[static_cast<uint32_t>(id)] = true;
        Entry e = {};
        e.id = static_cast<uint32_t>(id);
        if(version==2)e.enabled=GetPrivateProfileIntW(section,L"Enabled",1,file.c_str())!=0;
        e.category = GetPrivateProfileIntW(section, L"Category", 0, file.c_str());
        e.source = GetPrivateProfileIntW(section, L"Template", 0, file.c_str());
        e.avatar = GetPrivateProfileIntW(section, L"AvatarProfile", 0, file.c_str());
        const bool defaultId=e.id==60003 || e.id==60013 || e.id==60018 || e.id==60025 || e.id==60041;
        const int parts=static_cast<int>(GetPrivateProfileIntW(section,L"MissionParts",defaultId?1:-1,file.c_str()));
        if(version==1)e.missionParts=parts>0?1:-1;
        else {if(parts < -1 || parts>1)return false;e.missionParts=parts;}
        // Older Manager exports omit this optional field. Their default entries
        // still resolve the server's fixed-name tokens after a client DLL update.
        e.fixedName = GetPrivateProfileIntW(section,L"FixedName",defaultId?1:0,file.c_str())!=0;
        if (e.fixedName && e.id!=60003 && e.id!=60013 && e.id!=60018 && e.id!=60025 && e.id!=60041) return false;
        wchar_t name[100] = {}, sprite[100] = {}, nameHex[200] = {};
        DWORD n = GetPrivateProfileStringW(section, L"Name", L"", name, 100, file.c_str());
        const DWORD hexCount = GetPrivateProfileStringW(section, L"NameHex", L"", nameHex, 200, file.c_str());
        if (hexCount) {
            if (hexCount > 192 || hexCount % 4) return false;
            n = hexCount / 4;
            for (DWORD i = 0; i < n; ++i) {
                unsigned value = 0;
                for (DWORD byte = 0; byte < 2; ++byte) {
                    unsigned decoded = 0;
                    for (DWORD digit = 0; digit < 2; ++digit) {
                        const wchar_t c = nameHex[i*4+byte*2+digit];
                        const int v = c >= L'0' && c <= L'9' ? c-L'0' :
                                      c >= L'a' && c <= L'f' ? c-L'a'+10 :
                                      c >= L'A' && c <= L'F' ? c-L'A'+10 : -1;
                        if (v < 0) return false;
                        decoded = decoded*16 + static_cast<unsigned>(v);
                    }
                    value |= decoded << (byte*8);
                }
                if (!value || value < 32) return false;
                name[i] = static_cast<wchar_t>(value);
            }
            name[n] = 0;
        }
        const DWORD s = GetPrivateProfileStringW(section, L"AvatarSprite", L"", sprite, 100, file.c_str());
        if (!n || n > 48 || s >= 50 || e.category < 1200000 || e.category >= 1300000 ||
            e.source < 60001 || e.source > 60999 || e.avatar < 60001 || e.avatar > 60999) return false;
        e.name = name;
        e.sprite = sprite;
        if (!e.sprite.empty()) {
            const std::wstring expected = L"sdgo\\mods\\operator_catalog\\assets\\" + std::to_wstring(e.id) + L".spr";
            if (e.sprite != expected) return false;
        }
        if(version==2) {
            wchar_t value[128]{};
            const DWORD got=GetPrivateProfileStringW(section,L"Dialogue",L"\x1",value,128,file.c_str());
            e.resources=!(got==1 && value[0]==1);
            if(e.resources) {
                const std::wstring own=L"sdgo\\mods\\operator_catalog\\assets\\"+std::to_wstring(e.id)+L"\\";
                auto resource=[&](const wchar_t* key,const wchar_t* native,const wchar_t* suffix,std::wstring& output) {
                    const DWORD length=GetPrivateProfileStringW(section,key,L"\x1",value,128,file.c_str());
                    if(length>=50 || (length==1 && value[0]==1))return false;
                    output=value;if(output.empty())return true;
                    const std::wstring ending=suffix;
                    if(output.size()<ending.size() || output.substr(output.size()-ending.size())!=ending ||
                       output.find(L"..")!=std::wstring::npos || output.find(L':')!=std::wstring::npos)return false;
                    if(output.rfind(own,0)==0)return true;
                    const std::wstring prefix=native;
                    if(output.rfind(prefix,0)!=0)return false;
                    const std::wstring number=output.substr(prefix.size(),output.size()-prefix.size()-ending.size());
                    return !number.empty() && number.find_first_not_of(L"0123456789")==std::wstring::npos;
                };
                if(!resource(L"Dialogue",L"gdrs\\",L".gdt",e.dialogue) ||
                   !resource(L"IntroVoice",L"sdrs\\",L".zsd",e.intro))return false;
                for(size_t i=0;i<e.sprites.size();++i) {
                    const std::wstring key=L"Sprite"+std::to_wstring(i);
                    if(!resource(key.c_str(),L"sprs\\",L".spr",e.sprites[i]))return false;
                }
            }
        }
        g_entries.push_back(std::move(e));
    }
    return true;
}

void AddRequired(std::vector<std::wstring>& paths, const std::wstring& path)
{
    if (std::find(paths.begin(), paths.end(), path) == paths.end())
        paths.push_back(path);
}

void CollectRequiredBundles(
    std::map<uint32_t, std::vector<std::wstring>>& required)
{
    for (const Entry& entry : g_entries) {
        const std::wstring base =
            L"sdgo\\mods\\operator_catalog\\assets\\" +
            std::to_wstring(entry.id);
        if (!entry.sprite.empty()) {
            auto& paths = required[entry.id];
            AddRequired(paths, base + L".txr");
            for (uint32_t frame = 0; frame < 7; ++frame)
                AddRequired(paths, base + L"_" + std::to_wstring(frame) + L".spr");
        }
        if (!entry.resources)
            continue;
        const std::wstring prefix = base + L"\\";
        const auto collect = [&](const std::wstring& path) {
            if (!path.empty() && path.rfind(prefix, 0) == 0)
                AddRequired(required[entry.id], path);
        };
        collect(entry.dialogue);
        collect(entry.intro);
        for (const std::wstring& sprite : entry.sprites)
            collect(sprite);
    }
}

struct PreparedBundle
{
    std::wstring configName;
    sdgo::resource_overlay::BundleRegistration bundle;
};

bool RegisterResourceOverlays()
{
    std::map<uint32_t, std::vector<std::wstring>> required;
    CollectRequiredBundles(required);
    if (required.empty())
        return true;

    const wchar_t* modDirectory = g_api->ModDirectory(g_mod);
    if (!modDirectory || !*modDirectory) {
        g_api->Log(g_mod, SDGO_LOG_ERROR,
                   "operator resource directory is unavailable");
        return false;
    }

    std::vector<PreparedBundle> prepared;
    prepared.reserve(required.size());
    for (const auto& item : required) {
        PreparedBundle candidate;
        const std::wstring id = std::to_wstring(item.first);
        candidate.configName =
            L"assets\\" + id + L"\\resource-overlay.ini";
        const std::string owner = "operator_catalog:" +
                                  std::to_string(item.first);
        std::string error;
        if (!sdgo::resource_overlay::LoadBundleFiles(
                modDirectory, candidate.configName.c_str(), owner.c_str(),
                candidate.bundle, error)) {
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "sealed operator %lu resources rejected before publication: %s",
                       static_cast<unsigned long>(item.first), error.c_str());
            return false;
        }
        for (const std::wstring& path : item.second) {
            if (candidate.bundle.entries.find(path) !=
                candidate.bundle.entries.end())
                continue;
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "sealed operator %lu bundle is missing required resource %ls",
                       static_cast<unsigned long>(item.first), path.c_str());
            return false;
        }
        prepared.push_back(std::move(candidate));
    }

    using GetBroker = const SdgoResourceOverlayBrokerApiV1*(__cdecl*)();
    const SdgoResourceOverlayBrokerApiV1* shared = nullptr;
    const wchar_t* sharedModule = nullptr;
    for (const wchar_t* moduleName :
         {L"capsule_catalog.dll", L"unit_catalog.dll", L"map_catalog.dll"}) {
        HMODULE module = GetModuleHandleW(moduleName);
        if (!module)
            continue;
        auto getter = reinterpret_cast<GetBroker>(
            GetProcAddress(module, "SdgoGetResourceOverlayBrokerV1"));
        if (!getter) {
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "loaded %ls is incompatible: "
                       "SdgoGetResourceOverlayBrokerV1 is missing",
                       moduleName);
            return false;
        }
        const SdgoResourceOverlayBrokerApiV1* broker = getter();
        if (!broker)
            continue;
        if (broker->size < sizeof(SdgoResourceOverlayBrokerApiV1) ||
            broker->version != 1 || !broker->RegisterBundle) {
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "loaded %ls has an incompatible resource broker ABI",
                       moduleName);
            return false;
        }
        if (shared && shared != broker) {
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "multiple native resource brokers are active");
            return false;
        }
        shared = broker;
        sharedModule = moduleName;
    }

    if (shared) {
        for (const PreparedBundle& candidate : prepared) {
            char error[512] = {};
            if (!shared->RegisterBundle(
                    modDirectory, candidate.configName.c_str(),
                    candidate.bundle.owner.c_str(), error,
                    static_cast<uint32_t>(sizeof(error)))) {
                g_api->Log(g_mod, SDGO_LOG_ERROR,
                           "%ls resource broker rejected %s: %s",
                           sharedModule, candidate.bundle.owner.c_str(),
                           error[0] ? error : "unknown broker error");
                return false;
            }
        }
        g_api->Log(g_mod, SDGO_LOG_INFO,
                   "sealed operator resources registered with %ls",
                   sharedModule);
        return true;
    }

    auto& provider = sdgo::resource_overlay::LocalProvider();
    std::string error;
    for (const PreparedBundle& candidate : prepared) {
        if (!provider.RegisterTrustedBundle(candidate.bundle, error)) {
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "standalone operator resource broker rejected %s: %s",
                       candidate.bundle.owner.c_str(), error.c_str());
            return false;
        }
    }
    if (!provider.Start(g_api, g_mod, nullptr, nullptr, error)) {
        g_api->Log(g_mod, SDGO_LOG_ERROR,
                   "standalone operator resource broker failed to start: %s",
                   error.c_str());
        return false;
    }
    g_api->Log(g_mod, SDGO_LOG_INFO,
               "sealed operator resources registered with standalone resource broker");
    return true;
}
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl
SdgoAcquisitionSourcesV1(const SdgoAcquisitionRequestV1* request,
                         const SdgoAcquisitionSinkV1* sink)
{
    return g_ready ? g_acquisitionSources.Query(request, sink)
                   : SDGO_ACQUISITION_UNAVAILABLE;
}

SDGO_MOD_ENTRY
{
    g_ready = false;
    g_api = api;
    g_mod = mod;
    if (!api || api->version != SDGO_API_VERSION || !api->SettingInt(mod, "Enabled", 1)) return 0;
    std::string acquisitionError;
    g_acquisitionSources.Load(
        std::wstring(api->ModDirectory(mod)) + L"\\acquisition-sources.bin",
        sdgo::acquisition_sources_file::kOperatorProvider, &acquisitionError);
    if (g_acquisitionSources.status() == SDGO_ACQUISITION_INCOMPLETE)
        api->Log(mod, SDGO_LOG_WARN, "operator acquisition sources ignored: %s",
                 acquisitionError.c_str());
    g_data = reinterpret_cast<DataFn>(api->Symbol(mod, "operator.grid_data"));
    g_find = reinterpret_cast<FindFn>(api->Symbol(mod, "operator.grid_find"));
    g_make = reinterpret_cast<MakeFn>(api->Symbol(mod, "operator.grid_make_node"));
    g_insert = reinterpret_cast<InsertFn>(api->Symbol(mod, "operator.grid_insert"));
    g_new = reinterpret_cast<NewFn>(api->Symbol(mod, "operator.native_new"));
    g_delete = reinterpret_cast<DeleteFn>(api->Symbol(mod, "operator.native_delete"));
    if (!g_data || !g_find || !g_make || !g_insert || !g_new || !g_delete ||
        !ReadConfig(api->ModDirectory(mod))) return 0;
    if (g_entries.empty()) { api->Log(mod, SDGO_LOG_INFO, "no operator overrides configured"); g_ready = true; return 1; }
    if (!RegisterResourceOverlays()) return 0;
    for (const Entry& entry:g_entries) {
        if (!entry.fixedName) continue;
        g_nameDecoder=reinterpret_cast<uintptr_t>(api->Symbol(mod,"operator.native_name_decode"));
        if (!g_nameDecoder) g_nameDecoder=reinterpret_cast<uintptr_t>(api->Symbol(mod,"text.ansi_to_wide"));
        if (!g_nameDecoder || !api->HookImport ||
            !api->HookImport(mod,"kernel32.dll","MultiByteToWideChar",reinterpret_cast<void*>(&ConvertName),
                             reinterpret_cast<void**>(&g_convert))) return 0;
        break;
    }
    const int loaded = api->HookCode(mod, "operator.load_data",
                                     reinterpret_cast<void*>(&Load),
                                     reinterpret_cast<void**>(&g_originalLoad));
    g_ready = loaded != 0;
    return loaded;
}
