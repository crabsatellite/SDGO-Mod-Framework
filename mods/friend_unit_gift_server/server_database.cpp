#ifndef NOMINMAX
#define NOMINMAX
#endif
#define DBINITCONSTANTS
#include <windows.h>
#include <oledb.h>
#include <oledberr.h>
#include <transact.h>

#include "server_database.h"

#include "sql_policy.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cwchar>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace sdgo::friend_unit_gift {
namespace {

template<class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { Reset(); }

    T* Get() const noexcept { return value_; }
    T* operator->() const noexcept { return value_; }
    T** Put() noexcept {
        Reset();
        return &value_;
    }
    void Reset() noexcept {
        if (value_) value_->Release();
        value_ = nullptr;
    }

private:
    T* value_ = nullptr;
};

struct TextCell {
    DBSTATUS status = DBSTATUS_E_UNAVAILABLE;
    DBLENGTH length = 0;
    wchar_t value[512]{};
};

using TextRow = std::vector<std::wstring>;
using TextRows = std::vector<TextRow>;

bool BorrowNativeCommand(void* connection, ICommandText** output) noexcept {
    if (!output) return false;
    *output = nullptr;
    __try {
        if (!connection ||
            *reinterpret_cast<int*>(
                static_cast<BYTE*>(connection) + 0x240) != 0)
            return false;
        auto* command =
            *reinterpret_cast<ICommandText**>(
                static_cast<BYTE*>(connection) + 8);
        if (!command) return false;
        command->AddRef();
        *output = command;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *output = nullptr;
        return false;
    }
}

class NativeSession {
public:
    explicit NativeSession(unsigned timeoutSeconds)
        : timeout_seconds_(timeoutSeconds) {}

    bool Open(void* connection) noexcept {
        ComPtr<ICommandText> native;
        if (!BorrowNativeCommand(connection, native.Put()))
            return false;
        const auto hr = native->GetDBSession(
            __uuidof(IDBCreateCommand),
            reinterpret_cast<IUnknown**>(factory_.Put()));
        if (FAILED(hr) || !factory_.Get())
            return false;
        ComPtr<ITransactionLocal> transaction;
        const auto transactionResult = factory_->QueryInterface(
            __uuidof(ITransactionLocal),
            reinterpret_cast<void**>(transaction.Put()));
        if (FAILED(transactionResult) || !transaction.Get())
            return false;
        XACTTRANSINFO active{};
        if (SUCCEEDED(transaction->GetTransactionInfo(&active)))
            return false;
        return true;
    }

    bool Clean() noexcept {
        TextRows rows;
        if (!Query(
                L"SELECT CONVERT(bigint,@@TRANCOUNT),"
                L"CONVERT(bigint,@@OPTIONS)",
                2, &rows, 1) ||
            rows.size() != 1)
            return false;
        std::uint64_t transactionCount = 0;
        std::uint64_t options = 0;
        return ParseUnsigned(rows[0][0], &transactionCount) &&
               ParseUnsigned(rows[0][1], &options) &&
               transactionCount == 0 && (options & 2u) == 0;
    }

    bool Scalar(
        const std::wstring& sql,
        std::wstring* output) noexcept {
        if (!output) return false;
        TextRows rows;
        if (!Query(sql, 1, &rows, 1) || rows.size() != 1)
            return false;
        *output = rows[0][0];
        return true;
    }

    bool Query(
        const std::wstring& sql,
        std::size_t expectedColumns,
        TextRows* output,
        std::size_t maximumRows =
            static_cast<std::size_t>(100000),
        bool* issued = nullptr) noexcept {
        if (issued) *issued = false;
        if (!output || expectedColumns == 0 || expectedColumns > 32)
            return false;
        output->clear();
        ComPtr<ICommandText> command;
        if (!Create(sql, command))
            return false;
        ComPtr<IMultipleResults> results;
        DBROWCOUNT affected = 0;
        if (issued) *issued = true;
        const auto executeResult = command->Execute(
            nullptr, __uuidof(IMultipleResults), nullptr, &affected,
            reinterpret_cast<IUnknown**>(results.Put()));
        if (FAILED(executeResult) || !results.Get())
            return false;
        for (;;) {
            ComPtr<IRowset> rows;
            const auto next = results->GetResult(
                nullptr, DBRESULTFLAG_DEFAULT, __uuidof(IRowset),
                &affected, reinterpret_cast<IUnknown**>(rows.Put()));
            if (next == DB_S_NORESULT)
                return false;
            if (FAILED(next))
                return false;
            if (rows.Get())
                return ReadRows(
                    rows.Get(), expectedColumns, output, maximumRows);
        }
    }

