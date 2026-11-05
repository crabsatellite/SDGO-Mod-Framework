#include <windows.h>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/unit_catalog_format.h"
#include "preview_runtime.h"
#include "skill_presentation.h"
#include "../../support/native_resource_overlay/native_resource_overlay.h"

namespace {
using namespace sdgo::unit_catalog;

struct NativeValue {
    uint32_t type;
    uint8_t* record;
};
struct NativePair {
    uint32_t id;
    NativeValue* value;
};
struct InsertResult {
    void* node;
    uint8_t inserted;
    uint8_t padding[3];
};
static_assert(sizeof(NativeValue) == 8 && sizeof(NativePair) == 8 && sizeof(InsertResult) == 8);

using GridFind = bool(__thiscall*)(void*, const uint8_t**, uint32_t, int, char);
using GridData = void*(__cdecl*)();
using LoadTable = void(__thiscall*)(void*, const wchar_t*, uint32_t);
using Allocate = void*(__cdecl*)(size_t);
using Release = void(__cdecl*)(void*);
using MakeNode = void*(__thiscall*)(void*, const NativePair*);
using InsertNode = void(__thiscall*)(void*, InsertResult*, void*, char);
using RegisterRecord = void(__thiscall*)(void*, const void*, uint32_t, uint32_t);
using MapFind = void(__thiscall*)(void*, void**, const uint32_t*);

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
Catalog g_catalog;
GridFind g_find = nullptr;
GridData g_data = nullptr;
LoadTable g_loadLastTable = nullptr;
Allocate g_allocate = nullptr;
Release g_release = nullptr;
MakeNode g_makeNode = nullptr;
InsertNode g_insertNode = nullptr;
RegisterRecord g_registerRecord = nullptr;
MapFind g_mapFind = nullptr;
bool g_attempted = false;
uint32_t g_addedThisAttempt = 0;

struct Allocation {
    uint8_t* record = nullptr;
    NativeValue* value = nullptr;
    void* node = nullptr;

    ~Allocation() {
        if (node) g_release(node);
        if (value) g_release(value);
        if (record) g_release(record);
    }

