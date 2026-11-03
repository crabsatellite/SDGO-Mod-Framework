#pragma once

#include "sdgo/sdgo_api.h"

#include <array>
#include <cstdint>
#include <string>

namespace sdgo::shop_art::ui {

struct NativeString {
    union {
        wchar_t small[8];
        const wchar_t* pointer;
    } data{};
    uint32_t length = 0;
    uint32_t capacity = 7;
};
static_assert(sizeof(NativeString) == 24, "host UTF-16 string ABI");

struct Bindings {
    using Add = uint32_t(__thiscall*)(void*, const NativeString*, int, int, int, int, uint32_t);
    using Find = void*(__thiscall*)(void*, const NativeString*, int);
    using Text = void(__thiscall*)(void*, int, const NativeString*, int, int, int, int,
                                  uint32_t, uint32_t, uint8_t, uint32_t, int);
    using DrawLock = int(__thiscall*)(void*);
    using DrawUnlock = void(__thiscall*)(void*);
    using DrawQuad = void(__thiscall*)(void*, const void*, float, float, uint32_t,
                                      uint32_t, uint8_t);

    Add add = nullptr;
    Find find = nullptr;
    Text text = nullptr;
    void* buttonVtable = nullptr;
    DrawLock drawLock = nullptr;
    DrawUnlock drawUnlock = nullptr;
    DrawQuad drawQuad = nullptr;
};

class Overlay {
public:
    bool Bind(const SdgoApi*, SdgoMod*);
    bool Bind(const Bindings&);
    void Update(void* service, int row);
    void Clear(void* service, int row);

private:
    struct Row {
        void* background = nullptr;
        void* text = nullptr;
        void* goods = nullptr;
    };

    bool Attach(void* service);
    bool EnsureRow(int row, void* goods);
    bool RowCurrent(int row) const;
    void HideUnsafe(int row);
    void UpdateUnsafe(void* service, int row);
    void Hide(int row);

    Bindings bindings_{};
    void* service_ = nullptr;
    void* dialog_ = nullptr;
    std::array<Row, 8> rows_{};
};

const wchar_t* BackgroundControlName(int row);
const wchar_t* TextControlName(int row);

} // namespace sdgo::shop_art::ui
