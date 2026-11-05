#pragma once

#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"

// Passive, test-only ownership observer for records published into the native
// grid. It does not initiate cleanup, manufacture native objects, or claim
// skill/funnel actor lifecycle acceptance.
class NativeRecordLifecycleProbe {
    static constexpr std::size_t kMaximumRecords = 16;
    static constexpr std::size_t kMaximumRecordBytes = 4976;
    static constexpr std::size_t kMaximumRunIdBytes = 255;
    static constexpr std::size_t kMaximumReceiptPathCharacters = 32767;
    static constexpr std::size_t kReceiptBufferBytes = 512 * 1024;

    using ClearGrid = void(__thiscall*)(void*);
    using AppFinalRelease = void(__thiscall*)(void*);
    using InsertNode = void(__thiscall*)(void*, void*, void*, char);
    using RegisterRecord =
        void(__thiscall*)(void*, const void*, std::uint32_t, std::uint32_t);
    using Delete = void(__cdecl*)(void*);
    using MapFind =
        void(__thiscall*)(void*, void**, const std::uint32_t*);

    struct Record {
        std::uint32_t expectedId = 0;
        std::uint32_t kind = 0;
        std::uint32_t recordBytes = 0;
        std::array<std::uint8_t, kMaximumRecordBytes> bytes{};

        std::uintptr_t raw = 0;
        std::uintptr_t wrapper = 0;
        std::uintptr_t node = 0;
        std::uintptr_t hashNode = 0;
        std::uintptr_t hashValue = 0;

        volatile LONG recordCaptured = 0;
        volatile LONG insertCaptures = 0;
        volatile LONG registerCaptures = 0;
        volatile LONG integrityCaptured = 0;
        volatile LONG conflict = 0;

        volatile LONG rawFrees = 0;
        volatile LONG wrapperFrees = 0;
        volatile LONG nodeFrees = 0;
        volatile LONG hashNodeFrees = 0;
        volatile LONG hashValueFrees = 0;
    };

    struct JsonWriter {
        char* output = nullptr;
        std::size_t capacity = 0;
        std::size_t length = 0;
        bool valid = true;

        void Character(char value) {
            if (!valid || length >= capacity) {
                valid = false;
                return;
            }
            output[length++] = value;
        }

        void Text(const char* value) {
            if (!value) {
                valid = false;
                return;
            }
            while (*value) Character(*value++);
        }

        void Unsigned(std::uint64_t value) {
            char digits[32]{};
            std::size_t count = 0;
            do {
                digits[count++] = static_cast<char>('0' + value % 10);
                value /= 10;
            } while (value && count < sizeof(digits));
            while (count) Character(digits[--count]);
        }

        void Boolean(bool value) {
            Text(value ? "true" : "false");
        }

        void Pointer(std::uintptr_t value) {
            static constexpr char kHex[] = "0123456789abcdef";
            Character('"');
            Text("0x");
            for (std::size_t nibble = sizeof(std::uintptr_t) * 2; nibble; --nibble) {
                const unsigned shift = static_cast<unsigned>((nibble - 1) * 4);
                Character(kHex[(value >> shift) & 0x0f]);
            }
            Character('"');
        }

        void Escaped(const char* value) {
            static constexpr char kHex[] = "0123456789abcdef";
            Character('"');
            if (value) {
                for (const auto* cursor =
                         reinterpret_cast<const unsigned char*>(value);
                     *cursor; ++cursor) {
                    const unsigned char character = *cursor;
                    if (character == '"' || character == '\\') {
                        Character('\\');
                        Character(static_cast<char>(character));
                    } else if (character == '\b') {
                        Text("\\b");
                    } else if (character == '\f') {
                        Text("\\f");
                    } else if (character == '\n') {
                        Text("\\n");
                    } else if (character == '\r') {
                        Text("\\r");
                    } else if (character == '\t') {
                        Text("\\t");
                    } else if (character < 0x20) {
                        Text("\\u00");
                        Character(kHex[(character >> 4) & 0x0f]);
                        Character(kHex[character & 0x0f]);
                    } else {
                        Character(static_cast<char>(character));
                    }
                }
            }
            Character('"');
        }

