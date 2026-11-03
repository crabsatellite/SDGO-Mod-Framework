#pragma once
#define DBINITCONSTANTS
#include <windows.h>
#include <oledb.h>
#include <oledberr.h>
#include <transact.h>
#include <cstddef>
#include <cstring>
#include <string>

namespace sdgo::hangar_capacity {
template<class T> class ComPtr {
    T* value_ = nullptr;
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { if (value_) value_->Release(); }
    T* get() const { return value_; }
    T* operator->() const { return value_; }
    T** put() { if (value_) value_->Release(); value_ = nullptr; return &value_; }
};

enum class PolicyPhase : std::uint8_t {
    kNone,
    kInput,
    kOpen,
    kEnsure,
    kOptions,
    kApply,
    kVerify,
    kRestore,
    kNative,
};

enum class PolicyStage : std::uint8_t {
    kNone,
    kAccount,
    kConnection,
    kBorrowBusy,
    kBorrowCommand,
    kBorrowException,
    kGetDbSession,
    kFactory,
    kTransactionInterface,
    kTransactionActive,
    kPolicyBuild,
    kCreateCommand,
    kCommandProperties,
    kSetTimeout,
    kTimeoutStatus,
    kSetCommandText,
    kExecute,
    kResults,
    kGetResult,
    kAccessor,
    kCreateAccessor,
    kGetRows,
    kGetData,
    kValueStatus,
    kValueLength,
    kOptionsFormat,
    kTransactionCount,
    kImplicitTransaction,
    kStateMismatch,
    kException,
    kCapacityReadback,
};

struct PolicyFailure {
    PolicyPhase phase = PolicyPhase::kNone;
    PolicyStage stage = PolicyStage::kNone;
    HRESULT hr = S_OK;
};

inline const char* PolicyPhaseName(PolicyPhase phase) noexcept {
    switch (phase) {
    case PolicyPhase::kInput: return "input";
    case PolicyPhase::kOpen: return "open";
    case PolicyPhase::kEnsure: return "ensure";
    case PolicyPhase::kOptions: return "options";
    case PolicyPhase::kApply: return "apply";
    case PolicyPhase::kVerify: return "verify";
    case PolicyPhase::kRestore: return "restore";
    case PolicyPhase::kNative: return "native";
    default: return "none";
    }
}

inline const char* PolicyStageName(PolicyStage stage) noexcept {
    switch (stage) {
    case PolicyStage::kAccount: return "account";
    case PolicyStage::kConnection: return "connection";
    case PolicyStage::kBorrowBusy: return "borrow_busy";
    case PolicyStage::kBorrowCommand: return "borrow_command";
    case PolicyStage::kBorrowException: return "borrow_exception";
    case PolicyStage::kGetDbSession: return "get_db_session";
    case PolicyStage::kFactory: return "factory";
    case PolicyStage::kTransactionInterface: return "transaction_interface";
    case PolicyStage::kTransactionActive: return "transaction_active";
    case PolicyStage::kPolicyBuild: return "policy_build";
    case PolicyStage::kCreateCommand: return "create_command";
    case PolicyStage::kCommandProperties: return "command_properties";
    case PolicyStage::kSetTimeout: return "set_timeout";
    case PolicyStage::kTimeoutStatus: return "timeout_status";
    case PolicyStage::kSetCommandText: return "set_command_text";
    case PolicyStage::kExecute: return "execute";
    case PolicyStage::kResults: return "results";
    case PolicyStage::kGetResult: return "get_result";
    case PolicyStage::kAccessor: return "accessor";
    case PolicyStage::kCreateAccessor: return "create_accessor";
    case PolicyStage::kGetRows: return "get_rows";
    case PolicyStage::kGetData: return "get_data";
    case PolicyStage::kValueStatus: return "value_status";
    case PolicyStage::kValueLength: return "value_length";
    case PolicyStage::kOptionsFormat: return "options_format";
    case PolicyStage::kTransactionCount: return "transaction_count";
    case PolicyStage::kImplicitTransaction: return "implicit_transaction";
    case PolicyStage::kStateMismatch: return "state_mismatch";
    case PolicyStage::kException: return "exception";
    case PolicyStage::kCapacityReadback: return "capacity_readback";
    default: return "none";
    }
}

inline void SetPolicyFailure(
    PolicyFailure* failure,
    PolicyPhase phase,
    PolicyStage stage,
    HRESULT hr) noexcept {
    if (failure)
        *failure = PolicyFailure{phase, stage, hr};
}

// Same verified World connection layout as the native command adapters. The
// original command and its parameter bindings are never changed by this Mod.
inline bool BorrowCommand(
    void* connection,
    ICommandText** command,
    PolicyFailure* failure = nullptr) {
    if (!command) {
        SetPolicyFailure(
            failure, PolicyPhase::kOpen, PolicyStage::kBorrowCommand, E_POINTER);
        return false;
    }
    *command = nullptr;
    __try {
        if (!connection) {
            SetPolicyFailure(
                failure, PolicyPhase::kOpen, PolicyStage::kConnection, E_POINTER);
            return false;
        }
        if (*reinterpret_cast<int*>(
                static_cast<BYTE*>(connection) + 0x240)) {
            SetPolicyFailure(
                failure,
                PolicyPhase::kOpen,
                PolicyStage::kBorrowBusy,
                HRESULT_FROM_WIN32(ERROR_BUSY));
            return false;
        }
        *command = *reinterpret_cast<ICommandText**>(static_cast<BYTE*>(connection) + 8);
        if (!*command) {
            SetPolicyFailure(
                failure,
                PolicyPhase::kOpen,
                PolicyStage::kBorrowCommand,
                E_POINTER);
            return false;
        }
        (*command)->AddRef();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *command = nullptr;
        SetPolicyFailure(
            failure,
            PolicyPhase::kOpen,
            PolicyStage::kBorrowException,
            E_UNEXPECTED);
        return false;
    }
}

class PolicySession {
    ComPtr<IDBCreateCommand> factory_;
    unsigned timeout_;
    PolicyFailure failure_{};