    void Published() {
        node = nullptr;
        value = nullptr;
        record = nullptr;
    }
};

bool HasAnyKey(void* grid, uint32_t id) {
    void* node = nullptr;
    g_mapFind(grid, &node, &id);
    return node && node != *reinterpret_cast<void**>(static_cast<uint8_t*>(grid) + 4);
}

bool NativeRecord(void* grid, uint32_t id, uint32_t kind, const uint8_t** record) {
    *record = nullptr;
    return g_find(grid, record, id, static_cast<int>(kind), 0) && *record;
}

const uint8_t* ReferenceRecord(void* grid, uint32_t id, uint32_t kind) {
    if (!id) return nullptr;
    const size_t imported = g_catalog.Find(id);
    if (imported != g_catalog.records.size() &&
        g_catalog.records[imported].kind == kind) {
        return g_catalog.records[imported].bytes.data();
    }
    const uint8_t* record = nullptr;
    return NativeRecord(grid, id, kind, &record) ? record : nullptr;
}

bool ReferenceExists(void* grid, uint32_t id, uint32_t kind) {
    return ReferenceRecord(grid, id, kind) != nullptr;
}

bool ValidateReferences(void* grid) {
    for (const auto& entry : g_catalog.records) {
        if (entry.kind == kUnitType) {
            const uint32_t transform = U32(entry.bytes.data() + kUnitTransformOffset);
            if (transform && !ReferenceExists(grid, transform, kUnitType)) {
                g_api->Log(g_mod, SDGO_LOG_ERROR, "unit %lu transform target %lu is unavailable",
                           static_cast<unsigned long>(entry.id), static_cast<unsigned long>(transform));
                return false;
            }
            for (const size_t offset : kUnitWeaponOffsets) {
                const uint32_t weapon = U32(entry.bytes.data() + offset);
                if (weapon && !ReferenceExists(grid, weapon, kWeaponType)) {
                    g_api->Log(g_mod, SDGO_LOG_ERROR, "unit %lu weapon %lu is unavailable",
                               static_cast<unsigned long>(entry.id), static_cast<unsigned long>(weapon));
                    return false;
                }
            }
            for (const size_t offset : kUnitSkillOffsets) {
                const uint32_t skill=U32(entry.bytes.data()+offset);
                if (skill && !ReferenceExists(grid,skill,kSkillType)) {
                    g_api->Log(g_mod,SDGO_LOG_ERROR,"unit %lu skill %lu is unavailable",
                        static_cast<unsigned long>(entry.id),static_cast<unsigned long>(skill));
                    return false;
                }
            }
        } else if (entry.kind == kWeaponType) {
            // Original CFunnel::Initialize (006409D0) resolves the type-10
            // weapon, then reads this DWORD and resolves the type-11 record.
            const uint32_t funnel=U32(entry.bytes.data()+kWeaponFunnelOffset);
            if (funnel && !ReferenceExists(grid,funnel,kFunnelType)) {
                g_api->Log(g_mod,SDGO_LOG_ERROR,"weapon %lu funnel %lu is unavailable",
                    static_cast<unsigned long>(entry.id),static_cast<unsigned long>(funnel));
                return false;
            }
        } else if (entry.kind == kSkillType) {
            // Original skill activation 0066C580 resolves this DWORD through
            // the same kind-13 lookup, including the linked record's link.
            const uint32_t linked=U32(entry.bytes.data()+kSkillLinkedSkillOffset);
            if (linked && !ReferenceExists(grid,linked,kSkillType)) {
                g_api->Log(g_mod,SDGO_LOG_ERROR,"skill %lu linked skill %lu is unavailable",
                    static_cast<unsigned long>(entry.id),static_cast<unsigned long>(linked));
                return false;
            }
        } else if (entry.kind == kItemCategoryType) {
            const uint32_t reference = U32(entry.bytes.data() + kItemCategoryRefOffset);
            const bool blueprint =
                entry.id / kItemCategoryClassDivisor == kBlueprintItemClass;
            const uint32_t kind = blueprint ? kBlueprintType : kUnitType;
            if (!ReferenceExists(grid, reference, kind)) {
                g_api->Log(g_mod, SDGO_LOG_ERROR,
                           "item category %lu %s %lu is unavailable",
                           static_cast<unsigned long>(entry.id),
                           blueprint ? "blueprint" : "unit",
                           static_cast<unsigned long>(reference));
                return false;
            }
        } else if (entry.kind == kBlueprintType) {
            const uint32_t output = U32(entry.bytes.data() + kBlueprintOutputOffset);
            if (!ReferenceExists(grid, output, kUnitType)) {
                g_api->Log(g_mod, SDGO_LOG_ERROR,
                           "blueprint %lu output unit %lu is unavailable",
                           static_cast<unsigned long>(entry.id),
                           static_cast<unsigned long>(output));
                return false;
            }
            const uint32_t count =
                U32(entry.bytes.data() + kBlueprintMaterialCountOffset);
            for (uint32_t slot = 0; slot < count; ++slot) {
                const uint32_t material =
                    U32(entry.bytes.data() + kBlueprintMaterialOffset +
                        slot * sizeof(uint32_t));
                if (!ReferenceExists(grid, material, kUnitType)) {
                    g_api->Log(g_mod, SDGO_LOG_ERROR,
                               "blueprint %lu material unit %lu is unavailable",
                               static_cast<unsigned long>(entry.id),
                               static_cast<unsigned long>(material));
                    return false;
                }
            }
        } else if (entry.kind == kUnitIndexType) {
            const uint32_t unit=U32(entry.bytes.data()+kUnitIndexRefOffset);
            if (!ReferenceExists(grid,unit,kUnitType)) {
                g_api->Log(g_mod,SDGO_LOG_ERROR,"unit index %lu references unavailable unit %lu",
                    static_cast<unsigned long>(entry.id),static_cast<unsigned long>(unit));
                return false;
            }
            const uint32_t blueprint =
                U32(entry.bytes.data() + kUnitIndexBlueprintRefOffset);
            if (blueprint) {
                const uint8_t* recipe =
                    ReferenceRecord(grid, blueprint, kBlueprintType);
                if (!recipe) {
                    g_api->Log(g_mod, SDGO_LOG_ERROR,
                               "unit index %lu references unavailable blueprint %lu",
                               static_cast<unsigned long>(entry.id),
                               static_cast<unsigned long>(blueprint));
                    return false;
                }
                const uint32_t output =
                    U32(recipe + kBlueprintOutputOffset);
                if (output != unit) {
                    g_api->Log(g_mod, SDGO_LOG_ERROR,
                               "unit index %lu unit %lu does not match blueprint %lu output %lu",
                               static_cast<unsigned long>(entry.id),
                               static_cast<unsigned long>(unit),
                               static_cast<unsigned long>(blueprint),
                               static_cast<unsigned long>(output));
                    return false;
                }
            }
        }
    }
    return true;
}

int InstallRecords(void* grid) {
    if (!grid || grid != g_data()) {
        g_api->Log(g_mod, SDGO_LOG_ERROR, "unit catalog grid identity mismatch");
        return -1;
    }
    g_api->Log(g_mod, SDGO_LOG_INFO, "native record context pid=%lu grid=%p",
               static_cast<unsigned long>(GetCurrentProcessId()), grid);

    std::vector<const Entry*> pending;
    int existing = 0;
    for (const auto& entry : g_catalog.records) {
        if (!HasAnyKey(grid, entry.id)) {
            pending.push_back(&entry);
            continue;
        }
        const uint8_t* native = nullptr;
        if (!NativeRecord(grid, entry.id, entry.kind, &native) ||
            std::memcmp(native, entry.bytes.data(), entry.bytes.size()) != 0) {
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "native grid key %lu conflicts with imported kind %lu; catalog aborted",
                       static_cast<unsigned long>(entry.id), static_cast<unsigned long>(entry.kind));
            return -1;
        }
        ++existing;
    }
    if (!ValidateReferences(grid)) return -1;

