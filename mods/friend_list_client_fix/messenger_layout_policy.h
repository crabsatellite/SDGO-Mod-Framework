#pragma once

#include <cstddef>
#include <cstdint>

namespace friend_list_client_fix
{
    inline constexpr char kAssignSymbol[] =
        "friend_list.client.native_string_assign";
    inline constexpr char kServiceLayoutCallSymbol[] =
        "friend_layout.service_assign_call";
    inline constexpr char kPopupLayoutCallSymbol[] =
        "friend_layout.popup_assign_call";
    inline constexpr char kServiceLayoutReturnSymbol[] =
        "friend_layout.service_assign_return";
    inline constexpr char kPopupLayoutReturnSymbol[] =
        "friend_layout.popup_assign_return";
    inline constexpr char kOriginalLayoutSymbol[] =
        "friend_layout.original_1364";

    inline constexpr std::uint32_t kAssignRva = 0x000026F0;
    inline constexpr std::uint32_t kServiceLayoutCallRva = 0x00559911;
    inline constexpr std::uint32_t kPopupLayoutCallRva = 0x00561F1E;
    inline constexpr std::uint32_t kServiceLayoutReturnRva = 0x00559916;
    inline constexpr std::uint32_t kPopupLayoutReturnRva = 0x00561F23;
    inline constexpr std::uint32_t kOriginalLayoutRva = 0x00A547F8;
    inline constexpr std::uint32_t kExchangeLayoutReturnRva = 0x00165DF5;

    inline constexpr std::size_t kLayoutLength = 13;
    inline constexpr wchar_t kOriginalLayoutPath[] = L"uirs\\1364.uir";
    inline constexpr wchar_t kFriendLayoutPath[] = L"uirs\\0553.uir";

    constexpr bool ShouldRedirectLayout(
        const wchar_t* text,
        const wchar_t* originalText,
        std::size_t length)
    {
        return text == originalText && length == kLayoutLength;
    }
}
