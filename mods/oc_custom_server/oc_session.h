// Native OLE DB session reuse for the World server: a new command on the
// request's own game database session, text-typed results, no credentials.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oledb.h>
#include <oledberr.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

namespace sdgo::oc_custom::server {

template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { Reset(); }
    T* Get() const noexcept { return value_; }
    T* operator->() const noexcept { return value_; }
    T** Put() noexcept { Reset(); return &value_; }
    void Reset() noexcept { if (value_) value_->Release(); value_ = nullptr; }
private:
    T* value_ = nullptr;
};

using TextRow = std::vector<std::wstring>;
using TextRows = std::vector<TextRow>;
using ResultSets = std::vector<TextRows>;

inline bool BorrowNativeCommand(void* connection, ICommandText** output) noexcept {
    if (!output) return false;
    *output = nullptr;
    __try {
        if (!connection ||
            *reinterpret_cast<int*>(static_cast<BYTE*>(connection) + 0x240) != 0)
            return false;
        auto* command = *reinterpret_cast<ICommandText**>(static_cast<BYTE*>(connection) + 8);
        if (!command) return false;
        command->AddRef();
        *output = command;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *output = nullptr;
        return false;
    }
}

inline bool ParseInt64(const std::wstring& text, std::int64_t* value) noexcept {
    if (!value || text.empty()) return false;
    wchar_t* end = nullptr;
    const auto parsed = std::wcstoll(text.c_str(), &end, 10);
    if (!end || *end != L'\0') return false;
    *value = parsed;
    return true;
}

inline std::int64_t Int64Or(const TextRow& row, std::size_t index, std::int64_t fallback = 0) noexcept {
    std::int64_t value = fallback;
    if (index < row.size() && !ParseInt64(row[index], &value)) return fallback;
    return value;
}

class Session {
public:
    explicit Session(unsigned timeout_seconds) : timeout_seconds_(timeout_seconds) {}

    bool Open(void* connection) noexcept {
        ComPtr<ICommandText> native;
        if (!BorrowNativeCommand(connection, native.Put())) return false;
        const auto hr = native->GetDBSession(
            __uuidof(IDBCreateCommand), reinterpret_cast<IUnknown**>(factory_.Put()));
        return SUCCEEDED(hr) && factory_.Get() != nullptr;
    }

    // Runs a batch and collects every rowset as text. A batch without any
    // rowset succeeds with an empty result.
    bool Query(const std::wstring& sql, ResultSets* output) noexcept {
        if (!output) return false;
        output->clear();
        ComPtr<ICommandText> command;
        if (!Create(sql, command)) return false;
        ComPtr<IMultipleResults> results;
        DBROWCOUNT affected = 0;
        const auto execute = command->Execute(
            nullptr, __uuidof(IMultipleResults), nullptr, &affected,
            reinterpret_cast<IUnknown**>(results.Put()));
        if (FAILED(execute) || !results.Get()) return false;
        for (;;) {
            ComPtr<IRowset> rows;
            const auto next = results->GetResult(
                nullptr, DBRESULTFLAG_DEFAULT, __uuidof(IRowset), &affected,
                reinterpret_cast<IUnknown**>(rows.Put()));
            if (next == DB_S_NORESULT) return true;
            if (FAILED(next)) return false;
            if (rows.Get()) {
                TextRows decoded;
                if (!ReadRows(rows.Get(), &decoded)) return false;
                output->push_back(std::move(decoded));
            }
        }
    }

    bool Execute(const std::wstring& sql) noexcept {
        ResultSets ignored;
        return Query(sql, &ignored);
    }

private:
    bool Create(const std::wstring& sql, ComPtr<ICommandText>& command) noexcept {
        if (!factory_.Get() || sql.empty()) return false;
        auto hr = factory_->CreateCommand(
            nullptr, __uuidof(ICommandText), reinterpret_cast<IUnknown**>(command.Put()));
        if (FAILED(hr) || !command.Get()) return false;
        ComPtr<ICommandProperties> properties;
        hr = command->QueryInterface(
            __uuidof(ICommandProperties), reinterpret_cast<void**>(properties.Put()));
        if (FAILED(hr) || !properties.Get()) return false;
        DBPROP property{};
        property.dwPropertyID = DBPROP_COMMANDTIMEOUT;
        property.dwOptions = DBPROPOPTIONS_REQUIRED;
        property.colid = DB_NULLID;
        property.vValue.vt = VT_I4;
        property.vValue.lVal = static_cast<LONG>(timeout_seconds_);
        DBPROPSET set{&property, 1, DBPROPSET_ROWSET};
        hr = properties->SetProperties(1, &set);
        if (FAILED(hr) || property.dwStatus != DBPROPSTATUS_OK) return false;
        return SUCCEEDED(command->SetCommandText(DBGUID_DBSQL, sql.c_str()));
    }