    bool Fail(PolicyPhase phase, PolicyStage stage, HRESULT hr) noexcept {
        failure_ = PolicyFailure{phase, stage, hr};
        return false;
    }

    bool Create(
        const std::wstring& sql,
        PolicyPhase phase,
        ComPtr<ICommandText>& command) {
        if (!factory_.get())
            return Fail(phase, PolicyStage::kFactory, E_UNEXPECTED);
        HRESULT hr = factory_->CreateCommand(
            nullptr,
            __uuidof(ICommandText),
            reinterpret_cast<IUnknown**>(command.put()));
        if (FAILED(hr))
            return Fail(phase, PolicyStage::kCreateCommand, hr);
        if (!command.get())
            return Fail(phase, PolicyStage::kCreateCommand, E_NOINTERFACE);
        ComPtr<ICommandProperties> properties;
        hr = command->QueryInterface(
            __uuidof(ICommandProperties),
            reinterpret_cast<void**>(properties.put()));
        if (FAILED(hr))
            return Fail(phase, PolicyStage::kCommandProperties, hr);
        DBPROP property{};
        property.dwPropertyID = DBPROP_COMMANDTIMEOUT;
        property.dwOptions = DBPROPOPTIONS_REQUIRED;
        property.colid = DB_NULLID;
        property.vValue.vt = VT_I4;
        property.vValue.lVal = static_cast<LONG>(timeout_);
        DBPROPSET set{&property, 1, DBPROPSET_ROWSET};
        hr = properties->SetProperties(1, &set);
        if (FAILED(hr))
            return Fail(phase, PolicyStage::kSetTimeout, hr);
        if (property.dwStatus != DBPROPSTATUS_OK)
            return Fail(
                phase,
                PolicyStage::kTimeoutStatus,
                DB_E_ERRORSOCCURRED);
        hr = command->SetCommandText(DBGUID_DBSQL, sql.c_str());
        if (FAILED(hr))
            return Fail(phase, PolicyStage::kSetCommandText, hr);
        return true;
    }