    bool Execute(const std::wstring& sql) noexcept {
        ComPtr<ICommandText> command;
        if (!Create(sql, command))
            return false;
        ComPtr<IMultipleResults> results;
        DBROWCOUNT affected = 0;
        const auto executeResult = command->Execute(
            nullptr, __uuidof(IMultipleResults), nullptr, &affected,
            reinterpret_cast<IUnknown**>(results.Put()));
        if (FAILED(executeResult) || !results.Get())
            return false;
        for (;;) {
            ComPtr<IRowset> rows;
            const auto next = results->GetResult(
                nullptr, DBRESULTFLAG_DEFAULT, __uuidof(IRowset),
                &affected, reinterpret_cast<IUnknown**>(rows.Put()));
            if (next == DB_S_NORESULT)
                return true;
            if (FAILED(next))
                return false;
        }
    }

private:
    static bool ParseUnsigned(
        const std::wstring& value,
        std::uint64_t* output) noexcept {
        if (!output || value.empty() || value.front() == L'-')
            return false;
        errno = 0;
        wchar_t* end = nullptr;
        const auto converted = std::wcstoull(value.c_str(), &end, 10);
        if (errno == ERANGE || !end || *end != L'\0')
            return false;
        *output = converted;
        return true;
    }

    bool Create(
        const std::wstring& sql,
        ComPtr<ICommandText>& command) noexcept {
        if (!factory_.Get() || sql.empty())
            return false;
        auto hr = factory_->CreateCommand(
            nullptr, __uuidof(ICommandText),
            reinterpret_cast<IUnknown**>(command.Put()));
        if (FAILED(hr) || !command.Get())
            return false;
        ComPtr<ICommandProperties> properties;
        hr = command->QueryInterface(
            __uuidof(ICommandProperties),
            reinterpret_cast<void**>(properties.Put()));
        if (FAILED(hr) || !properties.Get())
            return false;
        DBPROP property{};
        property.dwPropertyID = DBPROP_COMMANDTIMEOUT;
        property.dwOptions = DBPROPOPTIONS_REQUIRED;
        property.colid = DB_NULLID;
        property.vValue.vt = VT_I4;
        property.vValue.lVal =
            static_cast<LONG>(timeout_seconds_);
        DBPROPSET set{&property, 1, DBPROPSET_ROWSET};
        hr = properties->SetProperties(1, &set);
        if (FAILED(hr) || property.dwStatus != DBPROPSTATUS_OK)
            return false;
        return SUCCEEDED(
            command->SetCommandText(DBGUID_DBSQL, sql.c_str()));
    }

    static bool ReadRows(
        IRowset* rows,
        std::size_t expectedColumns,
        TextRows* output,
        std::size_t maximumRows) noexcept {
        if (!rows || !output)
            return false;
        ComPtr<IColumnsInfo> columns;
        auto hr = rows->QueryInterface(
            __uuidof(IColumnsInfo),
            reinterpret_cast<void**>(columns.Put()));
        if (FAILED(hr) || !columns.Get())
            return false;
        DBORDINAL columnCount = 0;
        DBCOLUMNINFO* columnInfo = nullptr;
        OLECHAR* names = nullptr;
        hr = columns->GetColumnInfo(
            &columnCount, &columnInfo, &names);
        if (FAILED(hr)) {
            CoTaskMemFree(columnInfo);
            CoTaskMemFree(names);
            return false;
        }
        bool exact = columnCount == expectedColumns;
        if (exact) {
            for (DBORDINAL index = 0; index < columnCount; ++index) {
                if (columnInfo[index].iOrdinal != index + 1) {
                    exact = false;
                    break;
                }
            }
        }
        CoTaskMemFree(columnInfo);
        CoTaskMemFree(names);
        if (!exact)
            return false;

        ComPtr<IAccessor> accessor;
        hr = rows->QueryInterface(
            __uuidof(IAccessor),
            reinterpret_cast<void**>(accessor.Put()));
        if (FAILED(hr) || !accessor.Get())
            return false;
        std::vector<DBBINDING> bindings(expectedColumns);
        std::vector<DBBINDSTATUS> statuses(
            expectedColumns, DBBINDSTATUS_BADBINDINFO);
        for (std::size_t index = 0; index < expectedColumns; ++index) {
            auto& binding = bindings[index];
            binding.iOrdinal = static_cast<DBORDINAL>(index + 1);
            binding.obStatus =
                static_cast<DBBYTEOFFSET>(
                    index * sizeof(TextCell) +
                    offsetof(TextCell, status));
            binding.obLength =
                static_cast<DBBYTEOFFSET>(
                    index * sizeof(TextCell) +
                    offsetof(TextCell, length));
            binding.obValue =
                static_cast<DBBYTEOFFSET>(
                    index * sizeof(TextCell) +
                    offsetof(TextCell, value));
            binding.dwPart =
                DBPART_STATUS | DBPART_LENGTH | DBPART_VALUE;
            binding.dwMemOwner = DBMEMOWNER_CLIENTOWNED;
            binding.eParamIO = DBPARAMIO_NOTPARAM;
            binding.cbMaxLen = sizeof(TextCell::value);
            binding.wType = DBTYPE_WSTR;
        }
        HACCESSOR handle = DB_NULL_HACCESSOR;
        hr = accessor->CreateAccessor(
            DBACCESSOR_ROWDATA,
            static_cast<DBCOUNTITEM>(bindings.size()),
            bindings.data(),
            static_cast<DBLENGTH>(
                expectedColumns * sizeof(TextCell)),
            &handle, statuses.data());
        if (FAILED(hr) || handle == DB_NULL_HACCESSOR ||
            std::any_of(
                statuses.begin(), statuses.end(),
                [](DBBINDSTATUS status) {
                    return status != DBBINDSTATUS_OK;
                })) {
            if (handle != DB_NULL_HACCESSOR)
                accessor->ReleaseAccessor(handle, nullptr);
            return false;
        }

        bool success = true;
        std::vector<TextCell> cells(expectedColumns);
        while (success) {
            DBCOUNTITEM count = 0;
            HROW* rowHandles = nullptr;
            hr = rows->GetNextRows(
                DB_NULL_HCHAPTER, 0, 1, &count, &rowHandles);
            if (FAILED(hr)) {
                success = false;
            } else if (count == 0) {
                CoTaskMemFree(rowHandles);
                break;
            } else if (!rowHandles || count != 1 ||
                       output->size() >= maximumRows) {
                success = false;
            } else {
                std::fill(cells.begin(), cells.end(), TextCell{});
                hr = rows->GetData(rowHandles[0], handle, cells.data());
                if (FAILED(hr)) {
                    success = false;
                } else {
                    TextRow decoded;
                    decoded.reserve(expectedColumns);
                    for (const auto& cell : cells) {
                        if (cell.status == DBSTATUS_S_ISNULL) {
                            decoded.emplace_back();
                        } else if (
                            cell.status != DBSTATUS_S_OK ||
                            cell.length >
                                sizeof(cell.value) - sizeof(wchar_t) ||
                            cell.length % sizeof(wchar_t) != 0) {
                            success = false;
                            break;
                        } else {
                            decoded.emplace_back(
                                cell.value,
                                cell.length / sizeof(wchar_t));
                        }
                    }
                    if (success)
                        output->push_back(std::move(decoded));
                }
            }
            if (count && rowHandles)
                rows->ReleaseRows(
                    count, rowHandles, nullptr, nullptr, nullptr);
            CoTaskMemFree(rowHandles);
        }
        accessor->ReleaseAccessor(handle, nullptr);
        return success;
    }