        void HexBytes(const std::uint8_t* value, std::size_t bytes) {
            static constexpr char kHex[] = "0123456789abcdef";
            Character('"');
            for (std::size_t index = 0; valid && index < bytes; ++index) {
                Character(kHex[value[index] >> 4]);
                Character(kHex[value[index] & 0x0f]);
            }
            Character('"');
        }
    };

    inline static NativeRecordLifecycleProbe* active_ = nullptr;
    inline static ClearGrid clearGrid_ = nullptr;
    inline static AppFinalRelease appFinalRelease_ = nullptr;
    inline static InsertNode insertNode_ = nullptr;
    inline static RegisterRecord registerRecord_ = nullptr;
    inline static Delete delete_ = nullptr;
    inline static MapFind mapFind_ = nullptr;
    inline static volatile LONG* gridSingleton_ = nullptr;

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    std::array<Record, kMaximumRecords> records_{};
    std::size_t recordCount_ = 0;
    std::array<char, kMaximumRunIdBytes + 1> runId_{};
    std::array<wchar_t, kMaximumReceiptPathCharacters + 1> receiptPath_{};
    std::array<char, kReceiptBufferBytes> receiptBuffer_{};

    volatile LONG appFinalEntered_ = 0;
    volatile LONG appFinalReturned_ = 0;
    volatile LONG appFinalActive_ = 0;
    volatile LONG clearEntered_ = 0;
    volatile LONG clearReturned_ = 0;
    volatile LONG clearActive_ = 0;
    volatile LONG singletonNull_ = 0;
    volatile LONG receiptAttempted_ = 0;
    DWORD appFinalThread_ = 0;
    DWORD clearThread_ = 0;
    std::uintptr_t finalGrid_ = 0;

    static std::uint32_t ExpectedBytes(std::uint32_t kind) {
        switch (kind) {
        case 2: return 4976;
        case 10: return 1871;
        case 11: return 732;
        case 13: return 1132;
        default: return 0;
        }
    }

