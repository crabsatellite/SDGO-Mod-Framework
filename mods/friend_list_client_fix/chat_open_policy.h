#pragma once

#include <cstdint>

namespace friend_list_client_fix
{
    inline constexpr char kLayoutSymbol[] = "friend_list.client.chat_open_layout";
    inline constexpr char kGateSymbol[] = "friend_list.client.offline_chat_gate";

    inline constexpr std::uint32_t kLayoutRva = 0x00564545;
    inline constexpr std::uint32_t kGateRva = 0x00564549;

    inline constexpr std::uint8_t kAcceptedRelationship = 7;

    struct FriendRow
    {
        std::uint32_t identity;
        std::uint8_t nameLead;
        std::uint8_t relationship;
        std::uint32_t presence;
    };

    constexpr bool IsConfirmedOfflineFriend(const FriendRow& row)
    {
        return row.identity != 0 && row.nameLead != 0 &&
               row.relationship == kAcceptedRelationship && row.presence == 0;
    }

    constexpr bool OpensNativeConversation(bool treeHasSelection, const FriendRow* row)
    {
        if (!treeHasSelection || !row) return false;
        return row->presence != 0 || IsConfirmedOfflineFriend(*row);
    }
}