    unsigned timeout_seconds_ = 10;
    ComPtr<IDBCreateCommand> factory_;
};

bool ParseUnsigned(
    const std::wstring& value,
    std::uint64_t* output) noexcept {
    if (!output || value.empty() || value.front() == L'-')
        return false;
    errno = 0;
    wchar_t* end = nullptr;
    const auto converted = std::wcstoull(value.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end != L'\0')
        return false;
    *output = converted;
    return true;
}

bool ParseSigned(
    const std::wstring& value,
    std::int64_t* output) noexcept {
    if (!output || value.empty())
        return false;
    errno = 0;
    wchar_t* end = nullptr;
    const auto converted = std::wcstoll(value.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end != L'\0')
        return false;
    *output = converted;
    return true;
}

template<class T>
bool NarrowUnsigned(
    const std::wstring& value,
    T* output) noexcept {
    std::uint64_t converted = 0;
    if (!ParseUnsigned(value, &converted) ||
        converted > static_cast<std::uint64_t>(
            std::numeric_limits<T>::max()))
        return false;
    *output = static_cast<T>(converted);
    return true;
}

bool CopyUtf16(
    const std::wstring& input,
    char16_t* output,
    std::size_t capacity,
    std::uint16_t* length) noexcept {
    static_assert(sizeof(wchar_t) == sizeof(char16_t));
    if (!output || !length || input.size() > capacity ||
        input.size() > std::numeric_limits<std::uint16_t>::max())
        return false;
    const auto* utf16 =
        reinterpret_cast<const char16_t*>(input.data());
    if (!wire::ValidUtf16(utf16, input.size()))
        return false;
    std::copy(utf16, utf16 + input.size(), output);
    *length = static_cast<std::uint16_t>(input.size());
    return true;
}

std::wstring RequestIdLiteral(const RequestId& requestId) {
    std::wostringstream output;
    output << L"'" << std::hex << std::setfill(L'0');
    for (std::size_t index = 0;
         index < requestId.bytes.size(); ++index) {
        output << std::setw(2)
               << static_cast<unsigned>(requestId.bytes[index]);
        if (index == 3 || index == 5 ||
            index == 7 || index == 9)
            output << L"-";
    }
    output << L"'";
    return output.str();
}

std::wstring NoteLiteral(const Request& request) {
    static_assert(sizeof(wchar_t) == sizeof(char16_t));
    const std::wstring note(
        reinterpret_cast<const wchar_t*>(request.note.data()),
        request.note_code_units);
    return SqlLiteral(note);
}

