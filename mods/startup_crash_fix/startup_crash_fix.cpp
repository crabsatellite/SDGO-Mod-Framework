// startup_crash_fix - random crash at game start (gate loading screen) of this GOnline.exe build.
//
// While the game starts (CGameApp::SetState, SERVICE_LOGO) the gate loading screen
// (0x0090C320) picks a loading set at random (0x00904180: grid data type 5, one of the ids
// from 27102 on, as many as there are sets with a unit; srand(timeGetTime())) and keeps the
// picked id at +0x20E0. It then walks the loading sets from 27101 and shows each set without a
// unit and the picked set (gate.show_set_unit, 0x008FE380). For a set with a unit, that
// function looks the unit up in the unit data (grid data type 2, 0x0059D2B0), creates the
// unit's model at +0x207C, sets it up from the rest of the set, and reads the unit record's
// name at +0x264 without checking that the lookup found it. A set whose unit is not in this
// client's data therefore crashes the game now and then (access violation at 0x008FE610, read
// of address 0x264), whenever the random pick lands on it.
//
// The screen cannot do without the model: later (0x008FEE30, from 0x0090CE90, reached when
// logging in) it plays an animation of the set whose id is at +0x20E0 on the model at +0x207C,
// without checking either. So a set whose unit is missing is replaced by another set, as if
// the random pick had landed on that one: the nearest set before it whose unit is known, else
// the nearest one after it. The replacement's id goes to +0x20E0. A replacement before it has
// already been passed by the walk and is shown at once; one after it is shown by the walk when
// it gets there. Either way the screen shows exactly one unit, set up from that set. The first
// time, the loading sets whose unit is missing are logged.

#include <windows.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "sdgo/login_showcase_api.h"
#include "sdgo/sdgo_api.h"

#include "model_fit.h"

namespace
{
    using ShowSetUnitFn = void(__thiscall*)(void* screen, int arg, const uint8_t* set);
    using GridDataFn = void*(__cdecl*)();
    using GridFindFn = int(__thiscall*)(void* grid, const uint8_t** record, uint32_t id, int type, char log);
    using ModelUpdateFn = void(__thiscall*)(void* model);

    constexpr int kTypeUnit = 2;
    constexpr int kTypeLoadingSet = 5;
    constexpr uint32_t kFirstLoadingSet = 27101; // 0x69DD, where 0x00904180 starts counting
    constexpr uint32_t kMaxLoadingSets = 4096;
    constexpr size_t kSetId = 0x00;
    constexpr size_t kSetUnit = 0x05;            // u32, 0: no unit
    constexpr size_t kScreenPickedSet = 0x20E0;  // u32, id of the loading set whose unit the screen shows

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    ShowSetUnitFn g_original = nullptr;
    GridDataFn g_gridData = nullptr;
    GridFindFn g_gridFind = nullptr;
    ModelUpdateFn g_modelUpdate = nullptr;
    bool g_guardReady = false;
    bool g_listed = false;
    PVOID volatile g_showcaseChooser = nullptr;
    bool g_showcaseFaultLogged = false;
    bool g_showcaseInvalidLogged = false;
    bool g_showcaseFitFaultLogged = false;
    bool g_showcaseFitBoundsLogged = false;
    SRWLOCK g_observationLock = SRWLOCK_INIT;
    SdgoLoginShowcaseObservationV1 g_observation{};

    bool Finite(float value)
    {
        return std::isfinite(value);
    }

