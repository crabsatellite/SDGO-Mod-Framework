#pragma once
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace friend_list_client_fix {
// CServiceCommunitySubMenu_New is the home-screen USER LIST context menu.
// The older MessengerFriendSubPopUp is a separate native service.
inline constexpr char kCommunityLabelsSymbol[] = "friend_list.client.community_menu_labels";
inline constexpr char kCommunityTextSymbol[] = "friend_list.client.community_text_item";
struct MenuString {
    wchar_t text[8]{};
    std::uint32_t length = 0;
    std::uint32_t capacity = 7;
};
using MenuText = void(__thiscall*)(void*, int, const MenuString*,
    int,int,int,int,std::uint32_t,std::uint32_t,std::uint8_t,std::uint32_t,int);
template<class T> T MenuRead(const void* object, std::size_t offset) {
    T value{};std::memcpy(&value,static_cast<const std::uint8_t*>(object)+offset,sizeof(value));return value;
}
inline void SetMenuCaption(void* button, const wchar_t* caption, MenuText write) {
    if (!button || !write) return;
    auto** begin=MenuRead<void**>(button,0x190);
    auto** end=MenuRead<void**>(button,0x194);
    if (!begin || end<=begin || end-begin>16 || !*begin) return;
    MenuString text; text.length=static_cast<std::uint32_t>(std::wcslen(caption));
    if (text.length>=8) return;
    std::memcpy(text.text,caption,(text.length+1)*sizeof(wchar_t));
    MenuString empty;
    for (int i=0;i<end-begin;++i) {
        auto* item=begin[i];if(!item) continue;
        write(button,i,i==0?&text:&empty,
            static_cast<int>(MenuRead<float>(item,0x48)),static_cast<int>(MenuRead<float>(item,0x4C)),
            MenuRead<int>(item,0x50),MenuRead<int>(item,0x54),
            MenuRead<std::uint32_t>(item,0x58),MenuRead<std::uint32_t>(item,0x5C),
            MenuRead<std::uint8_t>(item,0x64),MenuRead<std::uint32_t>(item,0x6C),MenuRead<int>(item,0x68));
    }
}
inline void LocalizeCommunityMenu(void* service, MenuText text, bool blocked) {
    const wchar_t* captions[]={L"私聊",L"玩家资料",L"机体资料",L"发送信件",
        MenuRead<std::uint32_t>(service,0x64)==2 && MenuRead<std::uint32_t>(service,0x8C)!=2 ? L"删除好友":L"添加好友",
        blocked ? L"取消屏蔽":L"屏蔽"};
    for (unsigned i=0;i<6;++i)
        SetMenuCaption(MenuRead<void*>(service,0x4C+i*4),captions[i],text);
}
}
