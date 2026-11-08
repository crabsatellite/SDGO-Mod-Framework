#include "sdgo/match_ai_events.h"

namespace { SdgoMatchAiEventsV1 sink{}; }
extern "C" __declspec(dllexport) int __cdecl SdgoMatchAiRegisterEventsV1(const SdgoMatchAiEventsV1* value) {
    if (!value || value->size != sizeof(sink) || value->version != 1 || !value->packet) return 0;
    sink = *value;
    return 1;
}
extern "C" __declspec(dllexport) void __cdecl EmitTransformFixture(uint16_t opcode, const uint8_t* raw) {
    if (sink.packet) sink.packet(opcode, raw, sink.user);
}