    void ObserveShowcase(void* screen,const uint8_t* set,uint32_t unit,uint32_t original)
    {
        AcquireSRWLockExclusive(&g_observationLock);
        g_observation.size=sizeof(g_observation);
        g_observation.unit_id=unit;
        g_observation.original_unit_id=original;
        g_observation.loading_set_id=*reinterpret_cast<const uint32_t*>(set);
        g_observation.screen=reinterpret_cast<uintptr_t>(screen);
        ++g_observation.completed_calls;
        ReleaseSRWLockExclusive(&g_observationLock);
        auto* model=*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(screen)+0x207C);
        if(model) {
            const float minX=*reinterpret_cast<float*>(model+0xA4);
            const float minY=*reinterpret_cast<float*>(model+0xA8);
            const float minZ=*reinterpret_cast<float*>(model+0xAC);
            const float maxX=*reinterpret_cast<float*>(model+0xB0);
            const float maxY=*reinterpret_cast<float*>(model+0xB4);
            const float maxZ=*reinterpret_cast<float*>(model+0xB8);
            const float scale=*reinterpret_cast<float*>(model+0x94);
            if(Finite(minX)&&Finite(minY)&&Finite(minZ)&&
               Finite(maxX)&&Finite(maxY)&&Finite(maxZ)&&Finite(scale)) {
                g_api->Log(g_mod,SDGO_LOG_INFO,
                           "native login showcase unit=%lu original=%lu set=%lu model=%p bounds=(%.3f,%.3f,%.3f)-(%.3f,%.3f,%.3f) scale=%.3f",
                           static_cast<unsigned long>(unit),static_cast<unsigned long>(original),
                           static_cast<unsigned long>(*reinterpret_cast<const uint32_t*>(set)),model,
                           minX,minY,minZ,maxX,maxY,maxZ,scale);
                return;
            }
        }
        g_api->Log(g_mod,SDGO_LOG_INFO,
                   "native login showcase unit=%lu original=%lu set=%lu model=%p bounds=unavailable",
                   static_cast<unsigned long>(unit),static_cast<unsigned long>(original),
                   static_cast<unsigned long>(*reinterpret_cast<const uint32_t*>(set)),model);
    }

    void FitShowcaseModel(void* screen,uint32_t unit,uint32_t original)
    {
        if(unit==original)return;
        __try {
            auto* model=*reinterpret_cast<uint8_t**>(
                static_cast<uint8_t*>(screen)+0x207C);
            if(!model)return;
            const sdgo::login_showcase_fit::Bounds bounds{
                *reinterpret_cast<float*>(model+0xA4),
                *reinterpret_cast<float*>(model+0xA8),
                *reinterpret_cast<float*>(model+0xAC),
                *reinterpret_cast<float*>(model+0xB0),
                *reinterpret_cast<float*>(model+0xB4),
                *reinterpret_cast<float*>(model+0xB8)
            };
            const float nativeScale=*reinterpret_cast<float*>(model+0x94);
            float fittedScale=nativeScale;
            if(!sdgo::login_showcase_fit::ReplacementScale(
                   bounds,nativeScale,fittedScale)) {
                if(!g_showcaseFitBoundsLogged) {
                    g_showcaseFitBoundsLogged=true;
                    g_api->Log(g_mod,SDGO_LOG_WARN,
                               "login showcase model bounds are invalid; native scale retained");
                }
                return;
            }
            if(fittedScale>=nativeScale-0.001f)return;

            auto** vtable=*reinterpret_cast<void***>(model);
            auto refresh=vtable
                ? reinterpret_cast<ModelUpdateFn>(vtable[7])
                : nullptr;
            if(!refresh || !g_modelUpdate)return;
            *reinterpret_cast<float*>(model+0x94)=fittedScale;
            *reinterpret_cast<float*>(model+0x98)=fittedScale;
            *reinterpret_cast<float*>(model+0x9C)=fittedScale;
            model[0xA0]=1;
            refresh(model);
            g_modelUpdate(model);
            g_api->Log(g_mod,SDGO_LOG_INFO,
                       "login showcase fitted replacement unit %lu from scale %.3f to %.3f",
                       static_cast<unsigned long>(unit),nativeScale,fittedScale);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            if(!g_showcaseFitFaultLogged) {
                g_showcaseFitFaultLogged=true;
                g_api->Log(g_mod,SDGO_LOG_WARN,
                           "login showcase model fitting raised an exception; native presentation retained");
            }
        }
    }

    uint32_t SetId(const uint8_t* set)
    {
        return *reinterpret_cast<const uint32_t*>(set + kSetId);
    }

    uint32_t SetUnit(const uint8_t* set)
    {
        return *reinterpret_cast<const uint32_t*>(set + kSetUnit);
    }

    uint32_t ShowcaseUnit()
    {
        auto chooser = reinterpret_cast<SdgoLoginShowcaseChooseUnitV1Fn>(
            InterlockedCompareExchangePointer(&g_showcaseChooser, nullptr, nullptr));
        if (!chooser)
            return 0;
        __try {
            return chooser();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (!g_showcaseFaultLogged) {
                g_showcaseFaultLogged = true;
                g_api->Log(g_mod, SDGO_LOG_WARN,
                           "login showcase chooser raised an exception; original loading selection retained");
            }
            return 0;
        }
    }

    bool UnitKnown(uint32_t unit)
    {
        const uint8_t* record = nullptr;
        return g_gridFind(g_gridData(), &record, unit, kTypeUnit, 0) != 0 && record != nullptr;
    }

    // nullptr past the last loading set: 0x00904180 stops at the first id without data.
    const uint8_t* FindSet(uint32_t id)
    {
        const uint8_t* set = nullptr;
        return g_gridFind(g_gridData(), &set, id, kTypeLoadingSet, 0) != 0 ? set : nullptr;
    }

    // The set has a unit, but not one in this client's unit data.
    bool UnitMissing(const uint8_t* set)
    {
        return SetUnit(set) != 0 && !UnitKnown(SetUnit(set));
    }

    // The loading sets in the order 0x00904180 walks them, with the ones whose unit is missing.
    void ListBrokenSets()
    {
        int sets = 0;
        int broken = 0;
        for (uint32_t id = kFirstLoadingSet; id < kFirstLoadingSet + kMaxLoadingSets; ++id) {
            const uint8_t* set = FindSet(id);
            if (!set)
                break;
            ++sets;
            if (UnitMissing(set)) {
                ++broken;
                g_api->Log(g_mod, SDGO_LOG_WARN, "loading set %lu: unit %lu is not in this client's unit data",
                           static_cast<unsigned long>(id), static_cast<unsigned long>(SetUnit(set)));
            }
        }
        g_api->Log(g_mod, SDGO_LOG_INFO, "%d loading sets, %d with a missing unit", sets, broken);
    }

    // The nearest set before the given one whose unit is known, else the nearest one after it.
    const uint8_t* Replacement(uint32_t id)
    {
        auto shows = [](const uint8_t* set) { return SetUnit(set) != 0 && !UnitMissing(set); };
        for (uint32_t other = id; other-- > kFirstLoadingSet;) {
            const uint8_t* set = FindSet(other);
            if (set && shows(set))
                return set;
        }
        for (uint32_t other = id + 1; other < kFirstLoadingSet + kMaxLoadingSets; ++other) {
            const uint8_t* set = FindSet(other);
            if (!set)
                break;
            if (shows(set))
                return set;
        }
        return nullptr;
    }

    // The set the game's function is called with: the set itself, or for a set whose unit is
    // missing its replacement; nullptr when the walk shows the replacement later.
    const uint8_t* Choose(uint8_t* screen, const uint8_t* set)
    {
        __try {
            if (!g_listed) {
                g_listed = true;
                ListBrokenSets();
            }
            if (!UnitMissing(set))
                return set;
            const uint8_t* other = Replacement(SetId(set));
            if (!other) {
                g_api->Log(g_mod, SDGO_LOG_WARN,
                           "loading set %lu picked: its unit %lu is missing and no set has a known unit; shown as the game would",
                           static_cast<unsigned long>(SetId(set)), static_cast<unsigned long>(SetUnit(set)));
                return set;
            }
            g_api->Log(g_mod, SDGO_LOG_INFO, "loading set %lu picked: its unit %lu is missing, set %lu (unit %lu) shown instead",
                       static_cast<unsigned long>(SetId(set)), static_cast<unsigned long>(SetUnit(set)),
                       static_cast<unsigned long>(SetId(other)), static_cast<unsigned long>(SetUnit(other)));
            const bool passed = SetId(other) < SetId(set);
            *reinterpret_cast<uint32_t*>(screen + kScreenPickedSet) = SetId(other);
            return passed ? other : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "exception while checking the loading set; shown as the game would");
            return set;
        }
    }

    bool ShowShowcaseUnit(void* screen, int arg, const uint8_t* set, uint32_t unit)
    {
        if (!unit || !UnitKnown(unit)) {
            if (unit && !g_showcaseInvalidLogged) {
                g_showcaseInvalidLogged = true;
                g_api->Log(g_mod, SDGO_LOG_WARN,
                           "login showcase selected unavailable unit %lu; original loading selection retained",
                           static_cast<unsigned long>(unit));
            }
            return false;
        }
        const uint32_t originalUnit = SetUnit(set);
        if (unit == originalUnit) {
            g_original(screen, arg, set);
            ObserveShowcase(screen,set,unit,originalUnit);
            return true;
        }

        // The native function consumes the complete 469-byte type-5 record
        // synchronously. It retains the model and picked set ID, not this pointer.
        // Later animations re-fetch the original record by that unchanged ID.
        std::array<uint8_t,469> presentation{};
        std::memcpy(presentation.data(),set,presentation.size());
        std::memcpy(presentation.data()+kSetUnit,&unit,sizeof(unit));
        g_original(screen,arg,presentation.data());
        FitShowcaseModel(screen,unit,originalUnit);
        ObserveShowcase(screen,set,unit,originalUnit);
        return true;
    }

    void __fastcall ShowSetUnit(void* screen, void* /*edx*/, int arg, const uint8_t* set)
    {
        if (set && SetUnit(set)) {
            const uint32_t unit = ShowcaseUnit();
            if (unit && ShowShowcaseUnit(screen, arg, set, unit))
                return;
        }
        if (const uint8_t* shown = Choose(static_cast<uint8_t*>(screen), set))
            g_original(screen, arg, shown);
    }

    template <typename Fn>
    bool Bind(Fn& out, const char* symbol)
    {
        out = reinterpret_cast<Fn>(g_api->Symbol(g_mod, symbol));
        return out != nullptr;
    }
}