bool SameDatabaseName(
    const std::wstring& left,
    const std::wstring& right) noexcept {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool VerifyBindings(
    NativeSession* game,
    NativeSession* member,
    const NativeDatabaseSessions& sessions,
    const std::wstring& expectedGame,
    const std::wstring& common) noexcept {
    if (!game || !member ||
        !game->Open(sessions.game) ||
        !member->Open(sessions.member) ||
        !game->Clean() || !member->Clean())
        return false;
    std::wstring gameName;
    std::wstring memberName;
    return game->Scalar(L"SELECT DB_NAME()", &gameName) &&
           member->Scalar(L"SELECT DB_NAME()", &memberName) &&
           SameDatabaseName(gameName, expectedGame) &&
           SameDatabaseName(memberName, common);
}

bool ValidateBindingsReadOnly(
    NativeSession* game,
    NativeSession* member,
    const NativeDatabaseSessions& sessions,
    const std::wstring& expectedGame,
    const std::wstring& common) noexcept {
    if (!game || !member ||
        !game->Open(sessions.game) ||
        !member->Open(sessions.member))
        return false;
    std::wstring gameName;
    std::wstring memberName;
    return game->Scalar(L"SELECT DB_NAME()", &gameName) &&
           member->Scalar(L"SELECT DB_NAME()", &memberName) &&
           SameDatabaseName(gameName, expectedGame) &&
           SameDatabaseName(memberName, common);
}

std::wstring OwnedSetPolicySql(
    const std::wstring& commonDatabase,
    const EligibilityPolicy& policy) {
    const auto modules = Modules(commonDatabase);
    std::wstring sql =
        L"DECLARE @gift_lock int=-999;\n"
        L"BEGIN TRY\n"
        L" EXEC @gift_lock=sys.sp_getapplock "
        L"@Resource=N'sdgo.friend_unit_gift.install.v1',"
        L"@LockMode=N'Exclusive',@LockOwner=N'Session',"
        L"@LockTimeout=10000,@DbPrincipal=N'public';\n"
        L" IF @gift_lock<0 RAISERROR("
        L"N'friend unit gift policy lock unavailable',16,1);\n";
    sql += detail::ExpectedObjectGuard(modules);
    sql +=
        L" IF ISNULL(CONVERT(nvarchar(128),(SELECT value "
        L"FROM sys.extended_properties WHERE class=3 AND "
        L"major_id=SCHEMA_ID(N'sdgo_friend_unit_gift') AND "
        L"name=N'sdgo_owner')),N'')<>" +
        SqlLiteral(kOwnerMarker) +
        L"\n  RAISERROR(N'friend unit gift owner differs',16,1);\n"
        L" IF NOT EXISTS(SELECT 1 FROM "
        L"[sdgo_friend_unit_gift].[State] WHERE Singleton=1 "
        L"AND OwnerMarker=" + SqlLiteral(kOwnerMarker) +
        L" AND SchemaVersion=" + std::to_wstring(kSchemaVersion) +
        L" AND DefinitionHash=N'" +
        DefinitionHash(commonDatabase) +
        L"')\n  RAISERROR(N'friend unit gift state differs',16,1);\n";
    for (const auto& module : modules) {
        const auto definition = SqlLiteral(module.definition);
        sql +=
            L" IF OBJECT_ID(N'[sdgo_friend_unit_gift].[" +
            module.name + L"]',N'P') IS NULL OR "
            L"DATALENGTH(OBJECT_DEFINITION(OBJECT_ID("
            L"N'[sdgo_friend_unit_gift].[" + module.name +
            L"]',N'P')))<>DATALENGTH(" + definition + L") OR "
            L"OBJECT_DEFINITION(OBJECT_ID("
            L"N'[sdgo_friend_unit_gift].[" + module.name +
            L"]',N'P')) COLLATE Latin1_General_100_BIN2<>" +
            definition +
            L" COLLATE Latin1_General_100_BIN2\n"
            L"  RAISERROR(N'friend unit gift module differs',16,1);\n"
            L" IF NOT EXISTS(SELECT 1 FROM "
            L"[sdgo_friend_unit_gift].[ModuleState] WHERE ObjectName=N'" +
            module.name + L"' AND DefinitionHash=N'" +
            StableHashText(module.definition) +
            L"' AND DATALENGTH(Definition)=DATALENGTH(" +
            definition + L") AND Definition COLLATE "
            L"Latin1_General_100_BIN2=" + definition +
            L" COLLATE Latin1_General_100_BIN2)\n"
            L"  RAISERROR(N'friend unit gift stored module differs',16,1);\n";
    }
    sql +=
        L" IF (SELECT COUNT(*) FROM "
        L"[sdgo_friend_unit_gift].[ModuleState])<>" +
        std::to_wstring(modules.size()) +
        L"\n  RAISERROR(N'friend unit gift module inventory differs',16,1);\n";
    const auto setPolicy = MakeSetPolicySql(policy, true);
    if (!setPolicy)
        return {};
    sql += L" " + *setPolicy + L"\n"
           L" EXEC sys.sp_releaseapplock "
           L"@Resource=N'sdgo.friend_unit_gift.install.v1',"
           L"@LockOwner=N'Session',@DbPrincipal=N'public';\n"
           L"END TRY\nBEGIN CATCH\n"
           L" IF @gift_lock>=0 EXEC sys.sp_releaseapplock "
           L"@Resource=N'sdgo.friend_unit_gift.install.v1',"
           L"@LockOwner=N'Session',@DbPrincipal=N'public';\n"
           L" DECLARE @gift_error nvarchar(2048)=ERROR_MESSAGE();\n"
           L" RAISERROR(N'friend unit gift policy ensure failed: %s',"
           L"16,1,@gift_error);\n"
           L"END CATCH;\n";
    return sql;
}

WireStatus StatusFromResult(const std::wstring& value) noexcept {
    std::int64_t code = 0;
    if (!ParseSigned(value, &code) || code < 0 || code > 12)
        return WireStatus::database_unavailable;
    return static_cast<WireStatus>(static_cast<std::int32_t>(code));
}

std::uint32_t Capabilities(
    bool offerEnabled,
    bool schemaPresent) noexcept {
    auto value =
        capability_protocol |
        capability_expanded_requests |
        capability_agent_validated;
    if (schemaPresent) {
        value |= capability_resolve_existing |
                 capability_provenance;
    }
    if (schemaPresent && offerEnabled)
        value |= capability_offer_enabled;
    return value;
}

bool ApplyMutation(
    const TextRows& rows,
    Reply* reply) noexcept {
    if (!reply || rows.size() != 1 || rows[0].size() != 5)
        return false;
    reply->status = StatusFromResult(rows[0][0]);
    if (reply->status == WireStatus::database_unavailable)
        return false;
    if (!rows[0][1].empty() &&
        !ParseUnsigned(rows[0][1], &reply->gift_id))
        return false;
    std::uint8_t state = 0;
    if (!NarrowUnsigned(rows[0][2], &state) ||
        !wire::ValidState(static_cast<GiftState>(state)))
        return false;
    reply->state = static_cast<GiftState>(state);
    if (!rows[0][4].empty() &&
        !ParseUnsigned(rows[0][4], &reply->unit_uid))
        return false;
    return true;
}

bool ApplyGiftDetail(
    const TextRows& rows,
    Reply* reply) noexcept {
    if (!reply || rows.size() != 1 || rows[0].size() != 13)
        return false;
    const auto& row = rows[0];
    std::uint8_t state = 0;
    std::uint8_t direction = 0;
    if (!ParseUnsigned(row[0], &reply->gift_id) ||
        !ParseUnsigned(row[1], &reply->unit_uid) ||
        !NarrowUnsigned(row[2], &reply->unit_number) ||
        !NarrowUnsigned(row[3], &state) ||
        !wire::ValidState(static_cast<GiftState>(state)) ||
        !NarrowUnsigned(row[4], &reply->peer_account) ||
        !CopyUtf16(
            row[5], reply->peer_name.data(),
            reply->peer_name.size(),
            &reply->peer_name_code_units) ||
        !CopyUtf16(
            row[6], reply->note.data(), reply->note.size(),
            &reply->note_code_units) ||
        !ParseUnsigned(row[7], &reply->offered_unix_millis) ||
        !ParseUnsigned(row[8], &reply->accepted_unix_millis) ||
        !NarrowUnsigned(row[9], &reply->level) ||
        !NarrowUnsigned(row[10], &reply->unit_class) ||
        !NarrowUnsigned(row[11], &reply->class_mark) ||
        !NarrowUnsigned(row[12], &direction) ||
        !wire::ValidDirection(
            static_cast<ListDirection>(direction)))
        return false;
    reply->state = static_cast<GiftState>(state);
    reply->direction = static_cast<ListDirection>(direction);
    reply->status = WireStatus::success;
    return true;
}

bool ApplyProvenance(
    const TextRows& rows,
    Reply* reply) noexcept {
    if (!reply || rows.size() != 1 || rows[0].size() != 12)
        return false;
    const auto& row = rows[0];
    std::uint8_t state = 0;
    if (!ParseUnsigned(row[0], &reply->gift_id) ||
        !ParseUnsigned(row[1], &reply->unit_uid) ||
        !NarrowUnsigned(row[2], &reply->unit_number) ||
        !NarrowUnsigned(row[3], &state) ||
        !wire::ValidState(static_cast<GiftState>(state)) ||
        !NarrowUnsigned(row[4], &reply->peer_account) ||
        !CopyUtf16(
            row[5], reply->peer_name.data(),
            reply->peer_name.size(),
            &reply->peer_name_code_units) ||
        !CopyUtf16(
            row[6], reply->note.data(), reply->note.size(),
            &reply->note_code_units) ||
        !ParseUnsigned(row[7], &reply->offered_unix_millis) ||
        !ParseUnsigned(row[8], &reply->accepted_unix_millis) ||
        !NarrowUnsigned(row[9], &reply->level) ||
        !NarrowUnsigned(row[10], &reply->unit_class) ||
        !NarrowUnsigned(row[11], &reply->class_mark))
        return false;
    reply->state = static_cast<GiftState>(state);
    reply->direction = ListDirection::inbox;
    reply->status = WireStatus::success;
    return true;
}

bool ApplyPreview(
    const TextRows& rows,
    Reply* reply) noexcept {
    if (!reply || rows.size() != 1 || rows[0].size() != 11)
        return false;
    const auto& row = rows[0];
    reply->status = StatusFromResult(row[0]);
    if (reply->status == WireStatus::database_unavailable)
        return false;
    std::uint8_t state = 0;
    if (!row[1].empty() &&
        !ParseUnsigned(row[1], &reply->gift_id))
        return false;
    if (!NarrowUnsigned(row[2], &state) ||
        !wire::ValidState(static_cast<GiftState>(state)) ||
        !ParseUnsigned(row[4], &reply->unit_uid))
        return false;
    reply->state = static_cast<GiftState>(state);
    if (!row[5].empty() &&
        !NarrowUnsigned(row[5], &reply->unit_number))
        return false;
    if (!row[6].empty() &&
        !NarrowUnsigned(row[6], &reply->level))
        return false;
    if (!row[7].empty() &&
        !NarrowUnsigned(row[7], &reply->unit_class))
        return false;
    if (!row[8].empty() &&
        !NarrowUnsigned(row[8], &reply->class_mark))
        return false;
    if (!NarrowUnsigned(row[9], &reply->peer_account) ||
        !CopyUtf16(
            row[10], reply->peer_name.data(),
            reply->peer_name.size(),
            &reply->peer_name_code_units))
        return false;
    reply->direction = ListDirection::inbox;
    return true;
}

bool ApplyList(
    const TextRows& rows,
    ListDirection direction,
    std::uint32_t page,
    Reply* reply) noexcept {
    if (!reply || page == 0)
        return false;
    reply->total_count =
        rows.size() >
                static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())
            ? std::numeric_limits<std::uint32_t>::max()
            : static_cast<std::uint32_t>(rows.size());
    const auto pageIndex =
        static_cast<std::uint64_t>(page - 1);
    const auto first = pageIndex * kMaximumRows;
    if (first > rows.size()) {
        reply->status = WireStatus::success;
        reply->direction = direction;
        return true;
    }
    const auto count = std::min<std::size_t>(
        kMaximumRows,
        rows.size() - static_cast<std::size_t>(first));
    for (std::size_t index = 0; index < count; ++index) {
        const auto& source =
            rows[static_cast<std::size_t>(first) + index];
        if (source.size() != 11)
            return false;
        auto& destination = reply->rows[index];
        std::uint8_t state = 0;
        if (!ParseUnsigned(source[0], &destination.gift_id) ||
            !ParseUnsigned(source[1], &destination.unit_uid) ||
            !NarrowUnsigned(source[2], &destination.unit_number) ||
            !NarrowUnsigned(source[3], &state) ||
            !wire::ValidState(static_cast<GiftState>(state)) ||
            !NarrowUnsigned(source[4], &destination.peer_account) ||
            !CopyUtf16(
                source[5], destination.peer_name.data(),
                destination.peer_name.size(),
                &destination.peer_name_code_units) ||
            !ParseUnsigned(
                source[7], &destination.offered_unix_millis) ||
            !NarrowUnsigned(source[8], &destination.level) ||
            !NarrowUnsigned(source[9], &destination.unit_class) ||
            !NarrowUnsigned(source[10], &destination.class_mark))
            return false;
        destination.state = static_cast<GiftState>(state);
        destination.direction = direction;
    }
    reply->row_count = static_cast<std::uint16_t>(count);
    reply->direction = direction;
    reply->status = WireStatus::success;
    return true;
}