    // Nothing enters the native tree until every record, wrapper and node has
    // been allocated. The game owns successful insertions and releases them
    // with the same CRT during its normal table cleanup.
    std::vector<std::unique_ptr<Allocation>> prepared;
    prepared.reserve(pending.size());
    for (const Entry* entry : pending) {
        auto item = std::make_unique<Allocation>();
        item->record = static_cast<uint8_t*>(g_allocate(entry->bytes.size()));
        item->value = static_cast<NativeValue*>(g_allocate(sizeof(NativeValue)));
        if (!item->record || !item->value) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "native allocation failed while preparing unit catalog");
            return -1;
        }
        std::memcpy(item->record, entry->bytes.data(), entry->bytes.size());
        *item->value = NativeValue{entry->kind, item->record};
        const NativePair pair{entry->id, item->value};
        item->node = g_makeNode(grid, &pair);
        if (!item->node) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "native node allocation failed while preparing unit catalog");
            return -1;
        }
        prepared.push_back(std::move(item));
    }

    // Recheck every destination after all allocations and before the first
    // insertion. This leaves allocation and preflight failures at zero writes.
    for (const Entry* entry : pending) {
        if (HasAnyKey(grid, entry->id)) {
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "native grid key %lu changed while preparing unit catalog; catalog aborted",
                       static_cast<unsigned long>(entry->id));
            return -1;
        }
    }

    int added = 0;
    for (size_t index = 0; index < prepared.size(); ++index) {
        Allocation& item = *prepared[index];
        const Entry& entry = *pending[index];
        uint8_t* const raw = item.record;
        void* const node = item.node;
        item.node = nullptr; // native insertion consumes/frees the node on either result
        InsertResult result{};
        g_insertNode(grid, &result, node, 0);
        if (!result.inserted) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "native insertion failed for grid key %lu",
                       static_cast<unsigned long>(entry.id));
            return -1;
        }
        item.Published();
        ++g_addedThisAttempt;
        g_registerRecord(grid, raw, static_cast<uint32_t>(entry.bytes.size()), entry.kind);

        const uint8_t* found = nullptr;
        if (!NativeRecord(grid, entry.id, entry.kind, &found) || found != raw) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "native readback failed for grid key %lu",
                       static_cast<unsigned long>(entry.id));
            return -1;
        }
        ++added;
    }

    g_api->Log(g_mod, SDGO_LOG_INFO,
               "native unit catalog registered: %d added, %d already present, %u total",
               added, existing, static_cast<unsigned>(g_catalog.records.size()));
    return added;
}

int GuardedInstall(void* grid, DWORD* exceptionCode) {
    __try {
        return InstallRecords(grid);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        *exceptionCode = GetExceptionCode();
        return -1;
    }
}

const wchar_t* BaseName(const wchar_t* path) {
    const wchar_t* name = path ? path : L"";
    for (const wchar_t* cursor = name; *cursor; ++cursor)
        if (*cursor == L'\\' || *cursor == L'/') name = cursor + 1;
    return name;
}

void __fastcall LoadLast(void* self, void*, const wchar_t* file, uint32_t type) {
    g_loadLastTable(self, file, type);
    if (g_attempted || type != 0x0d || _wcsicmp(BaseName(file), L"0280.gdt") != 0) return;
    g_attempted = true;
    g_addedThisAttempt = 0;
    DWORD code = 0;
    if (GuardedInstall(self, &code) < 0) {
        if (!g_addedThisAttempt) {
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "native unit catalog registration failed before publication (%08lX); "
                       "this attempt added no records",
                       static_cast<unsigned long>(code));
            return;
        }
        g_api->Log(g_mod, SDGO_LOG_ERROR,
                   "native unit catalog registration failed after %lu records were published (%08lX); "
                   "terminating the client to prevent use of partial imported state",
                   static_cast<unsigned long>(g_addedThisAttempt),
                   static_cast<unsigned long>(code));
        const DWORD status = code ? code : 0xE0C00101u;
        TerminateProcess(GetCurrentProcess(), status);
        ExitProcess(status);
    }
}

