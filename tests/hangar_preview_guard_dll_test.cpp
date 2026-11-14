// Execute the production hangar-preview guard DLL through its x86 hooks and sites.
#include <windows.h>
#include <float.h>
#include <xmmintrin.h>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include "sdgo/sdgo_api.h"
#include "sdgo/hangar_preview_guard_observer.h"

namespace {

static_assert(sizeof(void*) == 4, "hangar preview ABI fixture requires the x86 build");

unsigned checks = 0;

void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

void CheckCase(bool value, const char* name, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(std::string(name) + ": " + message);
}

template<class T>
void Put(void* data, size_t offset, T value) {
    std::memcpy(static_cast<uint8_t*>(data) + offset, &value, sizeof(value));
}

enum class Scenario {
    Success,
    WrongSetterReturn,
    MissingVertex24,
    FailedVertex24,
    NullVertex24,
    MissingVertex32,
    FailedVertex32,
    NullVertex32,
    MissingIndex,
    FailedIndex,
    NullIndex,
    FalseModelResult,
    NullModelObject,
    NestedLoadFrame
};

enum Failure : uint32_t {
    NoFailure = 0,
    MissingVertex = 1,
    VertexLock = 2,
    MissingIndex = 3,
    IndexLock = 4,
    ModelLoad = 5
};

using RowHook = void(__fastcall*)(void*, void*, void*);
using SetHook = uint8_t(__fastcall*)(void*, void*, const void*, const void*);
using ParseHook = void*(__fastcall*)(void*, void*, void*, uint8_t);
using LoadHook = uint8_t(__fastcall*)(void*, void*, const void*, const void*);
using StatusHook = SdgoHangarPreviewGuardGetStatusV1Fn;

void* rowHook = nullptr;
void* setHook = nullptr;
void* parseHook = nullptr;
void* loadHook = nullptr;
void* vertex24BeforeHook = nullptr;
void* vertex24AfterHook = nullptr;
void* vertex32BeforeHook = nullptr;
void* vertex32AfterHook = nullptr;
void* indexBeforeHook = nullptr;
void* indexAfterHook = nullptr;
void* modelResultHook = nullptr;
void* rowResultHook = nullptr;
void* setterReturnAddress = nullptr;
void* hangarSetterReturnAddress = nullptr;
StatusHook statusHook = nullptr;
SdgoHangarPreviewGuardGetStatusV2Fn statusV2Hook = nullptr;
bool expectMainScope = false;

Scenario scenario = Scenario::Success;
std::array<uint8_t, 0x80> rowOwner{};
std::array<uint8_t, 0x20> rowRecord{};
std::array<uint8_t, 0x10> definition{};
std::array<uint8_t, 0x80> renderer{};
std::array<uint8_t, 0x20> parser{};
std::array<uint8_t, 0x20> loader{};
std::array<uint8_t, 0x20> nestedLoader{};
std::array<uint8_t, 0x20> source{};
std::array<uint8_t, 0x20> nestedSource{};
std::array<uint8_t, 0x60> vertexPart{};
std::array<uint8_t, 0x30> indexBuffer{};

struct FakeObject {
    void** table = nullptr;
};

FakeObject candidate{};
FakeObject root{};
FakeObject nestedRoot{};
FakeObject vertexResource{};
FakeObject indexResource{};
std::array<void*, 1> candidateTable{};
std::array<void*, 1> rootTable{};
std::array<void*, 13> vertexResourceTable{};
std::array<void*, 13> indexResourceTable{};

unsigned originalRows = 0;
unsigned originalSets = 0;
unsigned originalParses = 0;
unsigned originalLoads = 0;
unsigned candidateDeletes = 0;
unsigned rootDeletes = 0;
unsigned unlocks = 0;
unsigned rendererLeaves = 0;
unsigned sourceCloses = 0;
unsigned warnings = 0;
unsigned infos = 0;
void* observedRowOwner = nullptr;
void* observedRow = nullptr;
void* observedRenderer = nullptr;
void* observedCandidateDelete = nullptr;
void* observedRootDelete = nullptr;
void* observedUnlock = nullptr;
void* observedLeave = nullptr;
void* observedClose = nullptr;
const void* observedName = nullptr;
const void* observedAnimation = nullptr;
unsigned candidateDeleteFlag = 0;
unsigned rootDeleteFlag = 0;
bool parserComplete = false;
bool rootDeletedAfterParser = false;
bool outerLoadFinished = false;
bool closeOrderingValid = true;
bool stageCountersValid = true;
bool expectScopedStages = false;
unsigned previewStageSamples = 0;
unsigned parserStageSamples = 0;
unsigned closeStageSamples = 0;
unsigned originalLoadDepth = 0;
void* parseResultSeenByLoad = nullptr;
uint8_t scopedLoadResult = 0;
uint8_t nestedLoadResult = 0;
uint8_t observedSetterAl = 0;
bool siteAbiPreserved = true;
bool modelSiteAbiPreserved = true;
bool rowSiteAbiPreserved = true;
bool vertexLastErrorPreserved = true;
bool indexLastErrorPreserved = true;
bool rowLastErrorPreserved = true;
int lastIndexBeforeResult = -1;
int lastIndexAfterResult = -1;

volatile LONG vertex24BeforeNative = 0;
volatile LONG vertex24AfterNative = 0;
volatile LONG vertex24Skipped = 0;
volatile LONG vertex32BeforeNative = 0;
volatile LONG vertex32AfterNative = 0;
volatile LONG vertex32Skipped = 0;
volatile LONG indexBeforeNative = 0;
volatile LONG indexAfterNative = 0;
volatile LONG modelNative = 0;
volatile LONG modelCleanup = 0;
volatile LONG rowNative = 0;
volatile LONG rowThumbnail = 0;

constexpr uint32_t kEaxSeed = 0x10203040u;
constexpr uint8_t kSetterAl = 0x6Du;
constexpr uint32_t kEcxSeed = 0x11223344u;
constexpr uint32_t kEdxSeed = 0x22334455u;
constexpr uint32_t kEbxSeed = 0x33445566u;
constexpr uint32_t kEdiSeed = 0x55667788u;
constexpr uint32_t kFlagsSeed = 0x246u;
constexpr uint32_t kFlagsMask = 0xCD5u;
uint32_t seededMxcsr = 0x1F80u;
__declspec(align(16)) uint32_t xmmSeed0[4] = {
    0x3F800000u, 0x40000000u, 0x40400000u, 0x40800000u
};
__declspec(align(16)) uint32_t xmmSeed1[4] = {
    0x41200000u, 0x41A00000u, 0x41F00000u, 0x42200000u
};
__declspec(align(16)) uint32_t xmmClobber[4] = {
    0xDEADBEEFu, 0xA5A5A5A5u, 0x13572468u, 0x89ABCDEFu
};
__declspec(align(16)) uint8_t callerFp[512]{};
__declspec(align(16)) uint8_t siteFpBefore[512]{};
__declspec(align(16)) uint8_t siteFpAfter[512]{};
uint32_t siteBeforeEbp = 0;
uint32_t siteAfterEax = 0;
uint32_t siteAfterEcx = 0;
uint32_t siteAfterEdx = 0;
uint32_t siteAfterEbx = 0;
uint32_t siteAfterEsi = 0;
uint32_t siteAfterEdi = 0;
uint32_t siteAfterEbp = 0;
uint32_t siteAfterFlags = 0;

bool SameFpState() {
    return !std::memcmp(siteFpBefore, siteFpAfter, 8) &&
           !std::memcmp(siteFpBefore + 24, siteFpAfter + 24, 8) &&
           !std::memcmp(siteFpBefore + 32, siteFpAfter + 32, 128) &&
           !std::memcmp(siteFpBefore + 160, siteFpAfter + 160, 128);
}

void ObserveStage(uint32_t preview, uint32_t activeParser) {
    SdgoHangarPreviewGuardStatusV1 value{};
    value.size = sizeof(value);
    value.version = 1;
    const bool valid = statusHook && statusHook(&value) &&
        value.active_preview == preview && value.active_parser == activeParser;
    SdgoHangarPreviewGuardStatusV2 scope{sizeof(scope),2};
    const bool mainScopeValid=statusV2Hook&&statusV2Hook(&scope)&&
        scope.active_hangar==(expectMainScope&&preview?1u:0u);
    stageCountersValid = stageCountersValid && valid && mainScopeValid;
    if (preview) ++previewStageSamples;
    if (activeParser) ++parserStageSamples;
}

void ClobberFp() {
    _mm_setcsr(0x3F80u);
    __asm { fninit }
    __asm { fld1 }
    __asm { movaps xmm0, xmmword ptr [xmmClobber] }
    __asm { movaps xmm1, xmmword ptr [xmmClobber] }
}

__declspec(naked) void Vertex24BeforeNative() {
    __asm { pushfd }
    __asm { inc dword ptr [vertex24BeforeNative] }
    __asm { popfd }
    __asm { ret }
}

__declspec(naked) void Vertex24AfterNative() {
    __asm { pushfd }
    __asm { inc dword ptr [vertex24AfterNative] }
    __asm { popfd }
    __asm { ret }
}

__declspec(naked) void Vertex24Skip() {
    __asm { pushfd }
    __asm { inc dword ptr [vertex24Skipped] }
    __asm { popfd }
    __asm { ret }
}

__declspec(naked) void Vertex32BeforeNative() {
    __asm { pushfd }
    __asm { inc dword ptr [vertex32BeforeNative] }
    __asm { popfd }
    __asm { ret }
}

__declspec(naked) void Vertex32AfterNative() {
    __asm { pushfd }
    __asm { inc dword ptr [vertex32AfterNative] }
    __asm { popfd }
    __asm { ret }
}

__declspec(naked) void Vertex32Skip() {
    __asm { pushfd }
    __asm { inc dword ptr [vertex32Skipped] }
    __asm { popfd }
    __asm { ret }
}

__declspec(naked) void IndexBeforeNative() {
    __asm { pushfd }
    __asm { inc dword ptr [indexBeforeNative] }
    __asm { popfd }
    __asm { mov eax, 1 }
    __asm { pop edi }
    __asm { pop esi }
    __asm { pop ebx }
    __asm { pop ebp }
    __asm { ret 8 }
}

__declspec(naked) void IndexAfterNative() {
    __asm { pushfd }
    __asm { inc dword ptr [indexAfterNative] }
    __asm { popfd }
    __asm { mov eax, 1 }
    __asm { pop edi }
    __asm { pop esi }
    __asm { pop ebx }
    __asm { pop ebp }
    __asm { ret 8 }
}

__declspec(naked) void ModelNative() {
    __asm { pushfd }
    __asm { inc dword ptr [modelNative] }
    __asm { popfd }
    __asm { ret }
}

__declspec(naked) void ModelCleanup() {
    __asm { pushfd }
    __asm { inc dword ptr [modelCleanup] }
    __asm { popfd }
    __asm { ret }
}

__declspec(naked) void RowNative() {
    __asm { pushfd }
    __asm { inc dword ptr [rowNative] }
    __asm { popfd }
    __asm { ret }
}

__declspec(naked) void RowThumbnail() {
    __asm { pushfd }
    __asm { inc dword ptr [rowThumbnail] }
    __asm { popfd }
    __asm { ret }
}

// Query mode returns the exact instruction reached after the production setter
// detour returns. Run mode uses that same instruction as the native return site.
__declspec(naked) uintptr_t __stdcall InvokeSetter(void*, int) {
    __asm { push ebp }
    __asm { mov ebp, esp }
    __asm { cmp dword ptr [ebp+0Ch], 0 }
    __asm { jne query_address }
    __asm { mov ecx, [ebp+8] }
    __asm { xor edx, edx }
    __asm { push 22222222h }
    __asm { push 11111111h }
    __asm { call dword ptr [setHook] }
after_call:
    __asm { mov esp, ebp }
    __asm { pop ebp }
    __asm { ret 8 }
query_address:
    __asm { mov eax, offset after_call }
    __asm { mov esp, ebp }
    __asm { pop ebp }
    __asm { ret 8 }
}

__declspec(naked) uintptr_t __stdcall InvokeSetterWrong(void*) {
    __asm { push ebp }
    __asm { mov ebp, esp }
    __asm { mov ecx, [ebp+8] }
    __asm { xor edx, edx }
    __asm { push 22222222h }
    __asm { push 11111111h }
    __asm { call dword ptr [setHook] }
    __asm { mov esp, ebp }
    __asm { pop ebp }
    __asm { ret 4 }
}

__declspec(naked) uintptr_t __stdcall InvokeHangarSetter(void*, int) {
    // Keep this caller distinct under release-linker identical-code folding.
    __asm { nop }
    __asm { push ebp }
    __asm { mov ebp, esp }
    __asm { cmp dword ptr [ebp+0Ch], 0 }
    __asm { jne query_address }
    __asm { mov ecx, [ebp+8] }
    __asm { xor edx, edx }
    __asm { push 22222222h }
    __asm { push 11111111h }
    __asm { call dword ptr [setHook] }
after_call:
    __asm { mov esp, ebp }
    __asm { pop ebp }
    __asm { ret 8 }
query_address:
    __asm { mov eax, offset after_call }
    __asm { mov esp, ebp }
    __asm { pop ebp }
    __asm { ret 8 }
}

// The production vertex hooks read ESI, and their after sites also read EAX.
// Seed and compare the complete live integer state plus selected FXSAVE fields.
__declspec(naked) void __stdcall InvokeEsiSite(void*, void*, uint32_t) {
    __asm { push ebp }
    __asm { mov ebp, esp }
    __asm { push ebx }
    __asm { push esi }
    __asm { push edi }
    __asm { fxsave [callerFp] }
    __asm { fninit }
    __asm { fld1 }
    __asm { fldpi }
    __asm { ldmxcsr dword ptr [seededMxcsr] }
    __asm { movaps xmm0, xmmword ptr [xmmSeed0] }
    __asm { movaps xmm1, xmmword ptr [xmmSeed1] }
    __asm { fxsave [siteFpBefore] }
    __asm { mov eax, [ebp+10h] }
    __asm { mov ecx, 11223344h }
    __asm { mov edx, 22334455h }
    __asm { mov ebx, 33445566h }
    __asm { mov esi, [ebp+0Ch] }
    __asm { mov edi, 55667788h }
    __asm { mov dword ptr [siteBeforeEbp], ebp }
    __asm { push 246h }
    __asm { popfd }
    __asm { call dword ptr [ebp+8] }
    __asm { pushfd }
    __asm { pop dword ptr [siteAfterFlags] }
    __asm { mov dword ptr [siteAfterEax], eax }
    __asm { mov dword ptr [siteAfterEcx], ecx }
    __asm { mov dword ptr [siteAfterEdx], edx }
    __asm { mov dword ptr [siteAfterEbx], ebx }
    __asm { mov dword ptr [siteAfterEsi], esi }
    __asm { mov dword ptr [siteAfterEdi], edi }
    __asm { mov dword ptr [siteAfterEbp], ebp }
    __asm { fxsave [siteFpAfter] }
    __asm { fxrstor [callerFp] }
    __asm { pop edi }
    __asm { pop esi }
    __asm { pop ebx }
    __asm { pop ebp }
    __asm { ret 12 }
}

// The model and row result sites read EDI and preserve the incoming EAX.
__declspec(naked) void __stdcall InvokeEdiSite(void*, void*, uint32_t) {
    __asm { push ebp }
    __asm { mov ebp, esp }
    __asm { push ebx }
    __asm { push esi }
    __asm { push edi }
    __asm { fxsave [callerFp] }
    __asm { fninit }
    __asm { fld1 }
    __asm { fldpi }
    __asm { ldmxcsr dword ptr [seededMxcsr] }
    __asm { movaps xmm0, xmmword ptr [xmmSeed0] }
    __asm { movaps xmm1, xmmword ptr [xmmSeed1] }
    __asm { fxsave [siteFpBefore] }
    __asm { mov eax, [ebp+10h] }
    __asm { mov ecx, 11223344h }
    __asm { mov edx, 22334455h }
    __asm { mov ebx, 33445566h }
    __asm { mov esi, 44556677h }
    __asm { mov edi, [ebp+0Ch] }
    __asm { mov dword ptr [siteBeforeEbp], ebp }
    __asm { push 246h }
    __asm { popfd }
    __asm { call dword ptr [ebp+8] }
    __asm { pushfd }
    __asm { pop dword ptr [siteAfterFlags] }
    __asm { mov dword ptr [siteAfterEax], eax }
    __asm { mov dword ptr [siteAfterEcx], ecx }
    __asm { mov dword ptr [siteAfterEdx], edx }
    __asm { mov dword ptr [siteAfterEbx], ebx }
    __asm { mov dword ptr [siteAfterEsi], esi }
    __asm { mov dword ptr [siteAfterEdi], edi }
    __asm { mov dword ptr [siteAfterEbp], ebp }
    __asm { fxsave [siteFpAfter] }
    __asm { fxrstor [callerFp] }
    __asm { pop edi }
    __asm { pop esi }
    __asm { pop ebx }
    __asm { pop ebp }
    __asm { ret 12 }
}

// These frames intentionally match IndexFailure in the production DLL:
// saved EDI, ESI, EBX and EBP followed by two stdcall arguments.
__declspec(naked) int __stdcall InvokeIndexBefore(void*, uint32_t) {
    __asm { push ebp }
    __asm { mov ebp, esp }
    __asm { push ebx }
    __asm { push esi }
    __asm { push edi }
    __asm { mov esi, [ebp+8] }
    __asm { mov ebx, 33445566h }
    __asm { mov edi, 55667788h }
    __asm { jmp dword ptr [indexBeforeHook] }
}

__declspec(naked) int __stdcall InvokeIndexAfter(void*, long) {
    __asm { push ebp }
    __asm { mov ebp, esp }
    __asm { push ebx }
    __asm { push esi }
    __asm { push edi }
    __asm { mov esi, [ebp+8] }
    __asm { mov eax, [ebp+0Ch] }
    __asm { mov ebx, 33445566h }
    __asm { mov edi, 55667788h }
    __asm { jmp dword ptr [indexAfterHook] }
}

void RecordEsiPreservation(void* esi, uint32_t eax) {
    siteAbiPreserved = siteAbiPreserved &&
        siteAfterEax == eax &&
        siteAfterEcx == kEcxSeed &&
        siteAfterEdx == kEdxSeed &&
        siteAfterEbx == kEbxSeed &&
        siteAfterEsi == reinterpret_cast<uintptr_t>(esi) &&
        siteAfterEdi == kEdiSeed &&
        siteAfterEbp == siteBeforeEbp &&
        (siteAfterFlags & kFlagsMask) == (kFlagsSeed & kFlagsMask) &&
        SameFpState();
}

bool EdiPreserved(void* edi, uint32_t eax) {
    return siteAfterEax == eax &&
           siteAfterEcx == kEcxSeed &&
           siteAfterEdx == kEdxSeed &&
           siteAfterEbx == kEbxSeed &&
           siteAfterEsi == 0x44556677u &&
           siteAfterEdi == reinterpret_cast<uintptr_t>(edi) &&
           siteAfterEbp == siteBeforeEbp &&
           (siteAfterFlags & kFlagsMask) == (kFlagsSeed & kFlagsMask) &&
           SameFpState();
}

void* __fastcall DeleteCandidate(void* self, void*, unsigned deleting) {
    ++candidateDeletes;
    observedCandidateDelete = self;
    candidateDeleteFlag = deleting;
    SetLastError(0xD311CA7Eu);
    return self;
}

void* __fastcall DeleteRoot(void* self, void*, unsigned deleting) {
    ++rootDeletes;
    observedRootDelete = self;
    rootDeleteFlag = deleting;
    rootDeletedAfterParser = parserComplete;
    SetLastError(0xD311B007u);
    return self;
}

long __stdcall UnlockBuffer(void* self) {
    ++unlocks;
    observedUnlock = self;
    SetLastError(0xBAD0F00Du);
    ClobberFp();
    return 0;
}

void __fastcall LeaveRenderer(void* self, void*) {
    ++rendererLeaves;
    observedLeave = self;
    SetLastError(0xBAD0C5E0u);
    ClobberFp();
}

int __cdecl CloseSource(void* stream) {
    ++sourceCloses;
    observedClose = stream;
    ObserveStage(expectScopedStages ? 1u : 0u, 0);
    ++closeStageSamples;
    closeOrderingValid = closeOrderingValid &&
        outerLoadFinished && originalLoadDepth == 0 &&
        parserComplete && rootDeletes == 1 && candidateDeletes == 0;
    SetLastError(0xBAD0C105u);
    return 0;
}

uint8_t __fastcall OriginalLoad(
    void* self, void*, const void* name, const void* animation) {
    ++originalLoads;
    observedRenderer = self;
    observedName = name;
    observedAnimation = animation;
    const unsigned depth = ++originalLoadDepth;
    ObserveStage(expectScopedStages ? 1u : 0u, 0);
    if (scenario == Scenario::NestedLoadFrame && depth == 1) {
        nestedLoadResult = reinterpret_cast<LoadHook>(loadHook)(
            nestedLoader.data(), nullptr, name, animation);
    }
    void* stream = depth > 1 ? static_cast<void*>(nestedSource.data())
                             : static_cast<void*>(source.data());
    void* parsed = reinterpret_cast<ParseHook>(parseHook)(
        parser.data(), nullptr, stream, 1);
    parseResultSeenByLoad = parsed;
    const uint8_t result = parsed ? 0x7Bu : 0u;
    if (depth == 1) outerLoadFinished = true;
    --originalLoadDepth;
    return result;
}

void PrepareVertex(unsigned stride, bool resource, bool mapping) {
    vertexPart.fill(0);
    const size_t base = stride == 24 ? 4 : 0x28;
    if (resource) Put(vertexPart.data(), base + 4, &vertexResource);
    if (mapping) Put(vertexPart.data(), base + 0x18, reinterpret_cast<void*>(0x51525354u));
}

void RunVertex(unsigned stride, bool resource, int result, bool mapping, bool runAfter) {
    PrepareVertex(stride, resource, mapping);
    void* before = stride == 24 ? vertex24BeforeHook : vertex32BeforeHook;
    void* after = stride == 24 ? vertex24AfterHook : vertex32AfterHook;
    InvokeEsiSite(before, vertexPart.data(), kEaxSeed);
    RecordEsiPreservation(vertexPart.data(), kEaxSeed);
    if (!runAfter) return;
    const DWORD error = GetLastError();
    InvokeEsiSite(after, vertexPart.data(), static_cast<uint32_t>(result));
    RecordEsiPreservation(vertexPart.data(), static_cast<uint32_t>(result));
    vertexLastErrorPreserved =
        vertexLastErrorPreserved && GetLastError() == error;
}

void PrepareIndex(bool resource, bool mapping) {
    indexBuffer.fill(0);
    if (resource) Put(indexBuffer.data(), 4, &indexResource);
    if (mapping) Put(indexBuffer.data(), 0x18, reinterpret_cast<void*>(0x61626364u));
}

void RunIndex(bool resource, long result, bool mapping, bool runAfter) {
    PrepareIndex(resource, mapping);
    lastIndexBeforeResult = InvokeIndexBefore(indexBuffer.data(), 0xCAFEBABEu);
    if (!runAfter) return;
    const DWORD error = GetLastError();
    lastIndexAfterResult = InvokeIndexAfter(indexBuffer.data(), result);
    indexLastErrorPreserved =
        indexLastErrorPreserved && GetLastError() == error;
}

void RunCleanParserSites() {
    RunVertex(24, true, 0, true, true);
    RunVertex(32, true, 0, true, true);
    RunIndex(true, 0, true, true);
}

void* __fastcall OriginalParse(void*, void*, void*, uint8_t) {
    ++originalParses;
    ObserveStage(expectScopedStages ? 1u : 0u,
                 expectScopedStages ? 1u : 0u);
    parserComplete = false;
    switch (scenario) {
    case Scenario::Success:
    case Scenario::FalseModelResult:
        RunCleanParserSites();
        break;
    case Scenario::WrongSetterReturn:
    case Scenario::MissingVertex24:
        RunVertex(24, false, 0, false, false);
        break;
    case Scenario::FailedVertex24:
        RunVertex(24, true, static_cast<int>(0x80004005u), true, true);
        break;
    case Scenario::NullVertex24:
        RunVertex(24, true, 0, false, true);
        break;
    case Scenario::MissingVertex32:
        RunVertex(32, false, 0, false, false);
        break;
    case Scenario::FailedVertex32:
        RunVertex(32, true, static_cast<int>(0x80004005u), true, true);
        break;
    case Scenario::NullVertex32:
        RunVertex(32, true, 0, false, true);
        break;
    case Scenario::MissingIndex:
        RunIndex(false, 0, false, false);
        break;
    case Scenario::FailedIndex:
        RunIndex(true, -1, true, true);
        break;
    case Scenario::NullIndex:
        RunIndex(true, 0, false, true);
        break;
    case Scenario::NullModelObject:
        break;
    case Scenario::NestedLoadFrame:
        if (originalLoadDepth > 1)
            RunCleanParserSites();
        else
            RunVertex(24, false, 0, false, false);
        break;
    }
    parserComplete = true;
    return originalLoadDepth > 1 ? static_cast<void*>(&nestedRoot)
                                 : static_cast<void*>(&root);
}

uint32_t ModelResultValue() {
    return scenario == Scenario::FalseModelResult ? 0xA5A50000u : 0xA5A50001u;
}

uint8_t __fastcall OriginalSet(
    void* self, void*, const void* name, const void* animation) {
    ++originalSets;
    ObserveStage(expectScopedStages ? 1u : 0u, 0);
    observedRenderer = self;
    observedName = name;
    observedAnimation = animation;
    Put(self, 0x34, &candidate);
    if (scenario == Scenario::NullModelObject) {
        scopedLoadResult = reinterpret_cast<LoadHook>(loadHook)(
            nullptr, nullptr, name, animation);
    } else {
        scopedLoadResult = reinterpret_cast<LoadHook>(loadHook)(
            loader.data(), nullptr, name, animation);
    }
    const uint32_t result = ModelResultValue();
    InvokeEdiSite(modelResultHook, self, result);
    modelSiteAbiPreserved = modelSiteAbiPreserved && EdiPreserved(self, result);
    return kSetterAl;
}

void __fastcall OriginalRow(void* self, void*, void* row) {
    ++originalRows;
    observedRowOwner = self;
    observedRow = row;
    const uintptr_t setterResult =
        scenario == Scenario::WrongSetterReturn
            ? InvokeSetterWrong(renderer.data())
            : InvokeSetter(renderer.data(), 0);
    observedSetterAl = static_cast<uint8_t>(setterResult);
    const DWORD error = GetLastError();
    InvokeEdiSite(rowResultHook, row, 0x66778899u);
    rowSiteAbiPreserved =
        rowSiteAbiPreserved && EdiPreserved(row, 0x66778899u);
    rowLastErrorPreserved =
        rowLastErrorPreserved && GetLastError() == error;
}

void Log(SdgoMod*, int level, const char*, ...) {
    if (level == SDGO_LOG_WARN) ++warnings;
    if (level == SDGO_LOG_INFO) ++infos;
    SetLastError(0xBAD0106u);
}

const char* missingSymbol = nullptr;

void* Symbol(SdgoMod*, const char* key) {
    if (missingSymbol && !std::strcmp(key, missingSymbol)) return nullptr;
    if (!std::strcmp(key, "preview_guard.row_set_return")) return setterReturnAddress;
    if (!std::strcmp(key, "preview_guard.hangar_set_return")) return hangarSetterReturnAddress;
    if (!std::strcmp(key, "preview_guard.close_source"))
        return reinterpret_cast<void*>(&CloseSource);
    if (!std::strcmp(key, "preview_guard.leave_renderer"))
        return reinterpret_cast<void*>(&LeaveRenderer);
    if (!std::strcmp(key, "preview_guard.vertex24_continue"))
        return reinterpret_cast<void*>(&Vertex24Skip);
    if (!std::strcmp(key, "preview_guard.vertex32_continue"))
        return reinterpret_cast<void*>(&Vertex32Skip);
    if (!std::strcmp(key, "preview_guard.model_cleanup"))
        return reinterpret_cast<void*>(&ModelCleanup);
    if (!std::strcmp(key, "preview_guard.row_thumbnail"))
        return reinterpret_cast<void*>(&RowThumbnail);
    return nullptr;
}

int HookCode(SdgoMod*, const char* key, void* detour, void** original) {
    if (!std::strcmp(key, "preview_guard.row_apply")) {
        rowHook = detour;
        *original = reinterpret_cast<void*>(&OriginalRow);
        return 1;
    }
    if (!std::strcmp(key, "preview_guard.set_model")) {
        setHook = detour;
        *original = reinterpret_cast<void*>(&OriginalSet);
        return 1;
    }
    if (!std::strcmp(key, "preview_guard.load_model")) {
        loadHook = detour;
        *original = reinterpret_cast<void*>(&OriginalLoad);
        return 1;
    }
    if (!std::strcmp(key, "preview_guard.parse")) {
        parseHook = detour;
        *original = reinterpret_cast<void*>(&OriginalParse);
        return 1;
    }
    return 0;
}

int HookSite(SdgoMod*, const char* key, void* detour, void** original) {
    if (!std::strcmp(key, "preview_guard.vertex24_before")) {
        vertex24BeforeHook = detour;
        *original = reinterpret_cast<void*>(&Vertex24BeforeNative);
        return 1;
    }
    if (!std::strcmp(key, "preview_guard.vertex24_after")) {
        vertex24AfterHook = detour;
        *original = reinterpret_cast<void*>(&Vertex24AfterNative);
        return 1;
    }
    if (!std::strcmp(key, "preview_guard.vertex32_before")) {
        vertex32BeforeHook = detour;
        *original = reinterpret_cast<void*>(&Vertex32BeforeNative);
        return 1;
    }
    if (!std::strcmp(key, "preview_guard.vertex32_after")) {
        vertex32AfterHook = detour;
        *original = reinterpret_cast<void*>(&Vertex32AfterNative);
        return 1;
    }
    if (!std::strcmp(key, "preview_guard.index_before")) {
        indexBeforeHook = detour;
        *original = reinterpret_cast<void*>(&IndexBeforeNative);
        return 1;
    }
    if (!std::strcmp(key, "preview_guard.index_after")) {
        indexAfterHook = detour;
        *original = reinterpret_cast<void*>(&IndexAfterNative);
        return 1;
    }
    if (!std::strcmp(key, "preview_guard.model_result")) {
        modelResultHook = detour;
        *original = reinterpret_cast<void*>(&ModelNative);
        return 1;
    }
    if (!std::strcmp(key, "preview_guard.row_result")) {
        rowResultHook = detour;
        *original = reinterpret_cast<void*>(&RowNative);
        return 1;
    }
    return 0;
}

SdgoHangarPreviewGuardStatusV1 Snapshot() {
    SdgoHangarPreviewGuardStatusV1 value{};
    value.size = sizeof(value);
    value.version = 1;
    Check(statusHook && statusHook(&value) && value.size == sizeof(value) &&
              value.version == 1,
          "V1 observer returns the exact live ABI");
    return value;
}

uint32_t Delta(uint32_t before, uint32_t after) {
    return after - before;
}

void ResetFixture(Scenario next, uint32_t unit, uint16_t slot = 0) {
    scenario = next;
    rowOwner.fill(0);
    rowRecord.fill(0);
    definition.fill(0);
    renderer.fill(0);
    parser.fill(0);
    loader.fill(0);
    nestedLoader.fill(0);
    source.fill(0);
    nestedSource.fill(0);
    vertexPart.fill(0);
    indexBuffer.fill(0);
    Put(definition.data(), 0, unit);
    Put(rowRecord.data(), 4, definition.data());
    Put(rowRecord.data(), 8, slot);
    candidate.table = candidateTable.data();
    root.table = rootTable.data();
    nestedRoot.table = rootTable.data();
    vertexResource.table = vertexResourceTable.data();
    indexResource.table = indexResourceTable.data();
    originalRows = originalSets = originalParses = originalLoads = 0;
    candidateDeletes = rootDeletes = unlocks = rendererLeaves = sourceCloses = 0;
    warnings = 0;
    observedRowOwner = observedRow = observedRenderer = nullptr;
    observedCandidateDelete = observedRootDelete = observedUnlock = observedLeave = nullptr;
    observedClose = nullptr;
    observedName = observedAnimation = nullptr;
    candidateDeleteFlag = rootDeleteFlag = 0;
    parserComplete = false;
    rootDeletedAfterParser = false;
    outerLoadFinished = false;
    closeOrderingValid = true;
    stageCountersValid = true;
    expectScopedStages = false;
    expectMainScope = false;
    previewStageSamples = parserStageSamples = closeStageSamples = 0;
    originalLoadDepth = 0;
    parseResultSeenByLoad = nullptr;
    scopedLoadResult = 0;
    nestedLoadResult = 0;
    observedSetterAl = 0;
    siteAbiPreserved = modelSiteAbiPreserved = rowSiteAbiPreserved = true;
    vertexLastErrorPreserved = indexLastErrorPreserved = rowLastErrorPreserved = true;
    lastIndexBeforeResult = lastIndexAfterResult = -1;
    vertex24BeforeNative = vertex24AfterNative = vertex24Skipped = 0;
    vertex32BeforeNative = vertex32AfterNative = vertex32Skipped = 0;
    indexBeforeNative = indexAfterNative = 0;
    modelNative = modelCleanup = rowNative = rowThumbnail = 0;
    __asm { fninit }
    _mm_setcsr(0x1F80u);
}

struct Expected {
    Failure failure = NoFailure;
    uint32_t missingVertex = 0;
    uint32_t failedVertex = 0;
    uint32_t missingIndex = 0;
    uint32_t failedIndex = 0;
    uint32_t failedModel = 0;
    uint32_t vertex24Failures = 0;
    uint32_t vertex32Failures = 0;
    bool rootDiscarded = false;
    bool unlock = false;
    bool sourceClosed = false;
    uint32_t parserCalls = 1;
    uint32_t nativeLoadCalls = 1;
};

void VerifyCommonScoped(
    const char* name, uint32_t unit, uint16_t slot,
    const SdgoHangarPreviewGuardStatusV1& before,
    const SdgoHangarPreviewGuardStatusV1& after,
    const Expected& expected) {
    const bool failed = expected.failure != NoFailure;
    CheckCase(Delta(before.rows, after.rows) == 1 &&
                  Delta(before.previews, after.previews) == 1 &&
                  Delta(before.loaded, after.loaded) == (failed ? 0u : 1u),
              name, "row, preview and loaded counters match one scoped setter");
    CheckCase(Delta(before.fallbacks, after.fallbacks) == (failed ? 1u : 0u) &&
                  Delta(before.discarded_models, after.discarded_models) ==
                      (failed ? 1u : 0u) &&
                  Delta(before.lock_releases, after.lock_releases) ==
                      (failed ? 1u : 0u) &&
                  Delta(before.discarded_roots, after.discarded_roots) ==
                      (expected.rootDiscarded ? 1u : 0u),
              name, "ownership and native fallback counters are balanced");
    CheckCase(Delta(before.missing_vertex, after.missing_vertex) ==
                      expected.missingVertex &&
                  Delta(before.failed_vertex_lock, after.failed_vertex_lock) ==
                      expected.failedVertex &&
                  Delta(before.missing_index, after.missing_index) ==
                      expected.missingIndex &&
                  Delta(before.failed_index_lock, after.failed_index_lock) ==
                      expected.failedIndex &&
                  Delta(before.failed_model_load, after.failed_model_load) ==
                      expected.failedModel,
              name, "only the first native failure reason is counted");
    CheckCase(Delta(before.vertex24_failures, after.vertex24_failures) ==
                      expected.vertex24Failures &&
                  Delta(before.vertex32_failures, after.vertex32_failures) ==
                      expected.vertex32Failures &&
                  Delta(before.closed_sources, after.closed_sources) ==
                      (expected.sourceClosed ? 1u : 0u),
              name, "stride failures and guarded source closure are reported separately");
    CheckCase(before.active_preview == 0 && before.active_parser == 0 &&
                  after.active_preview == 0 && after.active_parser == 0 &&
                  stageCountersValid && previewStageSamples >= 1 &&
                  parserStageSamples == expected.parserCalls,
              name, "observer stage flags cover only the active preview and parser frames");
    if (failed) {
        CheckCase(after.last_unit == unit && after.last_failed_slot == slot &&
                      after.last_failure == expected.failure,
                  name, "observer identifies the rejected unit, native inventory slot and first failure");
    } else {
        CheckCase(after.last_unit == before.last_unit &&
                      after.last_failed_slot == before.last_failed_slot &&
                      after.last_failure == before.last_failure,
                  name, "clean preview does not replace the last failure");
    }
    CheckCase(originalRows == 1 && originalSets == 1 &&
                  originalParses == expected.parserCalls &&
                  originalLoads == expected.nativeLoadCalls,
              name, "real code detours forward the expected native load and parse calls");
    void* expectedObservedRenderer = renderer.data();
    if (expected.nativeLoadCalls == 1) expectedObservedRenderer = loader.data();
    if (expected.nativeLoadCalls == 2) expectedObservedRenderer = nestedLoader.data();
    CheckCase(observedRowOwner == rowOwner.data() &&
                  observedRow == rowRecord.data() &&
                  observedRenderer == expectedObservedRenderer &&
                  observedName == reinterpret_cast<void*>(0x11111111u) &&
                  observedAnimation == reinterpret_cast<void*>(0x22222222u),
              name, "x86 thiscall arguments reach the native mocks unchanged");
    CheckCase(siteAbiPreserved && modelSiteAbiPreserved && rowSiteAbiPreserved &&
                  vertexLastErrorPreserved && indexLastErrorPreserved &&
                  rowLastErrorPreserved,
              name, "site detours preserve registers, flags, x87, SSE and LastError");
    const uint8_t expectedLoadResult =
        expected.nativeLoadCalls && !expected.rootDiscarded ? 0x7Bu : 0u;
    CheckCase(scopedLoadResult == expectedLoadResult,
              name, "load-model result follows the parser root accepted by that load frame");
    CheckCase(observedSetterAl == kSetterAl,
              name, "setter preserves the original nontrivial AL result");
    if (expected.parserCalls) {
        CheckCase(parseResultSeenByLoad ==
                      (expected.rootDiscarded ? nullptr : static_cast<void*>(&root)),
                  name, "the original load receives the guarded parser result exactly once");
    }
    if (failed) {
        CheckCase(candidateDeletes == 1 &&
                      observedCandidateDelete == &candidate &&
                      candidateDeleteFlag == 1 &&
                      *reinterpret_cast<void**>(renderer.data() + 0x34) == nullptr,
                  name, "failed candidate uses its scalar deleting destructor once");
        CheckCase(rendererLeaves == 1 && observedLeave == renderer.data() &&
                      modelCleanup == 1 && modelNative == 0 &&
                      rowThumbnail == 1 && rowNative == 0,
                  name, "matching renderer lock releases once into the thumbnail path");
    } else {
        CheckCase(candidateDeletes == 0 && rendererLeaves == 0 &&
                      *reinterpret_cast<void**>(renderer.data() + 0x34) == &candidate &&
                      modelCleanup == 0 && modelNative == 1 &&
                      rowThumbnail == 0 && rowNative == 1,
                  name, "clean model and row continuations remain native");
    }
    CheckCase(rootDeletes == (expected.rootDiscarded ? 1u : 0u) &&
                  (!expected.rootDiscarded ||
                   (observedRootDelete == &root && rootDeleteFlag == 1 &&
                    rootDeletedAfterParser)),
              name, "returned native root is destroyed once only after parser completion");
    CheckCase(sourceCloses == (expected.sourceClosed ? 1u : 0u) &&
                  (!expected.sourceClosed ||
                   (observedClose == source.data() && closeOrderingValid &&
                    closeStageSamples == 1)),
              name, "only a Mod-rejected parser result closes its exact source after load return");
    CheckCase(unlocks == (expected.unlock ? 1u : 0u) &&
                  (!expected.unlock ||
                   observedUnlock == (expected.failedIndex
                       ? static_cast<void*>(&indexResource)
                       : static_cast<void*>(&vertexResource))),
              name, "successful null mapping balances exactly one matching Unlock");
}

void VerifyScenarioSites(const char* name, Scenario next) {
    bool valid = false;
    switch (next) {
    case Scenario::Success:
    case Scenario::FalseModelResult:
        valid = vertex24BeforeNative == 1 && vertex24AfterNative == 1 &&
                vertex24Skipped == 0 &&
                vertex32BeforeNative == 1 && vertex32AfterNative == 1 &&
                vertex32Skipped == 0 &&
                indexBeforeNative == 1 && indexAfterNative == 1;
        break;
    case Scenario::MissingVertex24:
        valid = vertex24BeforeNative == 0 && vertex24AfterNative == 0 &&
                vertex24Skipped == 1;
        break;
    case Scenario::FailedVertex24:
    case Scenario::NullVertex24:
        valid = vertex24BeforeNative == 1 && vertex24AfterNative == 0 &&
                vertex24Skipped == 1;
        break;
    case Scenario::MissingVertex32:
        valid = vertex32BeforeNative == 0 && vertex32AfterNative == 0 &&
                vertex32Skipped == 1;
        break;
    case Scenario::FailedVertex32:
    case Scenario::NullVertex32:
        valid = vertex32BeforeNative == 1 && vertex32AfterNative == 0 &&
                vertex32Skipped == 1;
        break;
    case Scenario::MissingIndex:
        valid = indexBeforeNative == 0 && indexAfterNative == 0 &&
                lastIndexBeforeResult == 0;
        break;
    case Scenario::FailedIndex:
    case Scenario::NullIndex:
        valid = indexBeforeNative == 1 && indexAfterNative == 0 &&
                lastIndexBeforeResult == 1 && lastIndexAfterResult == 0;
        break;
    case Scenario::NullModelObject:
        valid = vertex24BeforeNative == 0 && vertex24AfterNative == 0 &&
                vertex32BeforeNative == 0 && vertex32AfterNative == 0 &&
                indexBeforeNative == 0 && indexAfterNative == 0;
        break;
    case Scenario::NestedLoadFrame:
        valid = vertex24BeforeNative == 1 && vertex24AfterNative == 1 &&
                vertex24Skipped == 1 &&
                vertex32BeforeNative == 1 && vertex32AfterNative == 1 &&
                vertex32Skipped == 0 &&
                indexBeforeNative == 1 && indexAfterNative == 1;
        break;
    case Scenario::WrongSetterReturn:
        break;
    }
    CheckCase(valid, name,
              "real site detours select the expected native or failure continuation");
}

void RunScopedCase(
    const char* name, Scenario next, uint32_t unit, const Expected& expected,
    uint16_t slot = 0) {
    ResetFixture(next, unit, slot);
    expectScopedStages = true;
    const auto before = Snapshot();
    const DWORD error = 0x51000000u + unit;
    SetLastError(error);
    reinterpret_cast<RowHook>(rowHook)(
        rowOwner.data(), nullptr, rowRecord.data());
    CheckCase(GetLastError() == error, name,
              "destructors, renderer release and warning preserve LastError");
    const auto after = Snapshot();
    VerifyCommonScoped(name, unit, slot, before, after, expected);
    VerifyScenarioSites(name, next);
    const bool warningExpected =
        expected.failure != NoFailure &&
        (after.fallbacks <= 5 || after.fallbacks % 1000 == 0);
    CheckCase(warnings == (warningExpected ? 1u : 0u),
              name, "only rejected rows emit the bounded warning");
}

void RunWrongReturnCase() {
    constexpr uint32_t unit = 70001;
    ResetFixture(Scenario::WrongSetterReturn, unit);
    const auto before = Snapshot();
    SetLastError(0x51117001u);
    reinterpret_cast<RowHook>(rowHook)(
        rowOwner.data(), nullptr, rowRecord.data());
    const auto after = Snapshot();
    Check(Delta(before.rows, after.rows) == 1 &&
              Delta(before.previews, after.previews) == 0 &&
              Delta(before.loaded, after.loaded) == 0 &&
              Delta(before.fallbacks, after.fallbacks) == 0,
          "wrong setter return address remains outside preview scope");
    Check(originalRows == 1 && originalSets == 1 && originalParses == 1 &&
              originalLoads == 1 && candidateDeletes == 0 &&
              rootDeletes == 0 && rendererLeaves == 0 && sourceCloses == 0 &&
              vertex24BeforeNative == 1 && vertex24Skipped == 0 &&
              modelNative == 1 && rowNative == 1,
          "row scope alone cannot activate parser guards or fallback ownership");
    Check(stageCountersValid && siteAbiPreserved &&
              modelSiteAbiPreserved && rowSiteAbiPreserved &&
              observedSetterAl == kSetterAl &&
              GetLastError() == 0x51117001u,
          "wrong-return forwarding preserves setter AL, site ABI and LastError");
}

void RunOutOfScopeCases() {
    ResetFixture(Scenario::MissingVertex32, 71000);
    const auto before = Snapshot();
    SetLastError(0x51227100u);

    PrepareVertex(24, false, false);
    InvokeEsiSite(vertex24BeforeHook, vertexPart.data(), kEaxSeed);
    RecordEsiPreservation(vertexPart.data(), kEaxSeed);
    InvokeEsiSite(vertex24AfterHook, vertexPart.data(), 0x80004005u);
    RecordEsiPreservation(vertexPart.data(), 0x80004005u);

    PrepareIndex(false, false);
    Check(InvokeIndexBefore(indexBuffer.data(), 1) == 1 &&
              InvokeIndexAfter(indexBuffer.data(), -1) == 1,
          "out-of-scope index sites take their native continuations");

    InvokeEdiSite(modelResultHook, renderer.data(), 0);
    modelSiteAbiPreserved =
        modelSiteAbiPreserved && EdiPreserved(renderer.data(), 0);
    InvokeEdiSite(rowResultHook, rowRecord.data(), 0x66778899u);
    rowSiteAbiPreserved =
        rowSiteAbiPreserved && EdiPreserved(rowRecord.data(), 0x66778899u);

    const uint8_t outsideLoad = reinterpret_cast<LoadHook>(loadHook)(
        nullptr, nullptr, reinterpret_cast<void*>(0x11111111u),
        reinterpret_cast<void*>(0x22222222u));
    void* outsideParse = reinterpret_cast<ParseHook>(parseHook)(
        parser.data(), nullptr, reinterpret_cast<void*>(0x31323334u), 1);
    const uint8_t outsideSetter = reinterpret_cast<SetHook>(setHook)(
        renderer.data(), nullptr, reinterpret_cast<void*>(0x11111111u),
        reinterpret_cast<void*>(0x22222222u));

    const auto after = Snapshot();
    Check(!std::memcmp(&before, &after, sizeof(before)),
          "out-of-scope code and site hooks leave every observer counter unchanged");
    Check(outsideLoad == 0x7Bu && outsideParse == &root &&
              outsideSetter == kSetterAl &&
              originalLoads == 2 && originalParses == 3 && originalSets == 1,
          "out-of-scope load, parser and setter preserve native results");
    Check(vertex24BeforeNative == 1 && vertex24AfterNative == 1 &&
              vertex24Skipped == 0 &&
              vertex32BeforeNative == 3 && vertex32Skipped == 0 &&
              indexBeforeNative == 1 && indexAfterNative == 1 &&
              modelNative == 2 && rowNative == 1,
          "invalid out-of-scope inputs still select the captured native continuations");
    Check(!candidateDeletes && !rootDeletes && !unlocks && !rendererLeaves &&
              !sourceCloses && stageCountersValid &&
              siteAbiPreserved && modelSiteAbiPreserved && rowSiteAbiPreserved,
          "out-of-scope forwarding performs no guard-owned cleanup and preserves ABI");
    Check(GetLastError() == 0x51227100u,
          "out-of-scope forwarding retains the native thread LastError");
}

void VerifyObserverValidation() {
    Check(!statusHook(nullptr), "V1 observer rejects null output");
    SdgoHangarPreviewGuardStatusV1 value{};
    value.size = sizeof(value) - sizeof(uint32_t);
    value.version = 1;
    Check(!statusHook(&value), "V1 observer rejects undersized output");
    value = {};
    value.size = sizeof(value) + sizeof(uint32_t);
    value.version = 1;
    Check(!statusHook(&value), "V1 observer rejects oversized output");
    value = {};
    value.size = sizeof(value);
    value.version = 2;
    Check(!statusHook(&value), "V1 observer rejects the wrong version");
    value = {};
    value.size = sizeof(value);
    value.version = 1;
    Check(statusHook(&value) && value.size == sizeof(value) && value.version == 1,
          "V1 observer accepts only its exact size and version");
}

void RunMainHangarCases(HMODULE dll) {
    auto statusV2 = reinterpret_cast<SdgoHangarPreviewGuardGetStatusV2Fn>(
        GetProcAddress(dll, "SdgoHangarPreviewGuardGetStatusV2"));
    SdgoHangarPreviewGuardStatusV2 initial{sizeof(initial), 2};
    Check(statusV2 && statusV2(&initial), "main hangar has an additive V2 observer");
    Check(!statusV2(nullptr), "V2 observer rejects null output");
    SdgoHangarPreviewGuardStatusV2 invalid{sizeof(initial)-4, 2};
    Check(!statusV2(&invalid), "V2 observer rejects a shorter structure");
    for (const auto next : {Scenario::MissingVertex32, Scenario::FailedVertex32,
         Scenario::NullVertex32, Scenario::MissingIndex, Scenario::FailedIndex,
         Scenario::NullIndex, Scenario::FalseModelResult, Scenario::NullModelObject,
         Scenario::Success}) {
        ResetFixture(next, 73000);
        expectScopedStages = true;
        expectMainScope = true;
        const auto before = Snapshot();
        SetLastError(0x53217300);
        const auto result = static_cast<uint8_t>(InvokeHangarSetter(renderer.data(), 0));
        const DWORD error = GetLastError();
        const auto after = Snapshot();
        const bool failed = next != Scenario::Success;
        const bool rejectedParser = failed && next != Scenario::FalseModelResult &&
            next != Scenario::NullModelObject;
        Check(result == kSetterAl && error == 0x53217300 &&
              originalRows == 0 && originalSets == 1 && rendererLeaves == 0 &&
              rowThumbnail == 0 && rowNative == 0,
              "standalone preview preserves setter AL and leaves native caller lock/row ownership alone");
        Check(candidateDeletes == (failed ? 1u : 0u) &&
              rootDeletes == (rejectedParser ? 1u : 0u) &&
              sourceCloses == (rejectedParser ? 1u : 0u) &&
              modelCleanup == (failed ? 1 : 0) && modelNative == (failed ? 0 : 1),
              "main hangar failure releases only its unregistered parser root, source and candidate");
        void* remaining = nullptr; std::memcpy(&remaining, renderer.data()+0x34, sizeof(remaining));
        Check(remaining == (failed ? nullptr : &candidate) &&
              stageCountersValid && siteAbiPreserved && modelSiteAbiPreserved,
              "the original hangar caller sees its null-model branch or an unchanged successful model");
        Check(after.rows == before.rows && after.previews == before.previews &&
              after.loaded == before.loaded && after.fallbacks == before.fallbacks &&
              after.lock_releases == before.lock_releases && !after.active_preview &&
              !after.active_parser && !after.active_failure,
              "standalone scope does not claim row fallback or retain active parser state");
    }
    SdgoHangarPreviewGuardStatusV2 final{sizeof(final), 2};
    Check(statusV2(&final) && final.hangar_previews == initial.hangar_previews+9 &&
          final.hangar_failed == initial.hangar_failed+8 && final.hangar_loaded == initial.hangar_loaded+1,
          "separate counters include eight rejected main previews and a later successful load");
}

void Run(SdgoModLoadFn entry, HMODULE dll) {
    candidateTable[0] = reinterpret_cast<void*>(&DeleteCandidate);
    rootTable[0] = reinterpret_cast<void*>(&DeleteRoot);
    vertexResourceTable[12] = reinterpret_cast<void*>(&UnlockBuffer);
    indexResourceTable[12] = reinterpret_cast<void*>(&UnlockBuffer);
    setterReturnAddress = reinterpret_cast<void*>(InvokeSetter(nullptr, 1));
    hangarSetterReturnAddress = reinterpret_cast<void*>(InvokeHangarSetter(nullptr, 1));
    Check(setterReturnAddress != nullptr,
          "setter fixture exposes its exact stable after-call address");

    statusHook = reinterpret_cast<StatusHook>(
        GetProcAddress(dll, SDGO_HANGAR_PREVIEW_GUARD_STATUS_EXPORT));
    statusV2Hook=reinterpret_cast<SdgoHangarPreviewGuardGetStatusV2Fn>(
        GetProcAddress(dll,"SdgoHangarPreviewGuardGetStatusV2"));
    Check(statusHook != nullptr, "production DLL exports the V1 observer");

    SdgoApi api{};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Symbol = Symbol;
    api.HookCode = HookCode;
    api.HookSite = HookSite;
    api.Log = Log;
    auto* mod = reinterpret_cast<SdgoMod*>(0x13572468u);

    Check(!entry(nullptr, mod), "loader rejects a null API");
    Check(!entry(&api, nullptr), "loader rejects a null Mod identity");
    const uint32_t fullSize = api.size;
    api.size = offsetof(SdgoApi, HookSite);
    Check(!entry(&api, mod), "loader rejects an API without the complete HookSite field");
    api.size = fullSize;
    missingSymbol = "preview_guard.vertex32_continue";
    Check(!entry(&api, mod) && !rowHook && !setHook && !parseHook && !loadHook,
          "missing verified dependency refuses activation before hook mutation");
    missingSymbol = nullptr;

    Check(entry(&api, mod) && rowHook && setHook && parseHook && loadHook &&
              vertex24BeforeHook && vertex24AfterHook &&
              vertex32BeforeHook && vertex32AfterHook &&
              indexBeforeHook && indexAfterHook &&
              modelResultHook && rowResultHook,
          "production DLL installs every captured code and site detour");
    Check(!entry(&api, mod), "production DLL refuses duplicate activation");
    Check(infos == 1, "successful activation emits its single informational log");

    RunOutOfScopeCases();
    RunWrongReturnCase();

    RunScopedCase("clean scoped preview", Scenario::Success, 72000, Expected{});

    Expected missing24{};
    missing24.failure = MissingVertex;
    missing24.missingVertex = 1;
    missing24.vertex24Failures = 1;
    missing24.rootDiscarded = true;
    missing24.sourceClosed = true;
    RunScopedCase("missing VB24", Scenario::MissingVertex24, 72024, missing24);

    Expected failed24{};
    failed24.failure = VertexLock;
    failed24.failedVertex = 1;
    failed24.vertex24Failures = 1;
    failed24.rootDiscarded = true;
    failed24.sourceClosed = true;
    RunScopedCase("failed VB24 lock", Scenario::FailedVertex24, 72124, failed24);

    Expected null24 = failed24;
    null24.unlock = true;
    RunScopedCase("VB24 null mapping", Scenario::NullVertex24, 72224, null24);

    Expected missing32 = missing24;
    missing32.vertex24Failures = 0;
    missing32.vertex32Failures = 1;
    RunScopedCase("missing VB32", Scenario::MissingVertex32, 72032, missing32);

    constexpr uint32_t duplicateUnit = 72932;
    constexpr uint16_t failedSlot = 7;
    constexpr uint16_t duplicateSlot = 19;
    const auto beforeDuplicateFailure = Snapshot();
    RunScopedCase(
        "duplicate definition failed cell", Scenario::MissingVertex32,
        duplicateUnit, missing32, failedSlot);
    const auto afterDuplicateFailure = Snapshot();
    Check(Delta(
              beforeDuplicateFailure.recovered_previews,
              afterDuplicateFailure.recovered_previews) == 0 &&
              afterDuplicateFailure.last_unit == duplicateUnit &&
              afterDuplicateFailure.last_failed_slot == failedSlot,
          "failed duplicate-definition cell records its exact native inventory slot without recovery");

    const auto beforeOtherSlot = afterDuplicateFailure;
    RunScopedCase(
        "duplicate definition other cell", Scenario::Success,
        duplicateUnit, Expected{}, duplicateSlot);
    const auto afterOtherSlot = Snapshot();
    Check(Delta(
              beforeOtherSlot.recovered_previews,
              afterOtherSlot.recovered_previews) == 0 &&
              afterOtherSlot.last_unit == duplicateUnit &&
              afterOtherSlot.last_failed_slot == failedSlot,
          "same definition in another native inventory slot cannot satisfy the failed cell recovery");

    const auto beforeSameSlot = afterOtherSlot;
    RunScopedCase(
        "duplicate definition same-cell retry", Scenario::Success,
        duplicateUnit, Expected{}, failedSlot);
    const auto afterSameSlot = Snapshot();
    Check(Delta(
              beforeSameSlot.recovered_previews,
              afterSameSlot.recovered_previews) == 1 &&
              afterSameSlot.last_unit == duplicateUnit &&
              afterSameSlot.last_failed_slot == failedSlot,
          "successful production setter retry for the exact failed unit and slot records one recovery");

    Expected failed32 = failed24;
    failed32.vertex24Failures = 0;
    failed32.vertex32Failures = 1;
    RunScopedCase("failed VB32 lock", Scenario::FailedVertex32, 72132, failed32);

    Expected null32 = failed32;
    null32.unlock = true;
    RunScopedCase("VB32 null mapping", Scenario::NullVertex32, 72232, null32);

    Expected missingIb{};
    missingIb.failure = MissingIndex;
    missingIb.missingIndex = 1;
    missingIb.rootDiscarded = true;
    missingIb.sourceClosed = true;
    RunScopedCase("missing index buffer", Scenario::MissingIndex, 72300, missingIb);
    Check(lastIndexBeforeResult == 0 && indexBeforeNative == 0,
          "missing index resource uses the native saved-register failure epilogue");

    Expected failedIb{};
    failedIb.failure = IndexLock;
    failedIb.failedIndex = 1;
    failedIb.rootDiscarded = true;
    failedIb.sourceClosed = true;
    RunScopedCase("failed index lock", Scenario::FailedIndex, 72400, failedIb);
    Check(lastIndexBeforeResult == 1 && lastIndexAfterResult == 0 &&
              indexBeforeNative == 1 && indexAfterNative == 0,
          "failed index lock returns through the exact ret-8 failure frame");

    Expected nullIb = failedIb;
    nullIb.unlock = true;
    RunScopedCase("index null mapping", Scenario::NullIndex, 72500, nullIb);
    Check(lastIndexBeforeResult == 1 && lastIndexAfterResult == 0 &&
              indexBeforeNative == 1 && indexAfterNative == 0,
          "null index mapping balances Unlock before the native failure epilogue");

    Expected falseModel{};
    falseModel.failure = ModelLoad;
    falseModel.failedModel = 1;
    RunScopedCase(
        "false model AL with high garbage", Scenario::FalseModelResult,
        72600, falseModel);
    Check(modelCleanup == 1 && modelNative == 0,
          "model result gate tests low AL despite nonzero high EAX");

    Expected nullModel = falseModel;
    nullModel.parserCalls = 0;
    nullModel.nativeLoadCalls = 0;
    RunScopedCase(
        "null scoped model object", Scenario::NullModelObject,
        72700, nullModel);

    Expected nestedFrame = missing24;
    nestedFrame.parserCalls = 2;
    nestedFrame.nativeLoadCalls = 2;
    RunScopedCase(
        "nested load frame isolates source ownership", Scenario::NestedLoadFrame,
        72800, nestedFrame);
    Check(nestedLoadResult == 0x7Bu && sourceCloses == 1 &&
              observedClose == source.data(),
          "nested successful load restores the outer frame and closes only its rejected source");

    RunMainHangarCases(dll);
    VerifyObserverValidation();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        HMODULE dll = LoadLibraryW(argv[1]);
        auto entry = dll ? reinterpret_cast<SdgoModLoadFn>(
            GetProcAddress(dll, "SdgoModLoad")) : nullptr;
        Check(entry != nullptr, "production DLL exports its loader");
        Run(entry, dll);
        FreeLibrary(dll);
        std::printf("PASS %u production hangar-preview guard checks\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::printf("FAIL %s\n", error.what());
        return 1;
    }
}