std::wstring MutationSql(
    Command command,
    std::uint32_t actor,
    const Request& request) {
    const std::wstring prefix =
        L"EXEC [sdgo_friend_unit_gift].[";
    if (command == Command::offer) {
        return prefix + std::wstring(L"Offer] @ActorAccount=") +
            std::to_wstring(actor) + L",@RequestId=" +
            RequestIdLiteral(request.request_id) +
            L",@RecipientAccount=" +
            std::to_wstring(request.receiver_id) +
            L",@UserUnitNumber=" +
            std::to_wstring(request.unit_uid) +
            L",@Note=" + NoteLiteral(request) + L";";
    }
    const wchar_t* procedure = nullptr;
    switch (command) {
    case Command::accept: procedure = L"Accept"; break;
    case Command::return_to_sender: procedure = L"Return"; break;
    case Command::takeback: procedure = L"ClaimReturned"; break;
    default: return {};
    }
    return prefix + procedure + L"] @ActorAccount=" +
        std::to_wstring(actor) + L",@RequestId=" +
        RequestIdLiteral(request.request_id) + L",@GiftId=" +
        std::to_wstring(request.gift_id) + L";";
}

std::wstring ReceiptSql(
    std::uint32_t actor,
    const RequestId& requestId) {
    return
        L"EXEC [sdgo_friend_unit_gift].[QueryReceipt] "
        L"@ActorAccount=" + std::to_wstring(actor) +
        L",@RequestId=" + RequestIdLiteral(requestId) + L";";
}

