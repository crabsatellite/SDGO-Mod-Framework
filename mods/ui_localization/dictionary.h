#pragma once
#include "phrases.generated.h"
#include <algorithm>
#include <cstddef>
#include <string_view>

namespace sdgo::ui_localization {
inline constexpr std::size_t kMaximumText = 1024;

inline const Phrase* Find(std::wstring_view source) {
    if(source.empty() || source.size()>kMaximumText ||
       source.find(L'\0')!=std::wstring_view::npos)return nullptr;
    const auto* end=kPhrases+kPhraseCount;
    const auto* row=std::lower_bound(kPhrases,end,source,
        [](const Phrase& entry,std::wstring_view value){return std::wstring_view(entry.source)<value;});
    return row!=end && std::wstring_view(row->source)==source ? row:nullptr;
}

inline const SystemPhrase* FindSystem(unsigned identity,std::wstring_view source) {
    const auto* end=kSystemPhrases+kSystemPhraseCount;
    const auto* row=std::lower_bound(kSystemPhrases,end,identity,
        [](const SystemPhrase& entry,unsigned value){return entry.id<value;});
    return row!=end && row->id==identity && std::wstring_view(row->source)==source ? row:nullptr;
}
} // namespace sdgo::ui_localization
