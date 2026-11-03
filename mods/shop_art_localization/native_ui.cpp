#include "native_ui.h"

#include <windows.h>

#include <array>
#include <cmath>
#include <cstring>
#include <string>

namespace sdgo::shop_art::ui {
namespace {

// EF39FC ends at the next native table, EF3A8C: 0x90 bytes / 36 slots.
// The private table preserves every slot except the per-control draw entry.
constexpr size_t kVtableBytes = 0x90;
constexpr size_t kDrawSlot = 0x20 / sizeof(void*);
constexpr wchar_t kSupportedSprite[] = L"sprs\\17825.spr";
constexpr wchar_t kCaption[] = L"上士及以上";
constexpr uint32_t kTextColor = 0xFFCD730F;
constexpr uint32_t kShadowColor = 0xFF000000;
constexpr int kOverlayX = 0;
constexpr int kOverlayY = 122;
constexpr int kOverlayWidth = 102;
constexpr int kOverlayHeight = 14;

Bindings g_bindings{};
alignas(void*) std::array<uint8_t, kVtableBytes> g_backgroundVtable{};
bool g_rendererReady = false;

template<class T>
T Read(const void* pointer, size_t offset = 0) {
    T value{};
    memcpy(&value, static_cast<const uint8_t*>(pointer) + offset, sizeof(value));
    return value;
}

template<class T>
void Write(void* pointer, size_t offset, T value) {
    memcpy(static_cast<uint8_t*>(pointer) + offset, &value, sizeof(value));
}

NativeString View(const std::wstring& text) {
    NativeString value;
    value.length = static_cast<uint32_t>(text.size());
    if (text.size() < 8) {
        memcpy(value.data.small, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    } else {
        value.data.pointer = text.c_str();
        value.capacity = value.length;
    }
    return value;
}

std::wstring NativeText(const NativeString& value) {
    if (value.length > 260 || value.capacity < value.length) return {};
    const auto* text = value.capacity < 8 ? value.data.small : value.data.pointer;
    if (!text) return {};
    return std::wstring(text, text + value.length);
}

bool SameSprite(void* control) {
    auto** begin = Read<uint8_t**>(control, 0x1A4);
    auto** end = Read<uint8_t**>(control, 0x1A8);
    if (!begin || end != begin + 1 || !*begin) return false;
    const auto path = NativeText(Read<NativeString>(*begin, 4));
    return _wcsicmp(path.c_str(), kSupportedSprite) == 0;
}

bool ExpectedGeometry(void* control) {
    const float x = Read<float>(control, 0x148);
    const float y = Read<float>(control, 0x14C);
    return std::isfinite(x) && std::isfinite(y) && x >= 0 && y >= 0 &&
           x <= 1024 && y <= 768 && Read<int>(control, 0x154) == 137 &&
           Read<int>(control, 0x158) == 154;
}

uint32_t Font(void* control) {
    auto** begin = Read<uint8_t**>(control, 0x190);
    auto** end = Read<uint8_t**>(control, 0x194);
    if (!begin || begin == end || end < begin || end - begin > 32 || !*begin) return 0;
    return Read<uint32_t>(*begin, 0x6C);
}

void __fastcall DrawBackground(void* control, void*) {
    if (!control || !g_rendererReady) return;
    __try {
        if (!Read<uint8_t>(control, 0x162) || !Read<uint8_t>(control, 0x163) ||
            !g_bindings.drawLock(control)) return;
        __try {
            float parentX = 0;
            float parentY = 0;
            if (auto* parent = Read<void*>(control, 0x15C)) {
                if (auto* grandparent = Read<void*>(parent, 0x15C)) {
                    parentX += Read<float>(grandparent, 0x148);
                    parentY += Read<float>(grandparent, 0x14C);
                }
                parentX += Read<float>(parent, 0x148);
                parentY += Read<float>(parent, 0x14C);
            }
            // C753E0 consumes the final byte as the D3D vertex alpha channel.
            // Stock C6B630 passes decimal 100 (0x64), which is translucent;
            // this background must be opaque to remove the baked orange glyphs.
            g_bindings.drawQuad(control, static_cast<uint8_t*>(control) + 0x138,
                                parentX, parentY, 0, 0x000000, 0xFF);
        } __finally {
            g_bindings.drawUnlock(control);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool InitializeRenderer(const Bindings& bindings) {
    if (g_rendererReady) {
        return g_bindings.buttonVtable == bindings.buttonVtable &&
               g_bindings.drawLock == bindings.drawLock &&
               g_bindings.drawUnlock == bindings.drawUnlock &&
               g_bindings.drawQuad == bindings.drawQuad;
    }
    if (!bindings.buttonVtable || !bindings.drawLock || !bindings.drawUnlock ||
        !bindings.drawQuad) return false;
    memcpy(g_backgroundVtable.data(), bindings.buttonVtable, g_backgroundVtable.size());
    reinterpret_cast<void**>(g_backgroundVtable.data())[kDrawSlot] =
        reinterpret_cast<void*>(&DrawBackground);
    g_bindings = bindings;
    g_rendererReady = true;
    return true;
}

const std::array<std::wstring, 8> kBackgroundNames = {
    L"ModShopArtBackground0", L"ModShopArtBackground1",
    L"ModShopArtBackground2", L"ModShopArtBackground3",
    L"ModShopArtBackground4", L"ModShopArtBackground5",
    L"ModShopArtBackground6", L"ModShopArtBackground7"
};
const std::array<std::wstring, 8> kTextNames = {
    L"ModShopArtText0", L"ModShopArtText1", L"ModShopArtText2", L"ModShopArtText3",
    L"ModShopArtText4", L"ModShopArtText5", L"ModShopArtText6", L"ModShopArtText7"
};

} // namespace

const wchar_t* BackgroundControlName(int row) {
    return row >= 0 && row < static_cast<int>(kBackgroundNames.size())
        ? kBackgroundNames[static_cast<size_t>(row)].c_str() : L"";
}

const wchar_t* TextControlName(int row) {
    return row >= 0 && row < static_cast<int>(kTextNames.size())
        ? kTextNames[static_cast<size_t>(row)].c_str() : L"";
}

bool Overlay::Bind(const SdgoApi* api, SdgoMod* mod) {
    if (!api || !api->Symbol) return false;
    Bindings bindings;
    bindings.add = reinterpret_cast<Bindings::Add>(
        api->Symbol(mod, "shop_art_localization.ui_add_button"));
    bindings.find = reinterpret_cast<Bindings::Find>(
        api->Symbol(mod, "shop_art_localization.ui_find_control"));
    bindings.text = reinterpret_cast<Bindings::Text>(
        api->Symbol(mod, "shop_art_localization.ui_text_item"));
    bindings.buttonVtable =
        api->Symbol(mod, "shop_art_localization.ui_button_vtable");
    bindings.drawLock = reinterpret_cast<Bindings::DrawLock>(
        api->Symbol(mod, "shop_art_localization.ui_draw_lock"));
    bindings.drawUnlock = reinterpret_cast<Bindings::DrawUnlock>(
        api->Symbol(mod, "shop_art_localization.ui_draw_unlock"));
    bindings.drawQuad = reinterpret_cast<Bindings::DrawQuad>(
        api->Symbol(mod, "shop_art_localization.ui_draw_quad"));
    return Bind(bindings);
}

bool Overlay::Bind(const Bindings& bindings) {
    if (!bindings.add || !bindings.find || !bindings.text ||
        !InitializeRenderer(bindings)) return false;
    bindings_ = bindings;
    return true;
}

bool Overlay::Attach(void* service) {
    if (!service) return false;
    auto* dialog = Read<void*>(service, 0x64);
    if (!dialog) return false;
    if (service_ == service && dialog_ == dialog) return true;
    service_ = service;
    dialog_ = dialog;
    rows_ = {};
    return true;
}

void Overlay::Hide(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    __try {
        HideUnsafe(row);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rows_[static_cast<size_t>(row)] = {};
    }
}

bool Overlay::RowCurrent(int row) const {
    if (row < 0 || row >= static_cast<int>(rows_.size()) ||
        !service_ || !dialog_ || Read<void*>(service_, 0x64) != dialog_) return false;
    const auto& controls = rows_[static_cast<size_t>(row)];
    if (!controls.background || !controls.text) return false;
    const std::wstring backgroundKey = BackgroundControlName(row);
    const std::wstring textKey = TextControlName(row);
    const auto backgroundName = View(backgroundKey);
    const auto textName = View(textKey);
    return bindings_.find(dialog_, &backgroundName, 4) == controls.background &&
           bindings_.find(dialog_, &textName, 4) == controls.text &&
           Read<void*>(controls.background) == g_backgroundVtable.data() &&
           Read<void*>(controls.text) == bindings_.buttonVtable &&
           Read<void*>(controls.background, 0x15C) == dialog_ &&
           Read<void*>(controls.text, 0x15C) == dialog_;
}

void Overlay::HideUnsafe(int row) {
    auto& controls = rows_[static_cast<size_t>(row)];
    if (!controls.background && !controls.text) return;
    if (!RowCurrent(row)) {
        controls = {};
        return;
    }
    Write<uint8_t>(controls.background, 0x162, 0);
    Write<uint8_t>(controls.text, 0x162, 0);
}

bool Overlay::EnsureRow(int row, void* goods) {
    auto& controls = rows_[static_cast<size_t>(row)];
    if (controls.background || controls.text) {
        if (RowCurrent(row)) return controls.goods == goods;
        controls = {};
    }

    const float goodsX = Read<float>(goods, 0x148);
    const float goodsY = Read<float>(goods, 0x14C);
    const int x = static_cast<int>(goodsX) + kOverlayX;
    const int y = static_cast<int>(goodsY) + kOverlayY;

    const std::wstring backgroundName = BackgroundControlName(row);
    auto nativeName = View(backgroundName);
    bindings_.add(dialog_, &nativeName, x, y, kOverlayWidth, kOverlayHeight, 0);
    auto* background = bindings_.find(dialog_, &nativeName, 4);
    const auto backgroundVtable = background ? Read<void*>(background) : nullptr;
    if (!background ||
        (backgroundVtable != bindings_.buttonVtable &&
         backgroundVtable != g_backgroundVtable.data()) ||
        Read<void*>(background, 0x15C) != dialog_) return false;
    Write<uint8_t>(background, 0x162, 0);
    Write<uint8_t>(background, 0x167, 0);
    Write<uint8_t>(background, 0x168, 0);
    Write<uint32_t>(background, 0x1CC, 0);

    const std::wstring textName = TextControlName(row);
    nativeName = View(textName);
    bindings_.add(dialog_, &nativeName, x, y, kOverlayWidth, kOverlayHeight, 0);
    auto* text = bindings_.find(dialog_, &nativeName, 4);
    if (!text || Read<void*>(text) != bindings_.buttonVtable ||
        Read<void*>(text, 0x15C) != dialog_) return false;
    Write<uint8_t>(text, 0x162, 0);
    Write<uint8_t>(text, 0x167, 0);
    Write<uint8_t>(text, 0x168, 0);

    const std::wstring caption = kCaption;
    const auto nativeCaption = View(caption);
    uint32_t font = 1;
    const std::wstring sourceName = L"GoodsStatic" + std::to_wstring(row);
    const auto nativeSourceName = View(sourceName);
    if (auto* source = bindings_.find(dialog_, &nativeSourceName, 3)) {
        if (const auto nativeFont = Font(source)) font = nativeFont;
    }
    bindings_.text(text, 0, &nativeCaption, 2, 0, kOverlayWidth - 4,
                   kOverlayHeight, kTextColor, kShadowColor, 0, font, 1);
    Write<void*>(background, 0, g_backgroundVtable.data());
    controls = {background, text, goods};
    return true;
}

void Overlay::UpdateUnsafe(void* service, int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    if (!Attach(service)) return;
    const std::wstring goodsName = L"Goods3DRender" + std::to_wstring(row);
    const auto nativeName = View(goodsName);
    auto* goods = bindings_.find(dialog_, &nativeName, 5);
    if (!goods || Read<void*>(goods, 0x15C) != dialog_ ||
        !ExpectedGeometry(goods) || !SameSprite(goods) ||
        !EnsureRow(row, goods)) {
        Hide(row);
        return;
    }
    const auto visible = Read<uint8_t>(goods, 0x162) ? uint8_t{1} : uint8_t{0};
    const auto& controls = rows_[static_cast<size_t>(row)];
    Write<uint8_t>(controls.background, 0x167, 0);
    Write<uint8_t>(controls.text, 0x167, 0);
    Write<uint8_t>(controls.background, 0x162, visible);
    Write<uint8_t>(controls.text, 0x162, visible);
}

void Overlay::Update(void* service, int row) {
    __try {
        UpdateUnsafe(service, row);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Hide(row);
    }
}

void Overlay::Clear(void* service, int row) {
    __try {
        if (row < 0 || row >= static_cast<int>(rows_.size()) || service != service_) return;
        if (!service_ || Read<void*>(service_, 0x64) != dialog_) {
            rows_[static_cast<size_t>(row)] = {};
            return;
        }
        HideUnsafe(row);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (row >= 0 && row < static_cast<int>(rows_.size()))
            rows_[static_cast<size_t>(row)] = {};
    }
}

} // namespace sdgo::shop_art::ui