bool RecoverReceipt(
    const NativeDatabaseSessions& sessions,
    const std::wstring& expectedGameDatabase,
    std::uint32_t actor,
    const RequestId& requestId,
    Reply* reply) noexcept {
    NativeSession recovery(10);
    if (!recovery.Open(sessions.game) || !recovery.Clean())
        return false;
    std::wstring database;
    if (!recovery.Scalar(L"SELECT DB_NAME()", &database) ||
        !SameDatabaseName(database, expectedGameDatabase))
        return false;
    TextRows rows;
    return recovery.Query(
               ReceiptSql(actor, requestId), 5, &rows, 1) &&
           ApplyMutation(rows, reply);
}

} // namespace

ServerDatabase::Configuration ServerDatabase::Snapshot() const {
    std::lock_guard<std::mutex> lock(configuration_mutex_);
    return configuration_;
}

bool ServerDatabase::Configure(
    std::wstring expectedGameDb,
    std::wstring commonDb,
    EligibilityPolicy policy) {
    if (!ValidIdentifier(expectedGameDb) ||
        !ValidIdentifier(commonDb) ||
        !ValidPolicy(policy))
        return false;
    std::lock_guard<std::mutex> lock(configuration_mutex_);
    configuration_.expected_game_db = std::move(expectedGameDb);
    configuration_.common_db = std::move(commonDb);
    configuration_.policy = policy;
    configuration_.configured = true;
    ready_.store(false, std::memory_order_release);
    schema_present_.store(false, std::memory_order_release);
    return true;
}