    struct TextCell {
        DBSTATUS status = DBSTATUS_E_UNAVAILABLE;
        DBLENGTH length = 0;
        wchar_t value[256]{};
    };

    static bool ReadRows(IRowset* rows, TextRows* output) noexcept {
        ComPtr<IColumnsInfo> columns;
        auto hr = rows->QueryInterface(
            __uuidof(IColumnsInfo), reinterpret_cast<void**>(columns.Put()));
        if (FAILED(hr) || !columns.Get()) return false;
        DBORDINAL column_count = 0;
        DBCOLUMNINFO* info = nullptr;
        OLECHAR* names = nullptr;
        hr = columns->GetColumnInfo(&column_count, &info, &names);
        if (FAILED(hr)) {
            CoTaskMemFree(info);
            CoTaskMemFree(names);
            return false;
        }
        bool plain = column_count >= 1 && column_count <= 64;
        for (DBORDINAL index = 0; plain && index < column_count; ++index)
            if (info[index].iOrdinal != index + 1) plain = false;
        CoTaskMemFree(info);
        CoTaskMemFree(names);
        if (!plain) return false;
        const auto count = static_cast<std::size_t>(column_count);

        ComPtr<IAccessor> accessor;
        hr = rows->QueryInterface(__uuidof(IAccessor), reinterpret_cast<void**>(accessor.Put()));
        if (FAILED(hr) || !accessor.Get()) return false;
        std::vector<DBBINDING> bindings(count);
        std::vector<DBBINDSTATUS> statuses(count, DBBINDSTATUS_BADBINDINFO);
        for (std::size_t index = 0; index < count; ++index) {
            auto& binding = bindings[index];
            binding.iOrdinal = static_cast<DBORDINAL>(index + 1);
            binding.obStatus = static_cast<DBBYTEOFFSET>(index * sizeof(TextCell) + offsetof(TextCell, status));
            binding.obLength = static_cast<DBBYTEOFFSET>(index * sizeof(TextCell) + offsetof(TextCell, length));
            binding.obValue = static_cast<DBBYTEOFFSET>(index * sizeof(TextCell) + offsetof(TextCell, value));
            binding.dwPart = DBPART_STATUS | DBPART_LENGTH | DBPART_VALUE;
            binding.dwMemOwner = DBMEMOWNER_CLIENTOWNED;
            binding.eParamIO = DBPARAMIO_NOTPARAM;
            binding.cbMaxLen = sizeof(TextCell::value);
            binding.wType = DBTYPE_WSTR;
        }
        HACCESSOR handle = DB_NULL_HACCESSOR;
        hr = accessor->CreateAccessor(
            DBACCESSOR_ROWDATA, static_cast<DBCOUNTITEM>(count), bindings.data(),
            static_cast<DBLENGTH>(count * sizeof(TextCell)), &handle, statuses.data());
        if (FAILED(hr) || handle == DB_NULL_HACCESSOR ||
            std::any_of(statuses.begin(), statuses.end(),
                        [](DBBINDSTATUS status) { return status != DBBINDSTATUS_OK; })) {
            if (handle != DB_NULL_HACCESSOR) accessor->ReleaseAccessor(handle, nullptr);
            return false;
        }
        bool success = true;
        std::vector<TextCell> cells(count);
        while (success) {
            DBCOUNTITEM fetched = 0;
            HROW* handles = nullptr;
            hr = rows->GetNextRows(DB_NULL_HCHAPTER, 0, 1, &fetched, &handles);
            if (FAILED(hr)) {
                success = false;
            } else if (fetched == 0) {
                CoTaskMemFree(handles);
                break;
            } else if (!handles || fetched != 1 || output->size() >= 4096) {
                success = false;
            } else {
                std::fill(cells.begin(), cells.end(), TextCell{});
                hr = rows->GetData(handles[0], handle, cells.data());
                if (FAILED(hr)) {
                    success = false;
                } else {
                    TextRow decoded;
                    decoded.reserve(count);
                    for (const auto& cell : cells) {
                        if (cell.status == DBSTATUS_S_ISNULL) {
                            decoded.emplace_back();
                        } else if (cell.status != DBSTATUS_S_OK ||
                                   cell.length > sizeof(cell.value) - sizeof(wchar_t) ||
                                   cell.length % sizeof(wchar_t) != 0) {
                            success = false;
                            break;
                        } else {
                            decoded.emplace_back(cell.value, cell.length / sizeof(wchar_t));
                        }
                    }
                    if (success) output->push_back(std::move(decoded));
                }
            }
            if (fetched && handles) rows->ReleaseRows(fetched, handles, nullptr, nullptr, nullptr);
            CoTaskMemFree(handles);
        }
        accessor->ReleaseAccessor(handle, nullptr);
        return success;
    }

    unsigned timeout_seconds_ = 10;
    ComPtr<IDBCreateCommand> factory_;
};

} // namespace sdgo::oc_custom::server
