#include "../mods/ui_localization/dictionary.h"
#include <cassert>
#include <string>

int main() {
    using namespace sdgo::ui_localization;
    const auto* cancel=Find(L"ยกเลิก");
    assert(cancel && std::wstring_view(cancel->target)==L"取消");
    assert(!Find(L"Search") && !Find(L"All") && !Find(L"GAME START") && !Find(L"EXIT"));
    assert(!Find(L"Search ") && !Find(L" Search"));
    assert(!Find(L"User-authored room title") && !Find(L"player_104"));
    assert(!Find(L"搜索") && !Find(std::wstring(kMaximumText+1,L'x')));
    const wchar_t nul[]={L'A',L'l',L'l',0,L'x'};
    assert(!Find(std::wstring_view(nul,5)));
    const auto* version=FindSystem(126956,L"Does not match the version. To end the game");
    assert(version && std::wstring_view(version->target)==L"版本不匹配，游戏即将结束。");
    assert(!FindSystem(126734,L"GAME START"));
    assert(!FindSystem(126955,L"Does not match the version. To end the game"));
    assert(!FindSystem(126956,L"An owner's different message"));
    assert(!FindSystem(126956,L"版本不匹配，游戏即将结束。"));
    return 0;
}