bool ServerDatabase::EnsureOwnedPolicy(
    const NativeDatabaseSessions& sessions,
    std::wstring* error) noexcept {
    auto fail = [&](const wchar_t* message) {
        ready_.store(false, std::memory_order_release);
        schema_present_.store(false, std::memory_order_release);
        if (error) *error = message;
        return false;
    };
    try {
        const auto configuration = Snapshot();
        if (!configuration.configured)
            return fail(L"friend unit gift database is not configured");
        NativeSession game(120);
        NativeSession member(10);
        if (!VerifyBindings(
                &game, &member, sessions,
                configuration.expected_game_db,
                configuration.common_db))
            return fail(L"native database session binding is unavailable");
        std::wstring schemaValue;
        if (!game.Scalar(
                L"SELECT CONVERT(nvarchar(1),CASE WHEN "
                L"SCHEMA_ID(N'sdgo_friend_unit_gift') IS NULL "
                L"THEN 0 ELSE 1 END)",
                &schemaValue) ||
            (schemaValue != L"0" && schemaValue != L"1"))
            return fail(L"friend unit gift schema state is unreadable");
        const bool schemaExists = schemaValue == L"1";
        if (!schemaExists && !configuration.policy.enabled) {
            schema_present_.store(false, std::memory_order_release);
            ready_.store(true, std::memory_order_release);
            if (error) error->clear();
            return true;
        }
        std::wstring sql;
        if (configuration.policy.enabled) {
            const auto install = MakeInstallSql(
                configuration.expected_game_db,
                configuration.common_db,
                configuration.policy);
            if (!install)
                return fail(L"friend unit gift install policy is invalid");
            sql = *install;
        } else {
            sql = OwnedSetPolicySql(
                configuration.common_db,
                configuration.policy);
            if (sql.empty())
                return fail(L"friend unit gift disable policy is invalid");
        }
        if (!game.Execute(sql))
            return fail(L"friend unit gift owned policy ensure failed");
        schema_present_.store(true, std::memory_order_release);
        ready_.store(true, std::memory_order_release);
        if (error) error->clear();
        return true;
    } catch (...) {
        return fail(L"friend unit gift owned policy ensure threw");
    }
}

bool ServerDatabase::ValidateSessionsReadOnly(
    const NativeDatabaseSessions& sessions,
    std::wstring* error) noexcept {
    try {
        const auto configuration = Snapshot();
        if (!configuration.configured) {
            if (error)
                *error = L"friend unit gift database is not configured";
            return false;
        }
        NativeSession game(10);
        NativeSession member(10);
        if (!ValidateBindingsReadOnly(
                &game, &member, sessions,
                configuration.expected_game_db,
                configuration.common_db)) {
            if (error)
                *error =
                    L"native database session binding is unavailable";
            return false;
        }
        if (error) error->clear();
        return true;
    } catch (...) {
        if (error)
            *error =
                L"native database session validation threw";
        return false;
    }
}

