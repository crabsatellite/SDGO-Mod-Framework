// Exercise the native mode buttons without changing any equipped parts.
int __cdecl FactoryPartsEntry(const SdgoGameTestContextV1* context) {
    static unsigned view = 0, nativeIcons = 0, nativeButtons = 0;
    static bool capturePending = false;
    static ULONGLONG clickedAt = 0;
    static void* upgrade = nullptr;
    if (!context->tick) {
        view = nativeIcons = nativeButtons = 0;
        capturePending = false;
        upgrade = nullptr;
    }
    if (!view) {
        const auto navigation = hangarNavigation.EnsureFactoryUpgradePage(&upgrade);
        if (navigation != HangarNavigation::kReady)
            return navigation >= 0x100 ? SDGO_GAME_TEST_FAIL : SDGO_GAME_TEST_WAIT;
        if (!ocObserver.Bind() || !g1Native.Bind()) return SDGO_GAME_TEST_FAIL;
        if (!ocObserver.click("tab", L"StatusRadio")) return SDGO_GAME_TEST_WAIT;
        view = 1;
        clickedAt = GetTickCount64();
        return SDGO_GAME_TEST_WAIT;
    }

    SdgoOcCustomClientG1NativeV1 state{};
    const bool g1 = view == 2 || view == 4;
    if (!g1Native.Read(state) || !state.page || !state.bar || !state.tab_shown || !state.page_idle ||
        !state.switch_g1 || !state.switch_g2 || bool(state.active) != g1 || GetTickCount64() - clickedAt < 2000)
        return SDGO_GAME_TEST_WAIT;

    if (!capturePending) {
        const auto name = std::wstring(context->directory) + L"\\factory-view-" + std::to_wstring(view) + L".bmp";
        if (!quietGraphics.CaptureNextFrame(name.c_str())) return SDGO_GAME_TEST_FAIL;
        capturePending = true;
        return SDGO_GAME_TEST_WAIT;
    }
    const auto captured = quietGraphics.CaptureFrameResult();
    if (captured == E_PENDING) return SDGO_GAME_TEST_WAIT;
    if (FAILED(captured)) return SDGO_GAME_TEST_FAIL;

    unsigned icons = 0, buttons = 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (ControlShown(ReadOr<void*>(state.bar, 0x1D8 + i * 4, nullptr))) icons |= 1u << i;
        if (ControlShown(ReadOr<void*>(state.bar, 0x178 + i * 4, nullptr))) buttons |= 1u << i;
    }
    if (view == 1) {
        nativeIcons = icons;
        nativeButtons = buttons;
    }
    const bool correctBar = g1 ? buttons && icons == 0
                               : (icons & buttons) && icons == nativeIcons && buttons == nativeButtons;
    const bool g2Enabled = ReadOr<uint8_t>(state.switch_g2, 0x167, 0) != 0;
    const bool g1Enabled = ReadOr<uint8_t>(state.switch_g1, 0x167, 0) != 0;
    // The dedicated test server offers both versions for this unit.
    const bool correctSwitch = g2Enabled == g1 && g1Enabled != g1;
    char message[160];
    sprintf_s(message, "parts view=%u g1=%u category_buttons=%u icons=%u g2_enabled=%u g1_enabled=%u",
              view, state.active, buttons, icons, g2Enabled, g1Enabled);
    context->Observe(message);
    context->Assert(correctBar, "G2 retains its original category icons and G1 shows text categories");
    context->Assert(correctSwitch, "the enabled switch matches the displayed parts mode");
    if (!correctBar || !correctSwitch) return SDGO_GAME_TEST_FAIL;
    if (view == 4) return SDGO_GAME_TEST_PASS;

    if (!NativeUiEvent(state.page, 0x101, g1 ? L"OcAttrG2Btn" : L"OcAttrG1Btn",
                       g1 ? state.switch_g2 : state.switch_g1)) return SDGO_GAME_TEST_FAIL;
    capturePending = false;
    ++view;
    clickedAt = GetTickCount64();
    return SDGO_GAME_TEST_WAIT;
}
const SdgoGameTestCaseV1 factoryPartsEntryCase{"pages.factory.parts_switch", 180000, &FactoryPartsEntry};