template<class T>
bool Bind(T& target, const char* name) {
    target = reinterpret_cast<T>(g_api->Symbol(g_mod, name));
    return target != nullptr;
}

bool RegisterResourceOverlay() {
    using GetBroker = const SdgoResourceOverlayBrokerApiV1*(__cdecl*)();
    HMODULE capsule = GetModuleHandleW(L"capsule_catalog.dll");
    if (capsule) {
        auto getter = reinterpret_cast<GetBroker>(
            GetProcAddress(capsule, "SdgoGetResourceOverlayBrokerV1"));
        if (!getter) {
            g_api->Log(g_mod, SDGO_LOG_ERROR,
                       "loaded capsule_catalog.dll is incompatible: "
                       "SdgoGetResourceOverlayBrokerV1 is missing");
            return false;
        }
        const SdgoResourceOverlayBrokerApiV1* broker = getter();
        if (broker) {
            if (broker->size < sizeof(SdgoResourceOverlayBrokerApiV1) ||
                broker->version != 1 || !broker->RegisterBundle) {
                g_api->Log(g_mod, SDGO_LOG_ERROR,
                           "loaded capsule resource broker has an incompatible ABI");
                return false;
            }
            char error[512] = {};
            if (!broker->RegisterBundle(g_api->ModDirectory(g_mod),
                                        L"resource-overlay.ini", "unit_catalog",
                                        error, static_cast<uint32_t>(sizeof(error)))) {
                g_api->Log(g_mod, SDGO_LOG_ERROR,
                           "capsule resource broker rejected unit bundle: %s",
                           error[0] ? error : "unknown broker error");
                return false;
            }
            g_api->Log(g_mod, SDGO_LOG_INFO,
                       "sealed unit resources registered with capsule resource broker");
            return true;
        }
        // A new provider can remain loaded after declining its own bundle in a
        // non-core host. A null getter is explicitly not a claim on the hook.
    }

    std::string error;
    auto& provider = sdgo::resource_overlay::LocalProvider();
    if (!provider.RegisterBundleFiles(g_api->ModDirectory(g_mod),
                                      L"resource-overlay.ini", "unit_catalog", error) ||
        !provider.Start(g_api, g_mod, nullptr, nullptr, error)) {
        g_api->Log(g_mod, SDGO_LOG_ERROR,
                   "standalone unit resource broker rejected: %s", error.c_str());
        return false;
    }
    g_api->Log(g_mod, SDGO_LOG_INFO,
               "sealed unit resources registered with standalone resource broker");
    return true;
}
} // namespace

SDGO_MOD_ENTRY {
    g_api = api;
    g_mod = mod;
    if (!api || api->version != SDGO_API_VERSION || !api->SettingInt(mod, "Enabled", 1)) return 0;

    std::string error;
    if (!g_catalog.Load(std::wstring(api->ModDirectory(mod)) + L"\\catalog.bin", error)) {
        api->Log(mod, SDGO_LOG_ERROR, "%s", error.c_str());
        return 0;
    }
    if (!Bind(g_data, "grid.data") ||
        !Bind(g_find, "grid.find") ||
        !Bind(g_allocate, "crt.operator_new") ||
        !Bind(g_release, "crt.operator_delete") ||
        !Bind(g_makeNode, "grid.make_node") ||
        !Bind(g_insertNode, "grid.insert_node") ||
        !Bind(g_registerRecord, "grid.register_record") ||
        !Bind(g_mapFind, "grid.map_find")) {
        return 0;
    }
    if (!api->HookCode(mod, "unit_catalog.load_last_table",
                       reinterpret_cast<void*>(&LoadLast),
                       reinterpret_cast<void**>(&g_loadLastTable))) {
        return 0;
    }
    if (!sdgo::unit_preview::Attach(api, mod, g_catalog)) return 0;
    if (!sdgo::unit_skill_presentation::Attach(api, mod, g_catalog, g_data, g_find)) return 0;
    if (!RegisterResourceOverlay()) return 0;
    api->Log(mod, SDGO_LOG_INFO,
             "ready: %u validated native records; registration deferred to final data table",
             static_cast<unsigned>(g_catalog.records.size()));
    return 1;
}