Reply ServerDatabase::ExecuteWireRequest(
    const NativeDatabaseSessions& sessions,
    std::uint32_t authenticatedActor,
    const Request& request) noexcept {
    Reply reply;
    reply.command = request.command;
    reply.flags = request.flags;
    reply.request_id = request.request_id;
    reply.gift_id = request.gift_id;
    reply.unit_uid = request.unit_uid;
    reply.receiver_id = request.receiver_id;
    reply.page = request.page;
    reply.server_epoch = ServerRuntimeEpoch();
    reply.status = WireStatus::database_unavailable;
    try {
        const bool generatedHello =
            request.command == Command::hello &&
            Empty(request.request_id) &&
            (request.flags & envelope_agent_validated) != 0;
        if (!authenticatedActor ||
            !wire::ValidRequest(request, generatedHello)) {
            reply.status = WireStatus::invalid_argument;
            return reply;
        }
        const auto configuration = Snapshot();
        const bool schemaPresent =
            schema_present_.load(std::memory_order_acquire);
        reply.capabilities =
            Capabilities(
                configuration.policy.enabled, schemaPresent);
        if (!configuration.configured ||
            !ready_.load(std::memory_order_acquire))
            return reply;
        NativeSession game(15);
        NativeSession member(10);
        if (!VerifyBindings(
                &game, &member, sessions,
                configuration.expected_game_db,
                configuration.common_db))
            return reply;
        if (request.command == Command::hello) {
            reply.status = WireStatus::success;
            return reply;
        }
        if (!schemaPresent) {
            if (request.command == Command::offer)
                reply.status = WireStatus::disabled;
            return reply;
        }

        TextRows rows;
        switch (request.command) {
        case Command::offer:
        case Command::accept:
        case Command::return_to_sender:
        case Command::takeback: {
            const auto sql = MutationSql(
                request.command, authenticatedActor, request);
            bool issued = false;
            const bool read =
                !sql.empty() &&
                game.Query(sql, 5, &rows, 1, &issued);
            if (read && ApplyMutation(rows, &reply))
                return reply;
            if (issued) {
                if (RecoverReceipt(
                        sessions,
                        configuration.expected_game_db,
                        authenticatedActor,
                        request.request_id,
                        &reply))
                    return reply;
                reply.status = WireStatus::outcome_unknown;
                return reply;
            }
            break;
        }
        case Command::list_inbox:
        case Command::list_returned:
        case Command::inventory_returns: {
            const bool inbox =
                request.command == Command::list_inbox;
            const auto sql =
                L"EXEC [sdgo_friend_unit_gift].[" +
                std::wstring(inbox ? L"QueryInbox" : request.command == Command::inventory_returns ? L"QueryInventoryReturns" : L"QueryReturned") +
                L"] @ActorAccount=" +
                std::to_wstring(authenticatedActor) + L";";
            if (game.Query(sql, 11, &rows) &&
                ApplyList(
                    rows,
                    inbox ? ListDirection::inbox
                          : ListDirection::returned,
                    request.page, &reply))
                return reply;
            break;
        }
        case Command::detail: {
            const auto sql =
                L"EXEC [sdgo_friend_unit_gift].[QueryGift] "
                L"@ActorAccount=" +
                std::to_wstring(authenticatedActor) +
                L",@GiftId=" + std::to_wstring(request.gift_id) +
                L";";
            if (game.Query(sql, 13, &rows, 1) &&
                ApplyGiftDetail(rows, &reply))
                return reply;
            if (rows.empty())
                reply.status = WireStatus::not_authorized;
            break;
        }
        case Command::receipt:
            if (RecoverReceipt(
                    sessions,
                    configuration.expected_game_db,
                    authenticatedActor,
                    request.request_id,
                    &reply))
                return reply;
            reply.status = WireStatus::outcome_unknown;
            return reply;
        case Command::provenance: {
            const auto sql =
                L"EXEC [sdgo_friend_unit_gift].[QueryProvenance] "
                L"@ActorAccount=" +
                std::to_wstring(authenticatedActor) +
                L",@UserUnitNumber=" +
                std::to_wstring(request.unit_uid) + L";";
            const bool queried = game.Query(sql, 12, &rows, 1);
            if (queried && ApplyProvenance(rows, &reply))
                return reply;
            // No accepted gift owns this unit for the actor: an ordinary unit,
            // not a database failure. (The fallback below is for failures.)
            if (queried && rows.empty()) {
                reply.status = WireStatus::not_authorized;
                return reply;
            }
            break;
        }
        case Command::preview_offer: {
            const auto sql =
                L"EXEC [sdgo_friend_unit_gift].[PreviewOffer] "
                L"@ActorAccount=" +
                std::to_wstring(authenticatedActor) +
                L",@RecipientAccount=" +
                std::to_wstring(request.receiver_id) +
                L",@UserUnitNumber=" +
                std::to_wstring(request.unit_uid) + L";";
            if (game.Query(sql, 11, &rows, 1) &&
                ApplyPreview(rows, &reply))
                return reply;
            break;
        }
        case Command::hello:
            break;
        }
    } catch (...) {
    }
    reply.status = WireStatus::database_unavailable;
    reply.row_count = 0;
    reply.note_code_units = 0;
    reply.peer_name_code_units = 0;
    return reply;
}

} // namespace sdgo::friend_unit_gift
