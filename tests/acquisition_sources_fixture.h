#pragma once

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace acquisition_fixture {

struct Row {
    std::uint32_t subject;
    std::uint32_t id;
    const char* key;
    const wchar_t* text;
};

struct Header {
    char magic[8];
    std::uint32_t version;
    std::uint32_t headerBytes;
    std::uint32_t count;
    std::uint32_t payloadBytes;
    std::uint32_t payloadHash;
    std::uint32_t provider;
};
static_assert(sizeof(Header) == 32);
static_assert(sizeof(wchar_t) == 2);

inline std::uint32_t Hash32(const void* bytes, std::size_t size) noexcept {
    const auto* data = static_cast<const std::uint8_t*>(bytes);
    std::uint32_t value = 2166136261u;
    for (std::size_t index = 0; index < size; ++index)
        value = (value ^ data[index]) * 16777619u;
    return value;
}

inline void AppendU32(std::vector<std::uint8_t>& output,
                      std::uint32_t value) {
    const std::size_t offset = output.size();
    output.resize(offset + sizeof(value));
    std::memcpy(output.data() + offset, &value, sizeof(value));
}

inline bool Write(const std::filesystem::path& path, std::uint32_t provider,
                  const std::vector<Row>& rows) {
    std::vector<std::uint8_t> payload;
    for (const Row& row : rows) {
        const std::size_t keyBytes = std::strlen(row.key);
        const std::size_t textUnits = std::wcslen(row.text);
        AppendU32(payload, row.subject);
        AppendU32(payload, row.id);
        AppendU32(payload, static_cast<std::uint32_t>(keyBytes));
        AppendU32(payload, static_cast<std::uint32_t>(textUnits));
        payload.insert(payload.end(), row.key, row.key + keyBytes);
        const auto* text = reinterpret_cast<const std::uint8_t*>(row.text);
        payload.insert(payload.end(), text, text + textUnits * sizeof(wchar_t));
    }
    Header header{};
    std::memcpy(header.magic, "SDGOAS1", 8);
    header.version = 1;
    header.headerBytes = sizeof(header);
    header.count = static_cast<std::uint32_t>(rows.size());
    header.payloadBytes = static_cast<std::uint32_t>(payload.size());
    header.payloadHash = Hash32(payload.data(), payload.size());
    header.provider = provider;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(payload.data()),
                 static_cast<std::streamsize>(payload.size()));
    return static_cast<bool>(output);
}

struct Capture {
    std::vector<std::pair<std::string, std::wstring>> rows;
};

inline int __cdecl Emit(void* context, const char* key, const wchar_t* text,
                        std::uint32_t chars) {
    auto* capture = static_cast<Capture*>(context);
    if (!capture || !key || !text) return 0;
    capture->rows.emplace_back(key, std::wstring(text, text + chars));
    return 1;
}

} // namespace acquisition_fixture