extern "C" __declspec(dllexport)
int __cdecl SdgoRegisterLoginShowcaseChooserV1(SdgoLoginShowcaseChooseUnitV1Fn chooser)
{
    if (!chooser) {
        InterlockedExchangePointer(&g_showcaseChooser, nullptr);
        return 1;
    }
    // Optional presentation capabilities must never disable the original
    // missing-unit guard. Refuse expanded selection when fitting is unavailable.
    if (!g_guardReady || !g_modelUpdate)
        return 0;
    return InterlockedCompareExchangePointer(
               &g_showcaseChooser, reinterpret_cast<void*>(chooser), nullptr) == nullptr;
}

extern "C" __declspec(dllexport)
int __cdecl SdgoLoginShowcaseObserveV1(SdgoLoginShowcaseObservationV1* result)
{
    if(!result || result->size!=sizeof(*result))return 0;
    AcquireSRWLockShared(&g_observationLock);
    *result=g_observation;
    ReleaseSRWLockShared(&g_observationLock);
    if(!result->completed_calls || !result->screen)return 0;
    __try {
        auto* screen=reinterpret_cast<const uint8_t*>(result->screen);
        result->model=*reinterpret_cast<const uintptr_t*>(screen+0x207C);
        if(result->model)result->animation_handle=*reinterpret_cast<const uint32_t*>(result->model+0x400);
        return result->model!=0;
    } __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}

SDGO_MOD_ENTRY
{
    g_api = api;
    g_mod = mod;
    if (!Bind(g_gridData, "grid.data") ||
        !Bind(g_gridFind, "grid.find"))
        return 0;
    g_guardReady = api->HookCode(mod, "gate.show_set_unit", reinterpret_cast<void*>(&ShowSetUnit),
                                 reinterpret_cast<void**>(&g_original)) != 0;
    if (!g_guardReady)
        return 0;
    if (!Bind(g_modelUpdate, "model.commit"))
        api->Log(mod, SDGO_LOG_WARN,
                 "login showcase fitting unavailable; original startup crash protection remains active");
    return 1;
}