    static bool ReadU8(const void* address, std::uint8_t* value) {
        __try {
            std::memcpy(value, address, sizeof(*value));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool ReadU32(const void* address, std::uint32_t* value) {
        __try {
            std::memcpy(value, address, sizeof(*value));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool ReadNativePointer(const void* address, void** value) {
        std::uint32_t native = 0;
        if (!ReadU32(address, &native)) return false;
        *value = reinterpret_cast<void*>(static_cast<std::uintptr_t>(native));
        return true;
    }

    void* ReadSingleton() const {
        if (!gridSingleton_) return nullptr;
        const LONG native =
            InterlockedCompareExchange(gridSingleton_, 0, 0);
        return reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(static_cast<std::uint32_t>(native)));
    }

    Record* Find(std::uint32_t id) {
        for (std::size_t index = 0; index < recordCount_; ++index)
            if (records_[index].expectedId == id) return &records_[index];
        return nullptr;
    }

    static void MarkConflict(Record& record) {
        InterlockedExchange(&record.conflict, 1);
    }

    static void AssignPointer(
        Record& record, std::uintptr_t& destination, const void* value) {
        const auto address = reinterpret_cast<std::uintptr_t>(value);
        if (!address) {
            MarkConflict(record);
        } else if (!destination) {
            destination = address;
        } else if (destination != address) {
            MarkConflict(record);
        }
    }

    void CaptureRecord(
        Record& record, std::uint32_t kind, const void* raw,
        std::uint32_t bytes) {
        const std::uint32_t expected = ExpectedBytes(kind);
        if (!raw || !expected || expected != bytes ||
            bytes > kMaximumRecordBytes) {
            MarkConflict(record);
            return;
        }

        std::uint32_t id = 0;
        if (!ReadU32(raw, &id) || id != record.expectedId) {
            MarkConflict(record);
            return;
        }

        __try {
            if (!record.recordCaptured) {
                std::memcpy(record.bytes.data(), raw, bytes);
                record.kind = kind;
                record.recordBytes = bytes;
                record.raw = reinterpret_cast<std::uintptr_t>(raw);
                InterlockedExchange(&record.recordCaptured, 1);
            } else {
                if (record.kind != kind || record.recordBytes != bytes ||
                    record.raw != reinterpret_cast<std::uintptr_t>(raw) ||
                    std::memcmp(record.bytes.data(), raw, bytes) != 0) {
                    MarkConflict(record);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            MarkConflict(record);
        }
    }

    void CaptureIntegrity(void* grid, Record& record) {
        if (!grid || !record.raw || !mapFind_) return;
        const auto rawAddress =
            static_cast<std::uint32_t>(record.raw & 0xffffffffu);
        void* node = nullptr;
        void* sentinel = nullptr;
        void* value = nullptr;
        __try {
            auto* integrity =
                static_cast<std::uint8_t*>(grid) + 0x10;
            mapFind_(integrity, &node, &rawAddress);
            if (!ReadNativePointer(
                    static_cast<std::uint8_t*>(grid) + 0x14,
                    &sentinel) ||
                !node || node == sentinel ||
                !ReadNativePointer(
                    static_cast<std::uint8_t*>(node) + 0x10,
                    &value) ||
                !value) {
                return;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            MarkConflict(record);
            return;
        }

        AssignPointer(record, record.hashNode, node);
        AssignPointer(record, record.hashValue, value);
        if (record.hashNode && record.hashValue)
            InterlockedExchange(&record.integrityCaptured, 1);
    }

    void ObserveInserted(void* grid, void* result) {
        void* const singleton = ReadSingleton();
        if (!grid || grid != singleton || !singleton || !result) return;
        std::uint8_t inserted = 0;
        void* node = nullptr;
        if (!ReadU8(static_cast<std::uint8_t*>(result) + 4, &inserted) ||
            !inserted || !ReadNativePointer(result, &node) || !node) {
            return;
        }

        std::uint32_t id = 0;
        void* wrapper = nullptr;
        std::uint32_t kind = 0;
        void* raw = nullptr;
        if (!ReadU32(static_cast<std::uint8_t*>(node) + 0x0c, &id))
            return;
        Record* record = Find(id);
        if (!record) return;
        InterlockedIncrement(&record->insertCaptures);

        if (!ReadNativePointer(
                static_cast<std::uint8_t*>(node) + 0x10,
                &wrapper) ||
            !wrapper ||
            !ReadU32(wrapper, &kind) ||
            !ReadNativePointer(
                static_cast<std::uint8_t*>(wrapper) + 4,
                &raw) ||
            !raw) {
            MarkConflict(*record);
            return;
        }

        AssignPointer(*record, record->node, node);
        AssignPointer(*record, record->wrapper, wrapper);
        CaptureRecord(*record, kind, raw, ExpectedBytes(kind));
        CaptureIntegrity(grid, *record);
    }

    void ObserveRegister(
        void* grid, const void* raw, std::uint32_t bytes,
        std::uint32_t kind) {
        void* const singleton = ReadSingleton();
        if (!grid || grid != singleton || !singleton) return;
        std::uint32_t id = 0;
        if (!raw || !ReadU32(raw, &id)) return;
        Record* record = Find(id);
        if (!record) return;
        InterlockedIncrement(&record->registerCaptures);
        CaptureRecord(*record, kind, raw, bytes);
        CaptureIntegrity(grid, *record);
    }

    void ObserveFreed(void* value) {
        if (!value || !appFinalActive_ || !clearActive_ ||
            GetCurrentThreadId() != clearThread_ ||
            clearThread_ != appFinalThread_ ||
            !finalGrid_) {
            return;
        }
        const auto address = reinterpret_cast<std::uintptr_t>(value);
        for (std::size_t index = 0; index < recordCount_; ++index) {
            Record& record = records_[index];
            if (record.raw == address)
                InterlockedIncrement(&record.rawFrees);
            if (record.wrapper == address)
                InterlockedIncrement(&record.wrapperFrees);
            if (record.node == address)
                InterlockedIncrement(&record.nodeFrees);
            if (record.hashNode == address)
                InterlockedIncrement(&record.hashNodeFrees);
            if (record.hashValue == address)
                InterlockedIncrement(&record.hashValueFrees);
        }
    }

    static bool DistinctPointers(const Record& record) {
        const std::uintptr_t values[] = {
            record.raw, record.wrapper, record.node,
            record.hashNode, record.hashValue,
        };
        for (std::size_t left = 0; left < std::size(values); ++left) {
            if (!values[left]) return false;
            for (std::size_t right = left + 1;
                 right < std::size(values); ++right) {
                if (values[left] == values[right]) return false;
            }
        }
        return true;
    }

    static std::uintptr_t PointerAt(
        const Record& record, std::size_t index) {
        switch (index) {
        case 0: return record.raw;
        case 1: return record.wrapper;
        case 2: return record.node;
        case 3: return record.hashNode;
        case 4: return record.hashValue;
        default: return 0;
        }
    }

    bool CrossRecordPointersDistinct() const {
        for (std::size_t left = 0; left < recordCount_; ++left) {
            for (std::size_t right = left + 1;
                 right < recordCount_; ++right) {
                for (std::size_t leftPointer = 0;
                     leftPointer < 5; ++leftPointer) {
                    const auto leftAddress =
                        PointerAt(records_[left], leftPointer);
                    if (!leftAddress) continue;
                    for (std::size_t rightPointer = 0;
                         rightPointer < 5; ++rightPointer) {
                        if (leftAddress ==
                            PointerAt(records_[right], rightPointer)) {
                            return false;
                        }
                    }
                }
            }
        }
        return true;
    }

    bool RecordPassed(const Record& record) const {
        return record.recordCaptured == 1 &&
            record.insertCaptures == 1 &&
            record.registerCaptures == 1 &&
            record.integrityCaptured == 1 &&
            record.conflict == 0 &&
            ExpectedBytes(record.kind) == record.recordBytes &&
            DistinctPointers(record) &&
            record.rawFrees == 1 &&
            record.wrapperFrees == 1 &&
            record.nodeFrees == 1 &&
            record.hashNodeFrees == 1 &&
            record.hashValueFrees == 1;
    }

    bool Passed() const {
        if (!recordCount_ ||
            appFinalEntered_ != 1 || appFinalReturned_ != 1 ||
            clearEntered_ != 1 || clearReturned_ != 1 ||
            singletonNull_ != 1) {
            return false;
        }
        for (std::size_t index = 0; index < recordCount_; ++index)
            if (!RecordPassed(records_[index])) return false;
        return CrossRecordPointersDistinct();
    }

    void BuildReceipt(JsonWriter& json) const {
        json.Text("{\"schema\":\"sdgo.native-record-lifecycle/1\",");
        json.Text("\"observer_scope\":\"registration-and-native-grid-destruction-only\",");
        json.Text("\"run_id\":");
        json.Escaped(runId_.data());
        json.Text(",\"expected_ids\":[");
        for (std::size_t index = 0; index < recordCount_; ++index) {
            if (index) json.Character(',');
            json.Unsigned(records_[index].expectedId);
        }
        json.Text("],\"clear_entered\":");
        json.Unsigned(static_cast<std::uint32_t>(clearEntered_));
        json.Text(",\"clear_returned\":");
        json.Unsigned(static_cast<std::uint32_t>(clearReturned_));
        json.Text(",\"app_final_entered\":");
        json.Unsigned(static_cast<std::uint32_t>(appFinalEntered_));
        json.Text(",\"app_final_returned\":");
        json.Unsigned(static_cast<std::uint32_t>(appFinalReturned_));
        json.Text(",\"singleton_null\":");
        json.Boolean(singletonNull_ == 1);
        json.Text(",\"cross_record_pointers_distinct\":");
        json.Boolean(CrossRecordPointersDistinct());
        json.Text(",\"records\":[");

        for (std::size_t index = 0; index < recordCount_; ++index) {
            const Record& record = records_[index];
            if (index) json.Character(',');
            json.Text("{\"id\":");
            json.Unsigned(record.expectedId);
            json.Text(",\"kind\":");
            json.Unsigned(record.kind);
            json.Text(",\"record_size\":");
            json.Unsigned(record.recordBytes);
            json.Text(",\"record_hex\":");
            json.HexBytes(
                record.bytes.data(),
                record.recordCaptured == 1 ? record.recordBytes : 0);
            json.Text(",\"pointers\":{\"raw\":");
            json.Pointer(record.raw);
            json.Text(",\"wrapper\":");
            json.Pointer(record.wrapper);
            json.Text(",\"node\":");
            json.Pointer(record.node);
            json.Text(",\"hash_node\":");
            json.Pointer(record.hashNode);
            json.Text(",\"hash_value\":");
            json.Pointer(record.hashValue);
            json.Text("},\"captures\":{\"record\":");
            json.Unsigned(static_cast<std::uint32_t>(
                record.recordCaptured));
            json.Text(",\"insert\":");
            json.Unsigned(static_cast<std::uint32_t>(
                record.insertCaptures));
            json.Text(",\"register\":");
            json.Unsigned(static_cast<std::uint32_t>(
                record.registerCaptures));
            json.Text(",\"integrity\":");
            json.Unsigned(static_cast<std::uint32_t>(
                record.integrityCaptured));
            json.Text(",\"conflict\":");
            json.Boolean(record.conflict != 0);
            json.Text("},\"free_counts\":{\"raw\":");
            json.Unsigned(static_cast<std::uint32_t>(record.rawFrees));
            json.Text(",\"wrapper\":");
            json.Unsigned(static_cast<std::uint32_t>(
                record.wrapperFrees));
            json.Text(",\"node\":");
            json.Unsigned(static_cast<std::uint32_t>(record.nodeFrees));
            json.Text(",\"hash_node\":");
            json.Unsigned(static_cast<std::uint32_t>(
                record.hashNodeFrees));
            json.Text(",\"hash_value\":");
            json.Unsigned(static_cast<std::uint32_t>(
                record.hashValueFrees));
            json.Text("},\"passed\":");
            json.Boolean(RecordPassed(record));
            json.Character('}');
        }
        json.Text("],\"passed\":");
        json.Boolean(Passed());
        json.Text("}\r\n");
    }

    void WriteReceipt() {
        if (InterlockedCompareExchange(
                &receiptAttempted_, 1, 0) != 0) {
            return;
        }
        JsonWriter json{
            receiptBuffer_.data(), receiptBuffer_.size(), 0, true};
        BuildReceipt(json);
        if (!json.valid ||
            json.length > static_cast<std::size_t>(MAXDWORD)) {
            return;
        }

        HANDLE output = CreateFileW(
            receiptPath_.data(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        const bool complete =
            WriteFile(
                output, json.output, static_cast<DWORD>(json.length),
                &written, nullptr) != FALSE &&
            written == json.length &&
            FlushFileBuffers(output) != FALSE;
        CloseHandle(output);
        if (!complete) DeleteFileW(receiptPath_.data());
    }

    static void __fastcall ClearHook(void* self, void*) {
        NativeRecordLifecycleProbe* observer = active_;
        bool observing = false;
        if (observer && observer->appFinalActive_ &&
            observer->finalGrid_ ==
                reinterpret_cast<std::uintptr_t>(self) &&
            observer->appFinalThread_ == GetCurrentThreadId()) {
            observer->clearThread_ = GetCurrentThreadId();
            InterlockedIncrement(&observer->clearEntered_);
            InterlockedExchange(&observer->clearActive_, 1);
            observing = true;
        }

        clearGrid_(self);
        const DWORD error = GetLastError();
        if (observing) {
            InterlockedExchange(&observer->clearActive_, 0);
            InterlockedIncrement(&observer->clearReturned_);
        }
        SetLastError(error);
    }

    static void __fastcall AppFinalReleaseHook(void* self, void*) {
        NativeRecordLifecycleProbe* observer = active_;
        if (!observer) {
            appFinalRelease_(self);
            return;
        }

        observer->appFinalThread_ = GetCurrentThreadId();
        observer->finalGrid_ =
            reinterpret_cast<std::uintptr_t>(observer->ReadSingleton());
        InterlockedIncrement(&observer->appFinalEntered_);
        InterlockedExchange(&observer->appFinalActive_, 1);

        appFinalRelease_(self);
        const DWORD error = GetLastError();

        InterlockedExchange(&observer->appFinalActive_, 0);
        InterlockedIncrement(&observer->appFinalReturned_);
        InterlockedExchange(
            &observer->singletonNull_,
            observer->ReadSingleton() == nullptr ? 1 : 0);
        observer->WriteReceipt();
        SetLastError(error);
    }

    static void __fastcall InsertNodeHook(
        void* grid, void*, void* result, void* node, char flag) {
        insertNode_(grid, result, node, flag);
        const DWORD error = GetLastError();
        if (active_) active_->ObserveInserted(grid, result);
        SetLastError(error);
    }

    static void __fastcall RegisterRecordHook(
        void* grid, void*, const void* raw, std::uint32_t bytes,
        std::uint32_t kind) {
        registerRecord_(grid, raw, bytes, kind);
        const DWORD error = GetLastError();
        if (active_)
            active_->ObserveRegister(grid, raw, bytes, kind);
        SetLastError(error);
    }

    static void __cdecl DeleteHook(void* value) {
        delete_(value);
        const DWORD error = GetLastError();
        if (active_) active_->ObserveFreed(value);
        SetLastError(error);
    }

public:
    bool Install(
        const SdgoApi* api, SdgoMod* mod, const std::string& runId,
        const std::wstring& receiptPath,
        const std::vector<std::uint32_t>& exactIds) {
        if (!api || !api->Symbol || !api->HookCode ||
            active_ || runId.empty() ||
            runId.size() > kMaximumRunIdBytes ||
            receiptPath.empty() ||
            receiptPath.size() > kMaximumReceiptPathCharacters ||
            exactIds.empty() || exactIds.size() > kMaximumRecords) {
            return false;
        }

        for (std::size_t index = 0; index < exactIds.size(); ++index) {
            if (!exactIds[index]) return false;
            for (std::size_t previous = 0; previous < index; ++previous)
                if (exactIds[previous] == exactIds[index]) return false;
        }

        auto* mapFind =
            reinterpret_cast<MapFind>(api->Symbol(mod, "grid.map_find"));
        auto* singleton = reinterpret_cast<volatile LONG*>(
            api->Symbol(mod, "test.native_grid_singleton"));
        if (!mapFind || !singleton) return false;

        api_ = api;
        mod_ = mod;
        recordCount_ = exactIds.size();
        std::memcpy(runId_.data(), runId.data(), runId.size());
        runId_[runId.size()] = '\0';
        std::memcpy(
            receiptPath_.data(), receiptPath.data(),
            receiptPath.size() * sizeof(wchar_t));
        receiptPath_[receiptPath.size()] = L'\0';
        for (std::size_t index = 0; index < recordCount_; ++index)
            records_[index].expectedId = exactIds[index];

        mapFind_ = mapFind;
        gridSingleton_ = singleton;
        active_ = this;
        if (!api->HookCode(
                mod, "test.native_grid_clear",
                reinterpret_cast<void*>(&ClearHook),
                reinterpret_cast<void**>(&clearGrid_)) ||
            !api->HookCode(
                mod, "test.native_app_final_release",
                reinterpret_cast<void*>(&AppFinalReleaseHook),
                reinterpret_cast<void**>(&appFinalRelease_)) ||
            !api->HookCode(
                mod, "grid.insert_node",
                reinterpret_cast<void*>(&InsertNodeHook),
                reinterpret_cast<void**>(&insertNode_)) ||
            !api->HookCode(
                mod, "grid.register_record",
                reinterpret_cast<void*>(&RegisterRecordHook),
                reinterpret_cast<void**>(&registerRecord_)) ||
            !api->HookCode(
                mod, "crt.operator_delete",
                reinterpret_cast<void*>(&DeleteHook),
                reinterpret_cast<void**>(&delete_))) {
            active_ = nullptr;
            return false;
        }
        return true;
    }
};
