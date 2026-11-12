#pragma once

#include "sdgo/map_catalog_format.h"

namespace sdgo::map_catalog {

inline bool DeclaresSpeech(const Catalog& catalog, uint32_t id) {
    if (!id) return false;
    for (const auto& entry : catalog.records)
        if (entry.kind == kSpeechType && entry.id == id) return true;
    return false;
}

// The import protocol's `cutin.line` always names a speech row in this
// package. Older compilers also copied optional stock-mission references;
// those must never opt an NPC into the mode-5 native illustration path.
inline bool DeclaresUnitCutin(const Catalog& catalog, const Entry& entry) {
    if (entry.kind != kNpcTemplateType || entry.bytes.size() != 598 ||
        !entry.bytes[398] || !DeclaresSpeech(catalog, U32(entry.bytes.data() + 390))) return false;
    const uint32_t endSpeech = U32(entry.bytes.data() + 394);
    return !endSpeech || DeclaresSpeech(catalog, endSpeech);
}

} // namespace sdgo::map_catalog
