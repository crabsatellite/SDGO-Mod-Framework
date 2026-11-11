#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "sdgo/acquisition_sources.h"

namespace sdgo::acquisition_sources_file {

constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint32_t kHeaderBytes = 32;
constexpr std::uint32_t kMaximumRows = 65535;
constexpr std::uint32_t kMaximumPayloadBytes = 32u * 1024u * 1024u;
constexpr std::uint32_t kMaximumKeyBytes = 127;
constexpr std::uint32_t kMaximumTextUnits = 2048;
constexpr std::uint32_t kShopProvider = 1;
constexpr std::uint32_t kOperatorProvider = 2;

enum : std::uint32_t {
    kBlueprintUnclaimed = 0,
    kBlueprintEnabled = 1,
    kBlueprintDisabled = 2,
    kBlueprintIndeterminate = 3,
};

#pragma pack(push, 1)
struct Header {
    char magic[8];
    std::uint32_t version;
    std::uint32_t headerBytes;
    std::uint32_t count;
    std::uint32_t payloadBytes;
    std::uint32_t payloadHash;
    std::uint32_t authority;
};

struct SourceRowHeader {
    std::uint32_t subject;
    std::uint32_t id;
    std::uint32_t keyBytes;
    std::uint32_t textUnits;
};

struct AvailabilityRow {
    std::uint32_t recipe;
    std::uint32_t unit;
    std::uint32_t enabled;
};
#pragma pack(pop)

static_assert(sizeof(Header) == kHeaderBytes);
static_assert(sizeof(SourceRowHeader) == 16);
static_assert(sizeof(AvailabilityRow) == 12);
static_assert(sizeof(wchar_t) == 2, "SDGO client sidecars require Windows UTF-16 wchar_t");

inline std::uint32_t Hash32(const void* bytes, std::size_t size) noexcept {
    const auto* cursor = static_cast<const std::uint8_t*>(bytes);
    std::uint32_t value = 2166136261u;
    for (std::size_t index = 0; index < size; ++index)
        value = (value ^ cursor[index]) * 16777619u;
    return value;
}

namespace detail {

inline void Error(std::string* output, const char* message) {
    if (output) *output = message;
}

enum class ReadResult { Missing, Read, Invalid };

inline ReadResult ReadFile(const std::wstring& path,
                           std::vector<std::uint8_t>& bytes,
                           std::string* error) {
    bytes.clear();
    SetLastError(ERROR_SUCCESS);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            Error(error, "optional acquisition sidecar is absent");
            return ReadResult::Missing;
        }
        Error(error, "optional acquisition sidecar cannot be inspected");
        return ReadResult::Invalid;
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        Error(error, "optional acquisition sidecar is not a regular file");
        return ReadResult::Invalid;
    }

    FILE* stream = nullptr;
    if (_wfopen_s(&stream, path.c_str(), L"rb") || !stream) {
        Error(error, "optional acquisition sidecar is unreadable");
        return ReadResult::Invalid;
    }
    if (std::fseek(stream, 0, SEEK_END)) {
        std::fclose(stream);
        Error(error, "optional acquisition sidecar cannot be sized");
        return ReadResult::Invalid;
    }
    const long length = std::ftell(stream);
    if (length < static_cast<long>(sizeof(Header)) ||
        length > static_cast<long>(sizeof(Header) + kMaximumPayloadBytes)) {
        std::fclose(stream);
        Error(error, "optional acquisition sidecar size is outside the supported bound");
        return ReadResult::Invalid;
    }
    std::rewind(stream);
    bytes.resize(static_cast<std::size_t>(length));
    const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), stream);
    std::fclose(stream);
    if (read != bytes.size()) {
        bytes.clear();
        Error(error, "optional acquisition sidecar read was truncated");
        return ReadResult::Invalid;
    }
    return ReadResult::Read;
}

inline bool KeyCharacter(std::uint8_t value) noexcept {
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == ':' || value == '.' ||
           value == '_' || value == '-';
}