    bool Scalar(
        const std::wstring& sql,
        PolicyPhase phase,
        std::wstring& value) {
        ComPtr<ICommandText> command;
        if (!Create(sql, phase, command))
            return false;
        ComPtr<IMultipleResults> results;
        ComPtr<IRowset> rows;
        DBROWCOUNT affected = 0;
        HRESULT hr = command->Execute(
            nullptr,
            __uuidof(IMultipleResults),
            nullptr,
            &affected,
            reinterpret_cast<IUnknown**>(results.put()));
        if (FAILED(hr))
            return Fail(phase, PolicyStage::kExecute, hr);
        if (!results.get())
            return Fail(phase, PolicyStage::kResults, E_NOINTERFACE);
        for (;;) {
            hr = results->GetResult(nullptr, DBRESULTFLAG_DEFAULT, __uuidof(IRowset),
                &affected, reinterpret_cast<IUnknown**>(rows.put()));
            if (FAILED(hr))
                return Fail(phase, PolicyStage::kGetResult, hr);
            if (hr == DB_S_NORESULT)
                return Fail(phase, PolicyStage::kGetResult, DB_E_NOTFOUND);
            if (rows.get()) break;
        }
        ComPtr<IAccessor> access;
        hr = rows->QueryInterface(
            __uuidof(IAccessor),
            reinterpret_cast<void**>(access.put()));
        if (FAILED(hr))
            return Fail(phase, PolicyStage::kAccessor, hr);
        struct Cell { DBSTATUS status; DBLENGTH length; wchar_t text[96]; } cell{};
        DBBINDING binding{};
        binding.iOrdinal = 1; binding.obStatus = offsetof(Cell, status);
        binding.obLength = offsetof(Cell, length); binding.obValue = offsetof(Cell, text);
        binding.dwPart = DBPART_VALUE | DBPART_STATUS | DBPART_LENGTH;
        binding.dwMemOwner = DBMEMOWNER_CLIENTOWNED; binding.wType = DBTYPE_WSTR;
        binding.cbMaxLen = sizeof(cell.text);
        HACCESSOR handle = DB_NULL_HACCESSOR;
        DBBINDSTATUS status = DBBINDSTATUS_BADBINDINFO;
        hr = access->CreateAccessor(
            DBACCESSOR_ROWDATA,
            1,
            &binding,
            sizeof(cell),
            &handle,
            &status);
        if (FAILED(hr))
            return Fail(phase, PolicyStage::kCreateAccessor, hr);
        if (status != DBBINDSTATUS_OK)
            return Fail(phase, PolicyStage::kCreateAccessor, DB_E_BADBINDINFO);
        DBCOUNTITEM count = 0; HROW* data = nullptr;
        hr = rows->GetNextRows(
            DB_NULL_HCHAPTER, 0, 1, &count, &data);
        if (FAILED(hr)) {
            access->ReleaseAccessor(handle, nullptr);
            return Fail(phase, PolicyStage::kGetRows, hr);
        }
        if (count != 1 || !data) {
            CoTaskMemFree(data);
            access->ReleaseAccessor(handle, nullptr);
            return Fail(phase, PolicyStage::kGetRows, DB_E_NOTFOUND);
        }
        hr = rows->GetData(data[0], handle, &cell);
        if (FAILED(hr)) {
            rows->ReleaseRows(count, data, nullptr, nullptr, nullptr);
            CoTaskMemFree(data);
            access->ReleaseAccessor(handle, nullptr);
            return Fail(phase, PolicyStage::kGetData, hr);
        }
        bool ok = true;
        if (cell.status != DBSTATUS_S_OK) {
            Fail(phase, PolicyStage::kValueStatus, DB_E_ERRORSOCCURRED);
            ok = false;
        } else if (
            cell.length >= sizeof(cell.text) ||
            cell.length % sizeof(wchar_t) != 0) {
            Fail(phase, PolicyStage::kValueLength, DB_E_BADBINDINFO);
            ok = false;
        } else {
            value.assign(cell.text, cell.length / sizeof(wchar_t));
        }
        if (count && data) rows->ReleaseRows(count, data, nullptr, nullptr, nullptr);
        CoTaskMemFree(data); access->ReleaseAccessor(handle, nullptr);
        return ok;
    }

    bool Exec(const std::wstring& sql, PolicyPhase phase) {
        ComPtr<ICommandText> command;
        if (!Create(sql, phase, command))
            return false;
        ComPtr<IMultipleResults> results;
        DBROWCOUNT affected = 0;
        HRESULT hr = command->Execute(
            nullptr,
            __uuidof(IMultipleResults),
            nullptr,
            &affected,
            reinterpret_cast<IUnknown**>(results.put()));
        if (FAILED(hr))
            return Fail(phase, PolicyStage::kExecute, hr);
        if (!results.get())
            return Fail(phase, PolicyStage::kResults, E_NOINTERFACE);
        // Some provider errors are delivered after an earlier row-count token.
        // Consume the whole policy batch before declaring its commit successful.
        ComPtr<IRowset> row;
        for (;;) {
            hr = results->GetResult(nullptr, DBRESULTFLAG_DEFAULT, __uuidof(IRowset),
                &affected, reinterpret_cast<IUnknown**>(row.put()));
            if (hr == DB_S_NORESULT) return true;
            if (FAILED(hr))
                return Fail(phase, PolicyStage::kGetResult, hr);
        }
    }
public:
    explicit PolicySession(unsigned timeout) : timeout_(timeout) {}

    PolicyFailure Failure() const noexcept { return failure_; }

