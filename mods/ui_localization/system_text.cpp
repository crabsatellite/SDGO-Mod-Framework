#include "system_text.h"

#include <windows.h>

#include <cstring>
#include <map>

#include "dictionary.h"
#include "sdgo/content_localization_boundary.h"

namespace sdgo::ui_localization::system_text {
namespace {

static_assert(sizeof(wchar_t) == 2);
static_assert(kStyleOffset + sizeof(std::uint32_t) == kRecordBytes);

using GridFind = bool(__thiscall*)(void*, const std::uint8_t**, std::uint32_t,
                                  int, char);

GridFind g_original = nullptr;
SRWLOCK g_cacheLock = SRWLOCK_INIT;
std::map<Record, Record> g_cache;
alignas(8) volatile LONG64 g_lookups = 0;
alignas(8) volatile LONG64 g_replaced = 0;
alignas(8) volatile LONG64 g_invalid = 0;

std::uint32_t ReadId(const std::uint8_t* source) noexcept
{
    std::uint32_t value = 0;
    std::memcpy(&value, source, sizeof(value));
    return value;
}

bool ReadText(const std::uint8_t* source, wchar_t* buffer,
              std::wstring_view* text) noexcept
{
    for (std::size_t index = 0; index < kTextCharacters; ++index) {
        std::uint16_t value = 0;
        std::memcpy(&value, source + kTextOffset + index * 2, sizeof(value));
        buffer[index] = static_cast<wchar_t>(value);
        if (!value) {
            *text = std::wstring_view(buffer, index);
            return true;
        }
    }
    return false;
}

bool CopyRecord(const std::uint8_t* source, Record* output) noexcept
{
    if (!source || !output)
        return false;
    __try {
        std::memcpy(output->data(), source, output->size());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const std::uint8_t* Cached(const Record& source, const Record& patched) noexcept
{
    const std::uint8_t* result = nullptr;
    AcquireSRWLockExclusive(&g_cacheLock);
    try {
        const auto inserted = g_cache.emplace(source, patched);
        result = inserted.first->second.data();
    } catch (...) {
        result = nullptr;
    }
    ReleaseSRWLockExclusive(&g_cacheLock);
    return result;
}

bool Publish(const std::uint8_t** output, const std::uint8_t* replacement) noexcept
{
    if (!output || !replacement)
        return false;
    __try {
        *output = replacement;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool __fastcall Find(void* data, void*, const std::uint8_t** output,
                     std::uint32_t id, int type, char subtype)
{
    const bool result = g_original(data, output, id, type, subtype);
    const DWORD originalError = GetLastError();
    InterlockedIncrement64(&g_lookups);

    if (result && type == kTableType) {
        Record source{};
        Record patched{};
        const std::uint8_t* row = nullptr;
        __try {
            row = output ? *output : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            row = nullptr;
        }
        if (!CopyRecord(row, &source)) {
            InterlockedIncrement64(&g_invalid);
        } else {
            const PatchResult patch =
                TranslateRecord(source.data(), source.size(), id, type, &patched);
            if (patch == PatchResult::replaced) {
                const std::uint8_t* replacement = Cached(source, patched);
                if (Publish(output, replacement))
                    InterlockedIncrement64(&g_replaced);
                else
                    InterlockedIncrement64(&g_invalid);
            } else if (patch == PatchResult::invalid) {
                InterlockedIncrement64(&g_invalid);
            }
        }
    }

    SetLastError(originalError);
    return result;
}

} // namespace

PatchResult PatchRecord(const std::uint8_t* source, std::size_t sourceBytes,
                        std::uint32_t requestedId, int type,
                        std::wstring_view target, Record* output) noexcept
{
    if (type != kTableType)
        return PatchResult::unchanged;
    if (!source || !output || sourceBytes < kRecordBytes ||
        ReadId(source) != requestedId || target.size() >= kTextCharacters ||
        target.find(L'\0') != std::wstring_view::npos)
        return PatchResult::invalid;

    wchar_t sourceBuffer[kTextCharacters] = {};
    std::wstring_view sourceText;
    if (!ReadText(source, sourceBuffer, &sourceText))
        return PatchResult::invalid;

    Record patched{};
    std::memcpy(patched.data(), source, patched.size());
    std::memset(patched.data() + kTextOffset, 0, kTextCharacters * 2);
    for (std::size_t index = 0; index < target.size(); ++index) {
        const std::uint16_t value = static_cast<std::uint16_t>(target[index]);
        std::memcpy(patched.data() + kTextOffset + index * 2, &value,
                    sizeof(value));
    }
    *output = patched;
    return PatchResult::replaced;
}

PatchResult TranslateRecord(const std::uint8_t* source,
                            std::size_t sourceBytes,
                            std::uint32_t requestedId, int type,
                            Record* output) noexcept
{
    if (type != kTableType)
        return PatchResult::unchanged;
    if (!source || !output || sourceBytes < kRecordBytes ||
        ReadId(source) != requestedId)
        return PatchResult::invalid;

    wchar_t sourceBuffer[kTextCharacters] = {};
    std::wstring_view sourceText;
    if (!ReadText(source, sourceBuffer, &sourceText))
        return PatchResult::invalid;
    const SystemPhrase* phrase = FindSystem(requestedId, sourceText);
    if (!phrase)
        return PatchResult::unchanged;
    if(sdgo::content_localization_boundary::Query(0,0,sourceText.data(),
          static_cast<std::uint32_t>(sourceText.size()))!=SDGO_CONTENT_LOCALIZATION_NOT_OWNED)
        return PatchResult::unchanged;
    return PatchRecord(source, sourceBytes, requestedId, type, phrase->target,
                       output);
}

bool Install(const SdgoApi* api, SdgoMod* mod)
{
    if (g_original)
        return true;
    if (!api || api->size < offsetof(SdgoApi, HookCode) + sizeof(api->HookCode) ||
        !api->HookCode)
        return false;
    GridFind original = nullptr;
    if (!api->HookCode(mod, "ui_localization.grid_find",
                       reinterpret_cast<void*>(&Find),
                       reinterpret_cast<void**>(&original)) ||
        !original)
        return false;
    g_original = original;
    return true;
}

Stats Snapshot()
{
    return {
        static_cast<std::uint64_t>(
            InterlockedCompareExchange64(&g_lookups, 0, 0)),
        static_cast<std::uint64_t>(
            InterlockedCompareExchange64(&g_replaced, 0, 0)),
        static_cast<std::uint64_t>(
            InterlockedCompareExchange64(&g_invalid, 0, 0)),
    };
}

} // namespace sdgo::ui_localization::system_text