inline bool ValidUtf16(const std::uint8_t* bytes, std::uint32_t units) noexcept {
    for (std::uint32_t index = 0; index < units; ++index) {
        const std::uint16_t value = static_cast<std::uint16_t>(bytes[index * 2]) |
            (static_cast<std::uint16_t>(bytes[index * 2 + 1]) << 8);
        if (!value) return false;
        if (value >= 0xD800 && value <= 0xDBFF) {
            if (++index >= units) return false;
            const std::uint16_t low = static_cast<std::uint16_t>(bytes[index * 2]) |
                (static_cast<std::uint16_t>(bytes[index * 2 + 1]) << 8);
            if (low < 0xDC00 || low > 0xDFFF) return false;
        } else if (value >= 0xDC00 && value <= 0xDFFF) {
            return false;
        }
    }
    return true;
}

inline bool CopyRequest(const SdgoAcquisitionRequestV1* source,
                        SdgoAcquisitionRequestV1* output) noexcept {
#if defined(_MSC_VER)
    __try {
        if (!source) return false;
        std::memcpy(output, source, sizeof(*output));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    if (!source) return false;
    std::memcpy(output, source, sizeof(*output));
    return true;
#endif
}

inline bool CopySink(const SdgoAcquisitionSinkV1* source,
                     SdgoAcquisitionSinkV1* output) noexcept {
#if defined(_MSC_VER)
    __try {
        if (!source) return false;
        std::memcpy(output, source, sizeof(*output));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    if (!source) return false;
    std::memcpy(output, source, sizeof(*output));
    return true;
#endif
}

inline bool Emit(const SdgoAcquisitionSinkV1& sink, const char* key,
                 const wchar_t* text, std::uint32_t units) noexcept {
#if defined(_MSC_VER)
    __try {
        return sink.Emit(sink.context, key, text, units) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return sink.Emit(sink.context, key, text, units) != 0;
#endif
}

inline bool HeaderValid(const std::vector<std::uint8_t>& file,
                        const char (&magic)[9], Header* output,
                        std::string* error) {
    if (file.size() < sizeof(Header)) {
        Error(error, "short acquisition sidecar header");
        return false;
    }
    std::memcpy(output, file.data(), sizeof(*output));
    if (std::memcmp(output->magic, magic, 8) != 0 ||
        output->version != kFormatVersion ||
        output->headerBytes != sizeof(Header) ||
        output->count > kMaximumRows ||
        output->payloadBytes > kMaximumPayloadBytes) {
        Error(error, "unsupported acquisition sidecar header");
        return false;
    }
    if (file.size() != static_cast<std::size_t>(output->headerBytes) +
                           output->payloadBytes ||
        Hash32(file.data() + output->headerBytes, output->payloadBytes) !=
            output->payloadHash) {
        Error(error, "acquisition sidecar length or checksum mismatch");
        return false;
    }
    return true;
}

} // namespace detail

struct SourceRow {
    std::uint32_t subject = 0;
    std::uint32_t id = 0;
    std::string key;
    std::wstring text;
};

class SourceFile {
public:
    void Load(const std::wstring& path, std::uint32_t expectedProvider,
              std::string* error = nullptr) noexcept {
        rows_.clear();
        provider_ = 0;
        state_ = SDGO_ACQUISITION_UNAVAILABLE;
        if (error) error->clear();
        try {
            std::vector<std::uint8_t> bytes;
            const auto result = detail::ReadFile(path, bytes, error);
            if (result == detail::ReadResult::Missing) return;
            if (result != detail::ReadResult::Read) {
                state_ = SDGO_ACQUISITION_INCOMPLETE;
                return;
            }
            Parse(bytes, expectedProvider, error);
        } catch (...) {
            rows_.clear();
            state_ = SDGO_ACQUISITION_INCOMPLETE;
            detail::Error(error, "acquisition source sidecar could not be loaded");
        }
    }

    bool Parse(const std::vector<std::uint8_t>& file,
               std::uint32_t expectedProvider,
               std::string* error = nullptr) noexcept {
        rows_.clear();
        provider_ = 0;
        state_ = SDGO_ACQUISITION_INCOMPLETE;
        if (error) error->clear();
        try {
            Header header{};
            if ((expectedProvider != kShopProvider &&
                 expectedProvider != kOperatorProvider) ||
                !detail::HeaderValid(file, "SDGOAS1\0", &header, error) ||
                header.authority != expectedProvider) {
                if (error && error->empty())
                    detail::Error(error, "acquisition source provider does not match");
                return false;
            }

            std::vector<SourceRow> parsed;
            parsed.reserve(header.count);
            std::size_t cursor = header.headerBytes;
            for (std::uint32_t index = 0; index < header.count; ++index) {
                if (cursor > file.size() ||
                    file.size() - cursor < sizeof(SourceRowHeader)) {
                    detail::Error(error, "short acquisition source row header");
                    return false;
                }
                SourceRowHeader native{};
                std::memcpy(&native, file.data() + cursor, sizeof(native));
                cursor += sizeof(native);
                const bool supportedSubject =
                    native.subject == SDGO_ACQUISITION_BLUEPRINT ||
                    (expectedProvider == kShopProvider &&
                     native.subject == SDGO_ACQUISITION_UNIT);
                if (!supportedSubject || !native.id ||
                    !native.keyBytes || native.keyBytes > kMaximumKeyBytes ||
                    !native.textUnits || native.textUnits > kMaximumTextUnits) {
                    detail::Error(error, "invalid acquisition source row identity or bounds");
                    return false;
                }
                const std::size_t textBytes =
                    static_cast<std::size_t>(native.textUnits) * 2u;
                const std::size_t remaining = file.size() - cursor;
                if (native.keyBytes > remaining ||
                    textBytes > remaining - native.keyBytes) {
                    detail::Error(error, "short acquisition source row payload");
                    return false;
                }
                const auto* key = file.data() + cursor;
                if (!std::all_of(key, key + native.keyBytes,
                                 detail::KeyCharacter)) {
                    detail::Error(error, "invalid acquisition source stable key");
                    return false;
                }
                const auto* text = key + native.keyBytes;
                if (!detail::ValidUtf16(text, native.textUnits)) {
                    detail::Error(error, "invalid acquisition source UTF-16 text");
                    return false;
                }

                SourceRow row;
                row.subject = native.subject;
                row.id = native.id;
                row.key.assign(reinterpret_cast<const char*>(key), native.keyBytes);
                row.text.resize(native.textUnits);
                std::memcpy(row.text.data(), text, textBytes);
                cursor += native.keyBytes + textBytes;
                if (!parsed.empty()) {
                    const SourceRow& previous = parsed.back();
                    if (row.subject < previous.subject ||
                        (row.subject == previous.subject && row.id < previous.id) ||
                        (row.subject == previous.subject && row.id == previous.id &&
                         row.key <= previous.key)) {
                        detail::Error(error, "duplicate or unsorted acquisition source row");
                        return false;
                    }
                }
                parsed.push_back(std::move(row));
            }
            if (cursor != file.size()) {
                detail::Error(error, "acquisition source sidecar has trailing bytes");
                return false;
            }
            rows_.swap(parsed);
            provider_ = expectedProvider;
            state_ = SDGO_ACQUISITION_READY;
            return true;
        } catch (...) {
            rows_.clear();
            state_ = SDGO_ACQUISITION_INCOMPLETE;
            detail::Error(error, "acquisition source sidecar could not be parsed");
            return false;
        }
    }

    std::uint32_t Query(const SdgoAcquisitionRequestV1* request,
                        const SdgoAcquisitionSinkV1* sink) const noexcept {
        if (state_ != SDGO_ACQUISITION_READY) return state_;
        SdgoAcquisitionRequestV1 safeRequest{};
        SdgoAcquisitionSinkV1 safeSink{};
        if (!detail::CopyRequest(request, &safeRequest) ||
            !detail::CopySink(sink, &safeSink) ||
            safeRequest.size != sizeof(safeRequest) || safeRequest.version != 1 ||
            (safeRequest.subject != SDGO_ACQUISITION_BLUEPRINT &&
             !(provider_ == kShopProvider &&
               safeRequest.subject == SDGO_ACQUISITION_UNIT)) ||
            !safeRequest.id || safeSink.size != sizeof(safeSink) ||
            safeSink.version != 1 ||
            !safeSink.Emit) {
            return SDGO_ACQUISITION_INCOMPLETE;
        }
        try {
            const std::pair<std::uint32_t, std::uint32_t> target{
                safeRequest.subject, safeRequest.id};
            const auto first = std::lower_bound(
                rows_.begin(), rows_.end(), target,
                [](const SourceRow& row,
                   const std::pair<std::uint32_t, std::uint32_t>& value) {
                    return row.subject < value.first ||
                        (row.subject == value.first && row.id < value.second);
                });
            for (auto current = first;
                 current != rows_.end() &&
                 current->subject == safeRequest.subject &&
                 current->id == safeRequest.id; ++current) {
                if (!detail::Emit(safeSink, current->key.c_str(),
                                  current->text.c_str(),
                                  static_cast<std::uint32_t>(current->text.size())))
                    return SDGO_ACQUISITION_INCOMPLETE;
            }
            return SDGO_ACQUISITION_READY;
        } catch (...) {
            return SDGO_ACQUISITION_INCOMPLETE;
        }
    }

    std::uint32_t status() const noexcept { return state_; }
    const std::vector<SourceRow>& rows() const noexcept { return rows_; }

private:
    std::vector<SourceRow> rows_;
    std::uint32_t provider_ = 0;
    std::uint32_t state_ = SDGO_ACQUISITION_UNAVAILABLE;
};

class AvailabilityFile {
public:
    void Load(const std::wstring& path, std::uint32_t catalogPayloadHash,
              std::string* error = nullptr) noexcept {
        rows_.clear();
        ready_ = false;
        if (error) error->clear();
        try {
            std::vector<std::uint8_t> bytes;
            if (detail::ReadFile(path, bytes, error) != detail::ReadResult::Read)
                return;
            Parse(bytes, catalogPayloadHash, error);
        } catch (...) {
            rows_.clear();
            ready_ = false;
            detail::Error(error, "blueprint availability sidecar could not be loaded");
        }
    }

    bool Parse(const std::vector<std::uint8_t>& file,
               std::uint32_t catalogPayloadHash,
               std::string* error = nullptr) noexcept {
        rows_.clear();
        ready_ = false;
        if (error) error->clear();
        try {
            Header header{};
            if (!detail::HeaderValid(file, "SDGOBP1\0", &header, error) ||
                header.authority != catalogPayloadHash ||
                header.payloadBytes !=
                    static_cast<std::uint64_t>(header.count) * sizeof(AvailabilityRow)) {
                if (error && error->empty())
                    detail::Error(error, "blueprint availability authority does not match catalog");
                return false;
            }
            std::vector<AvailabilityRow> parsed;
            parsed.reserve(header.count);
            std::size_t cursor = header.headerBytes;
            std::uint32_t previous = 0;
            for (std::uint32_t index = 0; index < header.count; ++index) {
                AvailabilityRow row{};
                std::memcpy(&row, file.data() + cursor, sizeof(row));
                cursor += sizeof(row);
                if (!row.recipe || !row.unit || row.recipe <= previous ||
                    row.enabled > 1) {
                    detail::Error(error, "invalid, duplicate or unsorted blueprint availability row");
                    return false;
                }
                previous = row.recipe;
                parsed.push_back(row);
            }
            if (cursor != file.size()) {
                detail::Error(error, "blueprint availability sidecar has trailing bytes");
                return false;
            }
            rows_.swap(parsed);
            ready_ = true;
            return true;
        } catch (...) {
            rows_.clear();
            ready_ = false;
            detail::Error(error, "blueprint availability sidecar could not be parsed");
            return false;
        }
    }

    std::uint32_t State(std::uint32_t recipe, std::uint32_t unit) const noexcept {
        if (!ready_ || !recipe || !unit) return kBlueprintIndeterminate;
        const auto found = std::lower_bound(
            rows_.begin(), rows_.end(), recipe,
            [](const AvailabilityRow& row, std::uint32_t id) {
                return row.recipe < id;
            });
        if (found == rows_.end() || found->recipe != recipe ||
            found->unit != unit)
            return kBlueprintIndeterminate;
        return found->enabled ? kBlueprintEnabled : kBlueprintDisabled;
    }

    bool ready() const noexcept { return ready_; }
    const std::vector<AvailabilityRow>& rows() const noexcept { return rows_; }

private:
    std::vector<AvailabilityRow> rows_;
    bool ready_ = false;
};

} // namespace sdgo::acquisition_sources_file