    bool Open(void* connection) {
        failure_ = {};
        ComPtr<ICommandText> original;
        if (!BorrowCommand(connection, original.put(), &failure_))
            return false;
        const HRESULT hr = original->GetDBSession(
            __uuidof(IDBCreateCommand),
            reinterpret_cast<IUnknown**>(factory_.put()));
        if (FAILED(hr))
            return Fail(
                PolicyPhase::kOpen,
                PolicyStage::kGetDbSession,
                hr);
        if (!factory_.get())
            return Fail(
                PolicyPhase::kOpen,
                PolicyStage::kFactory,
                E_NOINTERFACE);
        return true;
    }

    bool Ensure(const std::wstring& sql) {
        failure_ = {};
        if (!factory_.get())
            return Fail(
                PolicyPhase::kEnsure,
                PolicyStage::kFactory,
                E_UNEXPECTED);
        // SQLOLEDB can defer a COM transaction before the first table access;
        // @@TRANCOUNT alone is therefore not enough to identify caller work.
        ComPtr<ITransactionLocal> transaction;
        XACTTRANSINFO active{};
        HRESULT hr = factory_->QueryInterface(
            __uuidof(ITransactionLocal),
            reinterpret_cast<void**>(transaction.put()));
        if (FAILED(hr))
            return Fail(
                PolicyPhase::kEnsure,
                PolicyStage::kTransactionInterface,
                hr);
        hr = transaction->GetTransactionInfo(&active);
        if (SUCCEEDED(hr))
            return Fail(
                PolicyPhase::kEnsure,
                PolicyStage::kTransactionActive,
                XACT_E_XTIONEXISTS);
        std::wstring initial;
        if (!Scalar(
                L"SELECT CONVERT(nvarchar(12),@@TRANCOUNT)+N'|'+CONVERT(nvarchar(12),@@OPTIONS)",
                PolicyPhase::kOptions,
                initial))
            return false;
        const auto separator = initial.find(L'|');
        if (separator == std::wstring::npos)
            return Fail(
                PolicyPhase::kOptions,
                PolicyStage::kOptionsFormat,
                E_INVALIDARG);
        if (initial.substr(0, separator) != L"0")
            return Fail(
                PolicyPhase::kOptions,
                PolicyStage::kTransactionCount,
                XACT_E_XTIONEXISTS);
        const std::wstring optionText = initial.substr(separator + 1);
        if (optionText.empty() ||
            optionText.find_first_not_of(L"0123456789") !=
                std::wstring::npos ||
            optionText.size() > 10)
            return Fail(
                PolicyPhase::kOptions,
                PolicyStage::kOptionsFormat,
                E_INVALIDARG);
        unsigned long options = 0;
        try {
            options = std::stoul(optionText);
        } catch (...) {
            return Fail(
                PolicyPhase::kOptions,
                PolicyStage::kOptionsFormat,
                E_INVALIDARG);
        }
        if (options & 2u)
            return Fail(
                PolicyPhase::kOptions,
                PolicyStage::kImplicitTransaction,
                XACT_E_XTIONEXISTS);
        const auto restore = [&]() {
            const std::wstring cleanup = std::wstring(L"IF @@TRANCOUNT>0 ROLLBACK TRANSACTION; SET NOCOUNT ") +
                ((options & 512u) ? L"ON;" : L"OFF;") + L"SET XACT_ABORT " +
                ((options & 16384u) ? L"ON;" : L"OFF;");
            const PolicyFailure primary = failure_;
            const bool restored = Exec(cleanup, PolicyPhase::kRestore);
            if (primary.stage != PolicyStage::kNone)
                failure_ = primary;
            return restored;
        };
        // An OLE DB command timeout/attention can bypass T-SQL CATCH. Since the
        // leased session had no caller transaction, rollback is ours alone.
        try {
            if (!Exec(sql, PolicyPhase::kApply)) {
                restore();
                return false;
            }
            std::wstring final;
            if (!Scalar(
                    L"SELECT CONVERT(nvarchar(12),@@TRANCOUNT)+N'|'+CONVERT(nvarchar(12),@@OPTIONS)",
                    PolicyPhase::kVerify,
                    final)) {
                restore();
                return false;
            }
            if (final != initial) {
                Fail(
                    PolicyPhase::kVerify,
                    PolicyStage::kStateMismatch,
                    E_UNEXPECTED);
                restore();
                return false;
            }
            return true;
        } catch (...) {
            Fail(
                PolicyPhase::kEnsure,
                PolicyStage::kException,
                E_UNEXPECTED);
            try {
                restore();
            } catch (...) {
            }
            return false;
        }
    }
};
} // namespace sdgo::hangar_capacity
