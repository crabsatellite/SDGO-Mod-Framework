#pragma once
#include "sdgo/acquisition_sources.h"
#include <windows.h>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace sdgo::acquisition_sources {
inline constexpr std::uint32_t kMaxText = 2048;
inline bool AllowBlueprint(std::uint32_t state, bool nativePrimary) noexcept {
    return state == 1 || (state == 0 && nativePrimary);
}
struct Batch {
    std::map<std::string, std::wstring> rows;
    std::size_t chars = 0;
    bool valid = true;
};

inline bool Copy(const char* key, const wchar_t* text, std::uint32_t count,
                 char* keyCopy, wchar_t* textCopy) noexcept {
    if (!key || !text || !count || count > kMaxText) return false;
    __try {
        std::size_t size = 0;
        for (; size < 127 && key[size]; ++size) {
            const auto c = static_cast<unsigned char>(key[size]);
            if (c < 0x21 || c > 0x7E) return false;
            keyCopy[size] = key[size];
        }
        if (!size || (size == 127 && key[size])) return false;
        keyCopy[size] = 0;
        std::memcpy(textCopy, text, count * 2);
        textCopy[count] = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto c = static_cast<std::uint16_t>(textCopy[i]);
            if (!c || (c < 0x20 && c != '\n' && c != '\t')) return false;
            if (c >= 0xD800 && c <= 0xDBFF) {
                if (++i >= count || textCopy[i] < 0xDC00 || textCopy[i] > 0xDFFF) return false;
            } else if (c >= 0xDC00 && c <= 0xDFFF) return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline int __cdecl Emit(void* context, const char* key, const wchar_t* text, std::uint32_t count) {
    auto* batch = static_cast<Batch*>(context);
    if (!batch || !batch->valid) return 0;
    char name[128]{};
    wchar_t value[kMaxText + 1]{};
    if (!Copy(key, text, count, name, value)) { batch->valid = false; return 0; }
    try {
        const auto found = batch->rows.find(name);
        if (found != batch->rows.end()) {
            if (found->second != value) batch->valid = false;
            return batch->valid ? 1 : 0;
        }
        if (batch->rows.size() >= 128 || batch->chars + count > 16384) { batch->valid = false; return 0; }
        batch->rows.emplace(name, value);
        batch->chars += count;
        return 1;
    } catch (...) { batch->valid = false; return 0; }
}

inline std::uint32_t Invoke(SdgoAcquisitionSourcesV1Fn provider,
                           const SdgoAcquisitionRequestV1* request,
                           const SdgoAcquisitionSinkV1* sink) noexcept {
    if (!provider) return SDGO_ACQUISITION_UNAVAILABLE;
    __try { return provider(request, sink); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return SDGO_ACQUISITION_INCOMPLETE; }
}

inline std::vector<std::wstring> Query(SdgoAcquisitionSourcesV1Fn provider,
                                     std::uint32_t subject, std::uint32_t id) {
    if (!id || (subject != SDGO_ACQUISITION_UNIT && subject != SDGO_ACQUISITION_BLUEPRINT)) return {};
    Batch batch;
    const SdgoAcquisitionRequestV1 request{sizeof(request), 1, subject, id};
    const SdgoAcquisitionSinkV1 sink{sizeof(sink), 1, &batch, &Emit};
    const auto status = Invoke(provider, &request, &sink);
    std::vector<std::wstring> result;
    if (status != SDGO_ACQUISITION_READY || !batch.valid) return result;
    for (const auto& row : batch.rows) result.push_back(row.second);
    return result;
}
} // namespace sdgo::acquisition_sources
