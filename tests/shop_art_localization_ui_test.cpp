#include "mods/shop_art_localization/native_ui.h"

#include <windows.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using sdgo::shop_art::ui::Bindings;
using sdgo::shop_art::ui::NativeString;

struct Block {
    std::array<uint8_t, 0x300> bytes{};
};

struct TextCall {
    std::wstring value;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    uint32_t color = 0;
    uint32_t shadow = 0;
    uint8_t shadowEnabled = 0;
    uint32_t font = 0;
    int align = 0;
};

std::array<void*, 36> g_buttonVtable{};
std::map<std::wstring, std::unique_ptr<Block>> g_controls;
std::map<void*, TextCall> g_textCalls;
std::vector<std::wstring> g_addOrder;
void* g_dialog = nullptr;
int g_lockCount = 0;
int g_unlockCount = 0;
int g_quadCount = 0;
uint32_t g_quadColor = 0;
uint8_t g_quadAlpha = 0;

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

std::wstring ReadString(const NativeString* value) {
    const auto* text = value->capacity < 8 ? value->data.small : value->data.pointer;
    return std::wstring(text, text + value->length);
}

NativeString MakeString(const wchar_t* text) {
    const std::wstring value = text;
    NativeString result;
    result.length = static_cast<uint32_t>(value.size());
    if (value.size() < 8) {
        memcpy(result.data.small, value.c_str(), (value.size() + 1) * sizeof(wchar_t));
    } else {
        result.data.pointer = text;
        result.capacity = result.length;
    }
    return result;
}

uint32_t __fastcall Add(void* dialog, void*, const NativeString* name, int x, int y,
                        int width, int height, uint32_t) {
    const auto key = ReadString(name);
    if (g_controls.count(key)) return 0;
    auto control = std::make_unique<Block>();
    Write<void*>(control->bytes.data(), 0, g_buttonVtable.data());
    Write<LONG>(control->bytes.data(), 0x138, x);
    Write<LONG>(control->bytes.data(), 0x13C, y);
    Write<LONG>(control->bytes.data(), 0x140, x + width);
    Write<LONG>(control->bytes.data(), 0x144, y + height);
    Write<float>(control->bytes.data(), 0x148, static_cast<float>(x));
    Write<float>(control->bytes.data(), 0x14C, static_cast<float>(y));
    Write<int>(control->bytes.data(), 0x154, width);
    Write<int>(control->bytes.data(), 0x158, height);
    Write<void*>(control->bytes.data(), 0x15C, dialog);
    Write<uint8_t>(control->bytes.data(), 0x162, 1);
    Write<uint8_t>(control->bytes.data(), 0x163, 1);
    Write<uint8_t>(control->bytes.data(), 0x167, 1);
    g_addOrder.push_back(key);
    g_controls.emplace(key, std::move(control));
    return 0;
}

void* __fastcall Find(void*, void*, const NativeString* name, int) {
    const auto found = g_controls.find(ReadString(name));
    return found == g_controls.end() ? nullptr : found->second->bytes.data();
}

void __fastcall Text(void* control, void*, int, const NativeString* value, int x, int y,
                     int width, int height, uint32_t color, uint32_t shadow,
                     uint8_t shadowEnabled, uint32_t font, int align) {
    g_textCalls[control] = {
        ReadString(value), x, y, width, height, color, shadow, shadowEnabled, font, align
    };
}

int __fastcall DrawLock(void*, void*) {
    ++g_lockCount;
    return 1;
}

void __fastcall DrawUnlock(void*, void*) {
    ++g_unlockCount;
}

void __fastcall DrawQuad(void*, void*, const void*, float, float, uint32_t,
                         uint32_t color, uint8_t alpha) {
    ++g_quadCount;
    g_quadColor = color;
    g_quadAlpha = alpha;
}

Block* AddFixture(const wchar_t* name, int type, int x, int y, int width, int height) {
    auto value = MakeString(name);
    Add(g_dialog, nullptr, &value, x, y, width, height, 0);
    auto* block = g_controls.at(name).get();
    Write<int>(block->bytes.data(), 0x12C, type);
    return block;
}

void SetSkin(Block& control, const wchar_t* path,
             std::array<uint8_t, 128>& skin,
             std::array<uint8_t*, 1>& skins) {
    auto native = MakeString(path);
    memcpy(skin.data() + 4, &native, sizeof(native));
    skins[0] = skin.data();
    Write<uint8_t**>(control.bytes.data(), 0x1A4, skins.data());
    Write<uint8_t**>(control.bytes.data(), 0x1A8, skins.data() + 1);
}

} // namespace

