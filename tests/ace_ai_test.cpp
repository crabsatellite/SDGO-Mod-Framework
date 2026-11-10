#include "ai_test_support.h"
#include "../mods/ace_ai/ace_ai.cpp"
#include <array>
#include <string>

namespace fixture {
std::array<uint8_t, 0x500> app{};
std::array<uint8_t, 0x100> service{};
std::array<uint8_t, 0x80> mode{};
int lineCtors = 0;
int lineDtors = 0;
int stringCtors = 0;
int appendCalls = 0;
int raiseStage = 0;
bool appendSuccess = false;
std::wstring appendedText;

uint8_t* App() { return app.data(); }
void __fastcall LineCtor(void*, void*) {
    ++lineCtors;
    if (raiseStage == 1) RaiseException(0xE0421001u, 0, 0, nullptr);
}
void __fastcall LineDtor(void*, void*) {
    ++lineDtors;
}
void* __fastcall StringCtor(void* target, void*, const wchar_t* text, size_t length) {
    ++stringCtors;
    if (raiseStage == 2) RaiseException(0xE0421002u, 0, 0, nullptr);
    appendedText.assign(text, length);
    return target;
}
void __fastcall Append(void* list, void*, const void* record) {
    ++appendCalls;
    const auto* bytes = static_cast<const uint8_t*>(record);
    CHECK(ai_test::Get<uint32_t>(bytes, 0x54) == 1000);
    CHECK(ai_test::Get<uint32_t>(bytes, 0x58) == 0x70D9FF);
    CHECK(ai_test::Get<uint16_t>(bytes, 0x64) == 10);
    if (raiseStage == 3) RaiseException(0xE0421003u, 0, 0, nullptr);
    if (appendSuccess) {
        auto* listBytes = static_cast<uint8_t*>(list);
        ai_test::Put<uint32_t>(listBytes, 4,
            ai_test::Get<uint32_t>(listBytes, 4) + 1);
    }
}
void Reset() {
    app.fill(0);
    service.fill(0);
    mode.fill(0);
    ai_test::Put(app.data(), 0x4A4, service.data());
    ai_test::Put(service.data(), 0x0C, mode.data());
    ai_test::Put<uint32_t>(service.data(), 0x3C, 3);
    ai_test::Put(service.data(), 0x94, reinterpret_cast<void*>(1));
    ai_test::Put<uint32_t>(service.data(), 0x98, 0);
    lineCtors = lineDtors = stringCtors = appendCalls = 0;
    raiseStage = 0;
    appendSuccess = false;
    appendedText.clear();
    g_active = true;
    g_app = &App;
    g_lineCtor = reinterpret_cast<StepFn>(&LineCtor);
    g_lineDtor = reinterpret_cast<StepFn>(&LineDtor);
    g_stringCtor = reinterpret_cast<StringCtorFn>(&StringCtor);
    g_appendLine = reinterpret_cast<AppendLineFn>(&Append);
}
}

int main() {
    using namespace fixture;
    SCENARIO("native feed append must be observed before provider reports success");
    Reset();
    CHECK(SdgoAceAiAnnounceTeamV1(mode.data(), 1) == 0);
    CHECK(appendCalls == 1 && lineCtors == 1 && lineDtors == 1);
    CHECK(ai_test::Get<uint32_t>(service.data(), 0x98) == 0);
    CHECK(appendedText == L"红队出现了王牌驾驶员！");
    appendSuccess = true;
    CHECK(SdgoAceAiAnnounceTeamV1(mode.data(), 1) == 1);
    CHECK(appendCalls == 2 && lineCtors == 2 && lineDtors == 2);
    CHECK(ai_test::Get<uint32_t>(service.data(), 0x98) == 1);

    SCENARIO("displayed colour selects the fixed blue notice");
    Reset(); appendSuccess = true;
    CHECK(SdgoAceAiAnnounceTeamV1(mode.data(), 2) == 1);
    CHECK(appendedText == L"蓝队出现了王牌驾驶员！");
    CHECK(SdgoAceAiAnnounceTeamV1(mode.data(), 0) == 0 && appendCalls == 1);

    SCENARIO("constructor and append faults never leak a constructed native line");
    for (int stage = 1; stage <= 3; ++stage) {
        Reset(); appendSuccess = true; raiseStage = stage;
        CHECK(SdgoAceAiAnnounceTeamV1(mode.data(), 1) == 0);
        CHECK(lineDtors == (stage == 1 ? 0 : 1));
        CHECK(ai_test::Get<uint32_t>(service.data(), 0x98) == 0);
    }

    SCENARIO("unready or mismatched battle state does not construct a line");
    Reset(); appendSuccess = true;
    ai_test::Put<uint32_t>(service.data(), 0x3C, 2);
    CHECK(SdgoAceAiAnnounceTeamV1(mode.data(), 1) == 0 && lineCtors == 0);
    ai_test::Put<uint32_t>(service.data(), 0x3C, 3);
    std::array<uint8_t, 8> otherMode{};
    CHECK(SdgoAceAiAnnounceTeamV1(otherMode.data(), 1) == 0 && lineCtors == 0);
    g_active = false;
    CHECK(SdgoAceAiAnnounceTeamV1(mode.data(), 1) == 0 && lineCtors == 0);

    return ai_test::Finish();
}
