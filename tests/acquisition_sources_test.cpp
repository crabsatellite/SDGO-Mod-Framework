#include "../mods/acquisition_sources/source_list.h"
#include <cassert>
#include <string>

using namespace sdgo::acquisition_sources;
std::uint32_t __cdecl Provider(const SdgoAcquisitionRequestV1* request, const SdgoAcquisitionSinkV1* sink) {
    assert(request->size == sizeof(*request) && request->version == 1);
    assert(sink->size == sizeof(*sink) && sink->version == 1);
    const std::wstring text = L"自定义扭蛋机2号";
    assert(sink->Emit(sink->context, "custom.2", text.data(), static_cast<std::uint32_t>(text.size())));
    assert(sink->Emit(sink->context, "custom.2", text.data(), static_cast<std::uint32_t>(text.size())));
    return request->id == 1 ? SDGO_ACQUISITION_READY : SDGO_ACQUISITION_INCOMPLETE;
}
std::uint32_t __cdecl Conflict(const SdgoAcquisitionRequestV1*, const SdgoAcquisitionSinkV1* sink) {
    sink->Emit(sink->context, "same", L"甲", 1);
    sink->Emit(sink->context, "same", L"乙", 1);
    return SDGO_ACQUISITION_READY;
}
std::uint32_t __cdecl Invalid(const SdgoAcquisitionRequestV1*, const SdgoAcquisitionSinkV1* sink) {
    sink->Emit(sink->context, "valid", L"来源", 2);
    const wchar_t broken[] = {0xD800};
    sink->Emit(sink->context, "bad", broken, 1);
    return SDGO_ACQUISITION_READY;
}
std::uint32_t __cdecl Fault(const SdgoAcquisitionRequestV1*, const SdgoAcquisitionSinkV1*) {
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    return SDGO_ACQUISITION_READY;
}
std::uint32_t __cdecl BoundaryKey(const SdgoAcquisitionRequestV1*, const SdgoAcquisitionSinkV1* sink) {
    char key[128]{};
    std::memset(key, 'k', 127);
    return sink->Emit(sink->context, key, L"边界来源", 4)
        ? SDGO_ACQUISITION_READY : SDGO_ACQUISITION_INCOMPLETE;
}
std::uint32_t __cdecl OversizeKey(const SdgoAcquisitionRequestV1*, const SdgoAcquisitionSinkV1* sink) {
    char key[129]{};
    std::memset(key, 'k', 128);
    sink->Emit(sink->context, key, L"过长来源", 4);
    return SDGO_ACQUISITION_READY;
}
int __cdecl DisplayReady() { return 1; }
int __cdecl DisplayIncompatible() { return 2; }
int __cdecl DisplayFault() { RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr); return 1; }
int main() {
    assert(AllowBlueprint(0, true) && !AllowBlueprint(0, false));
    assert(AllowBlueprint(1, true) && AllowBlueprint(1, false));
    assert(!AllowBlueprint(2, true) && !AllowBlueprint(2, false));
    assert(!AllowBlueprint(3, true) && !AllowBlueprint(3, false));
    assert(!DisplayInstalled(nullptr));
    assert(DisplayInstalled(&DisplayReady));
    assert(!DisplayInstalled(&DisplayIncompatible));
    assert(!DisplayInstalled(&DisplayFault));
    assert(Query(nullptr, SDGO_ACQUISITION_UNIT, 1).empty());
    const auto first = Query(&Provider, SDGO_ACQUISITION_UNIT, 1);
    assert(first.size() == 1 && first[0] == L"自定义扭蛋机2号");
    assert(Query(&Provider, SDGO_ACQUISITION_UNIT, 2).empty());
    assert(Query(&Conflict, SDGO_ACQUISITION_BLUEPRINT, 1).empty());
    assert(Query(&Invalid, SDGO_ACQUISITION_BLUEPRINT, 1).empty());
    assert(Query(&Fault, SDGO_ACQUISITION_UNIT, 1).empty());
    const auto boundary = Query(&BoundaryKey, SDGO_ACQUISITION_UNIT, 1);
    assert(boundary.size() == 1 && boundary[0] == L"边界来源");
    assert(Query(&OversizeKey, SDGO_ACQUISITION_UNIT, 1).empty());
    assert(Query(&Provider, 99, 1).empty());
    assert(Query(&Provider, SDGO_ACQUISITION_UNIT, 0).empty());
    return 0;
}
