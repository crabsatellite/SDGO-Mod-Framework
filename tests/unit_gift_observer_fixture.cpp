#include <cstdint>
#include <cstring>

#include "sdgo/friend_unit_gift_client_observer.h"

namespace {

SdgoFriendUnitGiftClientSnapshotV1 g_snapshot{};
SdgoFriendUnitGiftClientActionRequestV1 g_request{};
std::uint32_t g_snapshotCalls = 0;
std::uint32_t g_actionCalls = 0;
int g_snapshotResult = 1;
int g_actionResult = 1;

} // namespace

#if defined(_M_IX86) && \
    !defined(UNIT_GIFT_OBSERVER_FIXTURE_NO_PRODUCT_EXPORTS)
#pragma comment(linker, "/EXPORT:SdgoFriendUnitGiftClientGetSnapshotV1=_SdgoFriendUnitGiftClientGetSnapshotV1")
#pragma comment(linker, "/EXPORT:SdgoFriendUnitGiftClientActionV1=_SdgoFriendUnitGiftClientActionV1")
#endif
#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:TestUnitGiftReset=_TestUnitGiftReset")
#pragma comment(linker, "/EXPORT:TestUnitGiftSetSnapshot=_TestUnitGiftSetSnapshot")
#pragma comment(linker, "/EXPORT:TestUnitGiftSetSnapshotResult=_TestUnitGiftSetSnapshotResult")
#pragma comment(linker, "/EXPORT:TestUnitGiftSetActionResult=_TestUnitGiftSetActionResult")
#pragma comment(linker, "/EXPORT:TestUnitGiftSnapshotCalls=_TestUnitGiftSnapshotCalls")
#pragma comment(linker, "/EXPORT:TestUnitGiftActionCalls=_TestUnitGiftActionCalls")
#pragma comment(linker, "/EXPORT:TestUnitGiftCopyLastRequest=_TestUnitGiftCopyLastRequest")
#endif

#if !defined(UNIT_GIFT_OBSERVER_FIXTURE_NO_PRODUCT_EXPORTS)
extern "C" __declspec(dllexport) int __cdecl
SdgoFriendUnitGiftClientGetSnapshotV1(
    SdgoFriendUnitGiftClientSnapshotV1* output) {
    ++g_snapshotCalls;
    if (!output || !g_snapshotResult) return 0;
    *output = g_snapshot;
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl
SdgoFriendUnitGiftClientActionV1(
    const SdgoFriendUnitGiftClientActionRequestV1* request) {
    ++g_actionCalls;
    if (!request) return 0;
    g_request = *request;
    return g_actionResult;
}
#endif

extern "C" __declspec(dllexport) void __cdecl TestUnitGiftReset() {
    g_snapshot = {};
    g_request = {};
    g_snapshotCalls = 0;
    g_actionCalls = 0;
    g_snapshotResult = 1;
    g_actionResult = 1;
}

extern "C" __declspec(dllexport) void __cdecl TestUnitGiftSetSnapshot(
    const SdgoFriendUnitGiftClientSnapshotV1* snapshot) {
    g_snapshot = snapshot ? *snapshot : SdgoFriendUnitGiftClientSnapshotV1{};
}

extern "C" __declspec(dllexport) void __cdecl
TestUnitGiftSetSnapshotResult(int result) {
    g_snapshotResult = result;
}

extern "C" __declspec(dllexport) void __cdecl
TestUnitGiftSetActionResult(int result) {
    g_actionResult = result;
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl
TestUnitGiftSnapshotCalls() {
    return g_snapshotCalls;
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl
TestUnitGiftActionCalls() {
    return g_actionCalls;
}

extern "C" __declspec(dllexport) int __cdecl TestUnitGiftCopyLastRequest(
    SdgoFriendUnitGiftClientActionRequestV1* output) {
    if (!output) return 0;
    *output = g_request;
    return 1;
}