int main() {
    Block dialog;
    Block service;
    g_dialog = dialog.bytes.data();
    Write<void*>(service.bytes.data(), 0x64, g_dialog);

    Block* goods = AddFixture(L"Goods3DRender0", 5, 27, 130, 137, 154);
    std::array<uint8_t, 128> skin{};
    std::array<uint8_t*, 1> skins{};
    SetSkin(*goods, L"sprs\\17825.spr", skin, skins);

    Block* source = AddFixture(L"GoodsStatic0", 3, 26, 271, 139, 12);
    std::array<uint8_t, 128> textItem{};
    Write<uint32_t>(textItem.data(), 0x6C, 42);
    std::array<uint8_t*, 1> textItems{textItem.data()};
    Write<uint8_t**>(source->bytes.data(), 0x190, textItems.data());
    Write<uint8_t**>(source->bytes.data(), 0x194, textItems.data() + 1);

    Bindings bindings;
    bindings.add = reinterpret_cast<Bindings::Add>(&Add);
    bindings.find = reinterpret_cast<Bindings::Find>(&Find);
    bindings.text = reinterpret_cast<Bindings::Text>(&Text);
    bindings.buttonVtable = g_buttonVtable.data();
    bindings.drawLock = reinterpret_cast<Bindings::DrawLock>(&DrawLock);
    bindings.drawUnlock = reinterpret_cast<Bindings::DrawUnlock>(&DrawUnlock);
    bindings.drawQuad = reinterpret_cast<Bindings::DrawQuad>(&DrawQuad);

    sdgo::shop_art::ui::Overlay overlay;
    assert(overlay.Bind(bindings));
    overlay.Update(service.bytes.data(), 0);

    assert(g_addOrder[g_addOrder.size() - 2] == L"ModShopArtBackground0");
    assert(g_addOrder.back() == L"ModShopArtText0");
    auto* background = g_controls.at(L"ModShopArtBackground0")->bytes.data();
    auto* text = g_controls.at(L"ModShopArtText0")->bytes.data();
    assert(Read<void*>(background) != g_buttonVtable.data());
    assert(Read<void*>(text) == g_buttonVtable.data());
    assert(Read<float>(background, 0x148) == 27.0f);
    assert(Read<float>(background, 0x14C) == 252.0f);
    assert(Read<int>(background, 0x154) == 102);
    assert(Read<int>(background, 0x158) == 14);
    assert(Read<uint8_t>(background, 0x162) == 1);
    assert(Read<uint8_t>(background, 0x167) == 0);
    assert(Read<uint8_t>(text, 0x162) == 1);
    assert(Read<uint8_t>(text, 0x167) == 0);
    const auto call = g_textCalls.at(text);
    assert(call.value == L"上士及以上");
    assert(call.color == 0xFFCD730F);
    assert(call.shadowEnabled == 0);
    assert(call.font == 42);

    using Draw = void(__thiscall*)(void*);
    reinterpret_cast<Draw>(Read<void**>(background)[8])(background);
    assert(g_lockCount == 1 && g_unlockCount == 1 && g_quadCount == 1);
    assert(g_quadColor == 0 && g_quadAlpha == 0xFF);

    Write<uint8_t>(goods->bytes.data(), 0x162, 0);
    overlay.Update(service.bytes.data(), 0);
    assert(Read<uint8_t>(background, 0x162) == 0);
    assert(Read<uint8_t>(text, 0x162) == 0);

    Write<uint8_t>(goods->bytes.data(), 0x162, 1);
    SetSkin(*goods, L"sprs\\4724.spr", skin, skins);
    overlay.Update(service.bytes.data(), 0);
    assert(Read<uint8_t>(background, 0x162) == 0);
    assert(Read<uint8_t>(text, 0x162) == 0);

    SetSkin(*goods, L"sprs\\17825.spr", skin, skins);
    overlay.Update(service.bytes.data(), 0);
    assert(Read<uint8_t>(background, 0x162) == 1);
    assert(Read<uint8_t>(text, 0x162) == 1);

    auto oldBackgroundControl =
        std::move(g_controls.at(L"ModShopArtBackground0"));
    auto oldTextControl = std::move(g_controls.at(L"ModShopArtText0"));
    const auto* oldBackground = oldBackgroundControl->bytes.data();
    const auto* oldText = oldTextControl->bytes.data();
    g_controls.erase(L"ModShopArtBackground0");
    g_controls.erase(L"ModShopArtText0");
    overlay.Update(service.bytes.data(), 0);
    background = g_controls.at(L"ModShopArtBackground0")->bytes.data();
    text = g_controls.at(L"ModShopArtText0")->bytes.data();
    assert(background != oldBackground);
    assert(text != oldText);
    assert(Read<void*>(background) != g_buttonVtable.data());
    assert(Read<void*>(text) == g_buttonVtable.data());
    assert(Read<void*>(background, 0x15C) == g_dialog);
    assert(Read<void*>(text, 0x15C) == g_dialog);
    assert(Read<uint8_t>(background, 0x162) == 1);
    assert(Read<uint8_t>(text, 0x162) == 1);

    overlay.Clear(service.bytes.data(), 0);
    assert(Read<uint8_t>(background, 0x162) == 0);
    assert(Read<uint8_t>(text, 0x162) == 0);

    Block replacementDialog;
    Write<void*>(service.bytes.data(), 0x64, replacementDialog.bytes.data());
    Write<uint8_t>(background, 0x162, 1);
    Write<uint8_t>(text, 0x162, 1);
    overlay.Clear(service.bytes.data(), 0);
    assert(Read<uint8_t>(background, 0x162) == 1);
    assert(Read<uint8_t>(text, 0x162) == 1);
    return 0;
}
