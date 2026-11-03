#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace sdgo::friend_fix {
// SP_GetFriendList binds INT, STR(17), ... into a packed 0x5d-byte row.
inline constexpr size_t kFriendRowBytes = 0x5d;
inline constexpr size_t kFriendNameOffset = 4;
inline constexpr size_t kFriendNameBytes = 17;

inline bool ReadDamagedName(const void* row, std::uint32_t* id) noexcept {
    __try {
        if (!row || !id) return false;
        const auto* bytes = static_cast<const unsigned char*>(row);
        std::memcpy(id, bytes, sizeof(*id));
        if (!*id || *id > 0x7fffffffu) return false;
        for (size_t i = 0; i < kFriendNameBytes - 1; ++i) {
            const char value = static_cast<char>(bytes[kFriendNameOffset + i]);
            if (!value) break;
            if (value == '?') return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool EncodeFriendName(const std::wstring& value, UINT codePage,
                             std::array<char, kFriendNameBytes>& encoded) {
    if (value.empty() || value.find(L'\0') != std::wstring::npos ||
        !codePage || !IsValidCodePage(codePage)) return false;
    size_t length = (std::min)(value.size(), kFriendNameBytes - 1);
    while (length) {
        if (value[length - 1] >= 0xd800 && value[length - 1] <= 0xdbff) {
            --length;
            continue;
        }
        std::array<char, kFriendNameBytes> candidate{};
        BOOL substituted = FALSE;
        const auto flags = codePage == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
        const int count = WideCharToMultiByte(codePage, flags, value.data(),
            static_cast<int>(length), candidate.data(),
            static_cast<int>(kFriendNameBytes - 1), nullptr,
            codePage == CP_UTF8 ? nullptr : &substituted);
        if (count > 0 && !substituted) { encoded = candidate; return true; }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || substituted) return false;
        --length;
    }
    return false;
}

inline bool StoreFriendName(void* row, const std::array<char, kFriendNameBytes>& name) noexcept {
    __try {
        std::memcpy(static_cast<unsigned char*>(row) + kFriendNameOffset,
                    name.data(), name.size());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

template<class Fetch>
unsigned RepairFriendNames(void* rows, unsigned count, UINT codePage, Fetch&& fetch) {
    unsigned repaired = 0;
    for (unsigned i = 0; i < count; ++i) {
        auto* row = static_cast<unsigned char*>(rows) + i * kFriendRowBytes;
        std::uint32_t id = 0;
        if (!ReadDamagedName(row, &id)) continue;
        std::wstring canonical;
        std::array<char, kFriendNameBytes> encoded{};
        if (fetch(id, canonical) && EncodeFriendName(canonical, codePage, encoded) &&
            StoreFriendName(row, encoded)) ++repaired;
    }
    return repaired;
}
}
