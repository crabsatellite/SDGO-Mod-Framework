#include <windows.h>

#include <cstring>

#include "sdgo/operator_reward_filter.h"

namespace {
int g_registrationResult = 1;
bool g_registered = false;
SdgoOperatorRewardFilterSinkV1 g_sink{};
}

extern "C" __declspec(dllexport) void __cdecl
OperatorRewardFilterFixtureReset() {
    g_registrationResult = 1;
    g_registered = false;
    std::memset(&g_sink, 0, sizeof(g_sink));
}

extern "C" __declspec(dllexport) void __cdecl
OperatorRewardFilterFixtureSetRegistrationResult(int result) {
    g_registrationResult = result;
}

extern "C" __declspec(dllexport) int __cdecl
OperatorRewardFilterFixtureHasSink() {
    return g_registered ? 1 : 0;
}

extern "C" __declspec(dllexport) int __cdecl
OperatorRewardFilterFixtureInvoke(
    const SdgoOperatorRewardContextV1* context,
    SdgoOperatorRewardResultV1* result) {
    if (!g_registered || !g_sink.callback) {
        return SDGO_OPERATOR_REWARD_FILTER_REJECT;
    }
    return g_sink.callback(context, result, g_sink.user);
}

extern "C" __declspec(dllexport) int __cdecl
SdgoBattleRewardsRegisterOperatorRewardFilterV1(
    const SdgoOperatorRewardFilterSinkV1* sink) {
    if (g_registrationResult != 1) return g_registrationResult;
    if (!sink || sink->size < sizeof(*sink) ||
        sink->version != SDGO_OPERATOR_REWARD_FILTER_VERSION ||
        !sink->callback || g_registered) {
        return 0;
    }
    g_sink = *sink;
    g_registered = true;
    return 1;
}
