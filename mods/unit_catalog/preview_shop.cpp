#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "preview_runtime.h"
#include "preview_shop.h"

namespace sdgo::unit_preview {
namespace {

using ShopModalInitFn = void(__thiscall*)(std::uint8_t*, std::uint32_t);
using ShopUnitViewFn = void(__thiscall*)(std::uint8_t*, std::uint32_t);
using ShopSetButtonsFn = void(__thiscall*)(std::uint8_t*);

ShopModalInitFn g_modalInit = nullptr;
ShopUnitViewFn g_unitView = nullptr;
ShopSetButtonsFn g_setButtons = nullptr;

constexpr std::size_t kVisibleOffset = 0x162;
constexpr std::size_t kModelPane = 0x8c;
constexpr std::size_t kBuyButton = 0xcc;
constexpr std::size_t kCashButton = 0xd0;

// These controls describe the selected real SKU, not the private candidate.
// Hide them instead of presenting the original item's metadata beside the
// candidate model. The Cancel button and the native 3D pane remain available.
constexpr std::size_t kSkuControls[] = {
    0x68, 0x6c, 0x70, 0x74, 0x78, 0x7c, 0x80, 0x84, 0x88,
    0x90, 0x94, 0x98, 0x9c, 0xa0, 0xa4, 0xa8, 0xac, 0xb0,
    0xb4, 0xb8, 0xbc, 0xc0, 0xc4, 0xc8,
};

bool ReadControl(std::uint8_t* surface, std::size_t offset, std::uint8_t** control)
{
    if (!surface || !control)
        return false;
    __try {
        *control = *reinterpret_cast<std::uint8_t**>(surface + offset);
        return *control != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *control = nullptr;
        return false;
    }
}

bool SetVisible(std::uint8_t* control, bool visible)
{
    if (!control)
        return false;
    __try {
        control[kVisibleOffset] = visible ? 1 : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsVisible(std::uint8_t* control)
{
    if (!control)
        return false;
    __try {
        return control[kVisibleOffset] == 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void HideControl(std::uint8_t* surface, std::size_t offset)
{
    std::uint8_t* control = nullptr;
    if (ReadControl(surface, offset, &control))
        SetVisible(control, false);
}

void HideSkuMetadata(std::uint8_t* surface)
{
    for (const std::size_t offset : kSkuControls)
        HideControl(surface, offset);
}

void HidePurchaseControls(std::uint8_t* surface)
{
    // The 1127 detail dialog names these controls BuyBtn and CashBtn. It has
    // no GiftBtn; GiftBtn belongs to the separate eight-row shop list.
    HideControl(surface, kBuyButton);
    HideControl(surface, kCashButton);
}

void RenderCandidate(std::uint8_t* surface)
{
    std::uint32_t unit = 0;
    std::uint32_t category = 0;
    if (!surface || !Target(&unit, &category) || !unit || !category) {
        Failed("shop", "private preview target is unavailable");
        return;
    }

    std::uint8_t* pane = nullptr;
    if (!ReadControl(surface, kModelPane, &pane)) {
        Failed("shop", "native shop model pane is unavailable");
        return;
    }

    // The original renderer only sets the pane visible after it resolved a
    // type-2 unit record and submitted the native model/sprite path. Clearing
    // it first makes the post-call visibility check specific to this target.
    if (!SetVisible(pane, false)) {
        Failed("shop", "native shop model pane visibility is inaccessible");
        return;
    }

    g_unitView(surface, unit);

    std::uint8_t* renderedPane = nullptr;
    if (!ReadControl(surface, kModelPane, &renderedPane) ||
        renderedPane != pane || !IsVisible(renderedPane)) {
        Failed("shop", "native shop unit renderer did not expose the target model pane");
        return;
    }

    Seen("shop", unit, "native-secret-store-1127-unit-model-pane");
}

void __fastcall SetButtons(std::uint8_t* surface, void*)
{
    g_setButtons(surface);
    HidePurchaseControls(surface);
}

void __fastcall ModalInit(std::uint8_t* surface, void*, std::uint32_t argument)
{
    g_modalInit(surface, argument);

    // Preserve the game's complete dialog construction and native renderer.
    // Only remove fields whose values still belong to the selected real SKU.
    HideSkuMetadata(surface);
    HidePurchaseControls(surface);
    RenderCandidate(surface);
    HidePurchaseControls(surface);
}

} // namespace

bool InstallShop(const SdgoApi* api, SdgoMod* mod)
{
    if (!api || !api->Symbol || !api->HookCode) {
        Failed("shop", "mod framework hook API is unavailable");
        return false;
    }

    g_unitView = reinterpret_cast<ShopUnitViewFn>(
        api->Symbol(mod, "unit_preview.shop_unit_view"));
    if (!g_unitView) {
        Failed("shop", "verified native shop unit renderer symbol is unavailable");
        return false;
    }

    if (!api->HookCode(mod, "unit_preview.shop_set_buttons",
                       reinterpret_cast<void*>(&SetButtons),
                       reinterpret_cast<void**>(&g_setButtons)) ||
        !api->HookCode(mod, "unit_preview.shop_modal_init",
                       reinterpret_cast<void*>(&ModalInit),
                       reinterpret_cast<void**>(&g_modalInit))) {
        Failed("shop", "native shop preview hooks could not be installed");
        return false;
    }

    return true;
}

} // namespace sdgo::unit_preview
