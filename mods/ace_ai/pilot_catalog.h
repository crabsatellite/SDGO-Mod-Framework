#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "../announcements/news_json.h"
#include "sdgo/sdgo_api.h"

namespace battle_ai::ace {
struct PilotEntry {
    uint32_t unitId = 0;
    std::wstring displayName;
    bool role = false;
};
class PilotCatalog {
    std::map<uint32_t, PilotEntry> entries_;
    enum class ReadResult { Ok, Missing, Error };
    static ReadResult ReadFile(const std::wstring& path, std::string& out) {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") || !file) {
            WIN32_FIND_DATAW data{};
            const HANDLE found = FindFirstFileW(path.c_str(), &data);
            if (found != INVALID_HANDLE_VALUE) {
                FindClose(found);
                return ReadResult::Error;
            }
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                ? ReadResult::Missing : ReadResult::Error;
        }
        if (fseek(file, 0, SEEK_END)) { fclose(file); return ReadResult::Error; }
        const long size = ftell(file);
        if (size < 0 || size > 2 * 1024 * 1024) {
            fclose(file);
            return ReadResult::Error;
        }
        rewind(file);
        out.resize(static_cast<size_t>(size));
        const size_t read = size ? fread(out.data(), 1, out.size(), file) : 0;
        fclose(file);
        return read == out.size() ? ReadResult::Ok : ReadResult::Error;
    }
    static std::wstring Wide(const std::string& value) {
        if (value.empty()) return {};
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                               value.data(), static_cast<int>(value.size()),
                                               nullptr, 0);
        if (count <= 0 || count > 64) return {};
        std::wstring out(static_cast<size_t>(count), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                static_cast<int>(value.size()), out.data(), count) != count)
            return {};
        for (wchar_t c : out)
            if (c < 0x20 || c == L'<' || c == L'>') return {};
        return out;
    }
    bool Merge(const std::wstring& path, bool required, std::string& error) {
        using namespace sdgo::announcements::json;
        std::string bytes;
        const ReadResult read = ReadFile(path, bytes);
        if (read == ReadResult::Missing && !required) return true;
        if (read != ReadResult::Ok) {
            error = required ? "base-pilots.json is missing, unreadable, or too large"
                             : "owner-pilots.json is unreadable or too large";
            return false;
        }
        try {
            const Value root = Parser(bytes).Parse();
            if (root.kind != Value::Object || root["schema_version"].Integer(-1) != 1 ||
                root["pilots"].kind != Value::Array) throw std::runtime_error("schema");
            std::map<uint32_t, PilotEntry> incoming;
            for (const Value& value : root["pilots"].array) {
                if (value.kind != Value::Object || value["unit_id"].kind != Value::Number ||
                    value["display_name"].kind != Value::String || value["kind"].kind != Value::String)
                    throw std::runtime_error("entry");
                const int64_t raw = value["unit_id"].Integer(-1);
                const std::wstring name = Wide(value["display_name"].string);
                const bool named = value["kind"].string == "named";
                const bool role = value["kind"].string == "role";
                if (raw <= 0 || raw > 9999999 || name.empty() || (!named && !role) ||
                    !incoming.emplace(static_cast<uint32_t>(raw), PilotEntry{static_cast<uint32_t>(raw), name, role}).second)
                    throw std::runtime_error("identity");
            }
            for (auto& item : incoming) entries_[item.first] = std::move(item.second);
            return true;
        } catch (...) {
            error = "pilot catalog JSON is invalid";
            return false;
        }
    }
public:
    bool Load(const SdgoApi* api, SdgoMod* mod, std::string& error,
              const wchar_t* catalogDirectory = L"ace_ai") {
        entries_.clear();
        if (!api || !api->ModDirectory || !catalogDirectory || !*catalogDirectory) {
            error = "mod directory unavailable";
            return false;
        }
        std::wstring directory = api->ModDirectory(mod);
        const size_t slash = directory.find_last_of(L"\\/");
        if (slash == std::wstring::npos) { error = "mod directory has no parent"; return false; }
        directory.resize(slash + 1);
        directory += catalogDirectory;
        if (!Merge(directory + L"\\base-pilots.json", true, error)) return false;
        std::string overlayError;
        if (!Merge(directory + L"\\owner-pilots.json", false, overlayError)) {
            error = overlayError;
            return false;
        }
        return true;
    }
    const PilotEntry* Find(uint32_t unitId) const {
        auto found = entries_.find(unitId);
        return found == entries_.end() ? nullptr : &found->second;
    }
    size_t Size() const { return entries_.size(); }
};
} // namespace battle_ai::ace
