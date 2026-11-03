#include "mods/shop_localization/dictionary.h"

#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    std::ifstream stream(argv[1], std::ios::binary);
    if (!stream) return 2;
    const std::string bytes{
        std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{},
    };
    const std::wstring input(bytes.begin(), bytes.end());
    const auto dictionary = sdgo::shop_localization::Parse(input);

    size_t expanded = 0;
    for (const auto& [source, translation] : dictionary) {
        (void)source;
        if (translation.kind != sdgo::shop_localization::Kind::Description) continue;
        if (translation.text.find(L"\\n") != std::wstring::npos) return 3;
        if (translation.text.find(L'\n') != std::wstring::npos) ++expanded;
    }
    if (!expanded) return 4;
    return 0;
}
