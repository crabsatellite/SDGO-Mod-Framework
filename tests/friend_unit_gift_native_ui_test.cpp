#include "../mods/friend_unit_gift_client/native_ui.h"

#include <windows.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace sdgo::friend_unit_gift;
using namespace sdgo::friend_unit_gift::client::ui;

static_assert(sizeof(void*) == 4, "native UI fixture requires x86");

struct Control {
    std::array<std::uint8_t, 0x300> bytes{};
};

template <class T>
T Read(const void* pointer, std::size_t offset = 0) {
    T value{};
    std::memcpy(
        &value,
        static_cast<const std::uint8_t*>(pointer) + offset,
        sizeof(value));
    return value;
}

template <class T>
void Put(void* pointer, std::size_t offset, T value) {
    std::memcpy(
        static_cast<std::uint8_t*>(pointer) + offset,
        &value,
        sizeof(value));
}

void Check(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
}

std::wstring NativeText(const NativeString* value) {
    if (!value) return {};
    const wchar_t* text =
        value->capacity < 8
            ? value->data.small
            : value->data.pointer;
    return text ? std::wstring(text, value->length) : std::wstring();
}

std::map<std::wstring, std::unique_ptr<Control>> g_controls;
std::map<void*, std::wstring> g_captions;
std::map<void*, std::size_t> g_skin_calls;
std::map<
    void*,
    std::array<std::array<std::uint8_t, 0x50>, 16>>
    g_added_skin_entries;
std::map<void*, std::array<std::uint8_t*, 16>>
    g_added_skin_pointers;
void* g_button_vtable = reinterpret_cast<void*>(0x12345678);
void* g_input_seen = nullptr;
std::uint32_t g_input_units = 0;
int g_input_clear = -1;
unsigned g_input_limit_calls = 0;

void __fastcall InputLimitShim(
    void* input, void*, std::uint32_t units, int clear) {
    g_input_seen = input;
    g_input_units = units;
    g_input_clear = clear;
    ++g_input_limit_calls;
}

Control* NewControl(
    const std::wstring& name,
    void* parent,
    bool visible = true,
    bool enabled = true) {
    auto control = std::make_unique<Control>();
    Put<void*>(control->bytes.data(), 0, g_button_vtable);
    Put<void*>(control->bytes.data(), 0x15C, parent);
    Put<std::uint8_t>(
        control->bytes.data(), 0x162, visible ? 1 : 0);
    Put<std::uint8_t>(
        control->bytes.data(), 0x163, enabled ? 1 : 0);
    Put<std::uint8_t>(
        control->bytes.data(), 0x167, enabled ? 1 : 0);
    auto* result = control.get();
    g_controls[name] = std::move(control);
    return result;
}

std::uint32_t __fastcall AddShim(
    void* dialog,
    void*,
    const NativeString* name,
    int x,
    int y,
    int width,
    int height,
    std::uint32_t) {
    const auto decoded = NativeText(name);
    auto* control =
        NewControl(decoded, dialog, false, false);
    Put<float>(
        control->bytes.data(), 0x148,
        static_cast<float>(x));
    Put<float>(
        control->bytes.data(), 0x14C,
        static_cast<float>(y));
    Put<int>(control->bytes.data(), 0x154, width);
    Put<int>(control->bytes.data(), 0x158, height);
    return 1;
}

void* __fastcall FindShim(
    void* dialog,
    void*,
    const NativeString* name,
    int) {
    const auto decoded = NativeText(name);
    const auto found = g_controls.find(decoded);
    if (found == g_controls.end()) return nullptr;
    auto* control = found->second->bytes.data();
    return Read<void*>(control, 0x15C) == dialog
               ? control
               : nullptr;
}

void __fastcall TextShim(
    void* control,
    void*,
    int,
    const NativeString* value,
    int,
    int,
    int,
    int,
    std::uint32_t,
    std::uint32_t,
    std::uint8_t,
    std::uint32_t,
    int) {
    g_captions[control] = NativeText(value);
}

void __fastcall SkinShim(
    void* control,
    void*,
    const NativeString*,
    std::uint8_t,
    std::uint32_t,
    std::uint32_t) {
    auto& count = g_skin_calls[control];
    if (count >= 16) return;
    auto& entry = g_added_skin_entries[control][count];
    Put<void*>(
        entry.data(),
        0,
        reinterpret_cast<void*>(0x2000 + count));
    auto& pointers = g_added_skin_pointers[control];
    pointers[count] = entry.data();
    ++count;
    Put<std::uint8_t**>(
        control, 0x1A4, pointers.data());
    Put<std::uint8_t**>(
        control, 0x1A8, pointers.data() + count);
}

void PrepareSkinSource(Control* source) {
    static std::array<std::array<std::uint8_t, 0x50>, 3> entries{};
    static std::array<std::uint8_t*, 3> pointers{};
    for (std::size_t index = 0; index < entries.size(); ++index) {
        Put<void*>(
            entries[index].data(),
            0,
            reinterpret_cast<void*>(0x1000 + index));
        pointers[index] = entries[index].data();
    }
    Put<std::uint8_t**>(
        source->bytes.data(), 0x1A4, pointers.data());
    Put<std::uint8_t**>(
        source->bytes.data(), 0x1A8,
        pointers.data() + pointers.size());
}

} // namespace

int main() {
    Bindings bindings;
    bindings.add =
        reinterpret_cast<Bindings::Add>(&AddShim);
    bindings.find =
        reinterpret_cast<Bindings::Find>(&FindShim);
    bindings.text =
        reinterpret_cast<Bindings::Text>(&TextShim);
    bindings.skin =
        reinterpret_cast<Bindings::Skin>(&SkinShim);
    bindings.button_vtable = g_button_vtable;

    Control noteEdit;
    Control nativeInput;
    void* input = nativeInput.bytes.data();
    CRITICAL_SECTION inputLock;
    InitializeCriticalSection(&inputLock);
    bindings.edit_vtable = reinterpret_cast<void*>(0x23456789);
    bindings.input_limit =
        reinterpret_cast<Bindings::InputLimit>(&InputLimitShim);
    bindings.input_context = &input;
    bindings.input_lock = &inputLock;
    Put<void*>(noteEdit.bytes.data(), 0, bindings.edit_vtable);
    Check(bindings.SetNoteLimit(noteEdit.bytes.data()) &&
              Read<std::uint32_t>(noteEdit.bytes.data(), 0x2A0) == 400 &&
              g_input_limit_calls == 0,
          "unfocused note supports 200 UTF-16 units without changing another input");
    Put<std::uint8_t>(noteEdit.bytes.data(), 0x165, 1);
    Check(bindings.SetNoteLimit(noteEdit.bytes.data()) &&
              g_input_limit_calls == 1 && g_input_seen == input &&
              g_input_units == 200 && g_input_clear == 0,
          "focused note refreshes the real input limit without clearing text or caret");
    input = nullptr;
    Check(!bindings.SetNoteLimit(noteEdit.bytes.data()) &&
              g_input_limit_calls == 1,
          "focused note fails closed when the native input is unavailable");
    input = nativeInput.bytes.data();
    Put<void*>(noteEdit.bytes.data(), 0, g_button_vtable);
    Check(!bindings.SetNoteLimit(noteEdit.bytes.data()),
          "note length writes require the exact original edit class");
    DeleteCriticalSection(&inputLock);

    Control viewport;
    Put<int>(viewport.bytes.data(), 0x154, 800);
    Put<int>(viewport.bytes.data(), 0x158, 600);
    Control friendService;
    Control friendDialog;
    Put<void*>(
        friendService.bytes.data(),
        0x4C,
        friendDialog.bytes.data());
    Put<void*>(
        friendDialog.bytes.data(),
        0x15C,
        viewport.bytes.data());
    Put<float>(friendDialog.bytes.data(), 0x148, 750.0f);
    Put<float>(friendDialog.bytes.data(), 0x14C, 500.0f);
    Put<int>(friendDialog.bytes.data(), 0x154, 120);
    Put<int>(friendDialog.bytes.data(), 0x158, 125);
    auto* friendSource =
        NewControl(L"Btn5", friendDialog.bytes.data());
    Put<float>(
        friendSource->bytes.data(), 0x14C, 100.0f);
    Put<int>(friendSource->bytes.data(), 0x154, 120);
    Put<int>(friendSource->bytes.data(), 0x158, 24);
    PrepareSkinSource(friendSource);
    NewControl(
        L"SkinlessExisting",
        friendDialog.bytes.data());
    Check(
        bindings.AddButton(
            friendDialog.bytes.data(),
            L"SkinlessExisting",
            0,
            0,
            120,
            24,
            friendSource->bytes.data(),
            L"失效") == nullptr,
        "a pre-existing skinless control cannot bypass "
        "native mouse-state validation");

    FriendEntry friendEntry;
    Check(
        friendEntry.Attach(
            friendService.bytes.data(), bindings),
        "friend gift entry attaches to the native context");
    Check(
        Read<int>(friendDialog.bytes.data(), 0x158) == 125 &&
            Read<float>(
                friendDialog.bytes.data(), 0x148) == 750.0f &&
            Read<float>(
                friendDialog.bytes.data(), 0x14C) == 500.0f,
        "unsupported gift capability leaves menu geometry inert");
    friendEntry.SetAvailable(true);
    Check(
        Read<int>(friendDialog.bytes.data(), 0x158) == 149 &&
            Read<float>(
                friendDialog.bytes.data(), 0x148) == 680.0f &&
            Read<float>(
                friendDialog.bytes.data(), 0x14C) == 451.0f,
        "visible gift entry expands and clamps inside the viewport");
    friendEntry.SetAvailable(false);
    Check(
        Read<int>(friendDialog.bytes.data(), 0x158) == 125 &&
            Read<float>(
                friendDialog.bytes.data(), 0x148) == 750.0f &&
            Read<float>(
                friendDialog.bytes.data(), 0x14C) == 500.0f,
        "hiding the gift entry restores exact menu geometry");
    friendEntry.Detach(friendService.bytes.data());

    Control detailService;
    Control detailRoot;
    Control detailRender;
    Put<void*>(
        detailRender.bytes.data(),
        0x15C,
        detailRoot.bytes.data());
    Put<std::uint8_t>(
        detailRender.bytes.data(), 0x162, 1);
    Put<std::uint8_t>(
        detailRender.bytes.data(), 0x163, 1);
    Put<std::uint8_t>(
        detailRender.bytes.data(), 0x167, 1);
    Put<void*>(
        detailService.bytes.data(),
        0x4C,
        detailRender.bytes.data());
    auto* detailSkin =
        NewControl(L"UnitDeleteBtn", detailRoot.bytes.data());
    PrepareSkinSource(detailSkin);
    UnitDetailView detailView;
    Check(
        detailView.Attach(
            detailService.bytes.data(),
            detailRoot.bytes.data(),
            detailSkin->bytes.data(),
            bindings),
        "unit detail provenance entry attaches without loose resources");
    Check(
        g_skin_calls[detailView.button()] == 3,
        "unit detail provenance entry clones three stock button skins "
        "for native mouse input");
    detailView.SetUnit(
        0x1122334455667788ull,
        true,
        true,
        L"查看赠送来源");
    Check(
        detailView.ready() &&
            Read<float>(
                detailView.button(), 0x148) == 118.0f &&
            Read<float>(
                detailView.button(), 0x14C) == 268.0f &&
            Read<int>(detailView.button(), 0x154) == 284 &&
            Read<int>(detailView.button(), 0x158) == 15 &&
            Read<float>(detailView.detail(), 0x148) == 118.0f &&
            Read<float>(detailView.detail(), 0x14C) == 41.0f &&
            Read<int>(detailView.detail(), 0x154) == 284 &&
            Read<int>(detailView.detail(), 0x158) == 227,
        "provenance entry and detail layer use native 1334 geometry");
    Check(
        detailView.Click(
            detailService.bytes.data(),
            detailView.button()),
        "provenance entry routes through its product control");
    detailView.SetUnit(
        0x1122334455667788ull,
        true,
        false,
        L"正在读取赠送来源…");
    Check(
        Read<std::uint8_t>(
            detailView.button(), 0x162) == 1 &&
            Read<std::uint8_t>(
                detailView.button(), 0x167) == 0,
        "provenance entry remains visible and disabled while querying");
    const std::wstring fullDetail =
        L"发送方：好友\n赠送时间：2026-11-03 00:00 UTC"
        L"\n接收时间：2026-11-03 00:01 UTC\n\n留言：\n"
        L"第一行\n第二行 Unicode 好友";
    detailView.SetDetail(fullDetail, true);
    Check(
        detailView.detail_visible() &&
            g_captions[detailView.detail()] == fullDetail &&
            Read<std::uint8_t>(
                detailView.detail(), 0x162) == 1 &&
            Read<std::uint8_t>(
                detailView.detail(), 0x163) == 0 &&
            Read<std::uint8_t>(
                detailView.detail(), 0x167) == 0 &&
            Read<std::uint8_t>(
                detailRender.bytes.data(), 0x162) == 0 &&
            Read<std::uint8_t>(
                detailRender.bytes.data(), 0x163) == 0 &&
            Read<std::uint8_t>(
                detailRender.bytes.data(), 0x167) == 0,
        "full Unicode provenance replaces only the native 3D viewport");
    detailView.ToggleDetail();
    Check(
        !detailView.detail_visible() &&
            Read<std::uint8_t>(
                detailView.detail(), 0x162) == 0 &&
            Read<std::uint8_t>(
                detailRender.bytes.data(), 0x162) == 1 &&
            Read<std::uint8_t>(
                detailRender.bytes.data(), 0x163) == 1 &&
            Read<std::uint8_t>(
                detailRender.bytes.data(), 0x167) == 1,
        "closing provenance restores the exact 3D viewport state");
    detailView.SetDetail(fullDetail, true);
    detailView.SetUnit(
        0x1122334455667788ull,
        false,
        false,
        L"查看赠送来源");
    Check(
        !detailView.detail_visible() &&
            Read<std::uint8_t>(
                detailRender.bytes.data(), 0x162) == 1 &&
            Read<std::uint8_t>(
                detailRender.bytes.data(), 0x163) == 1 &&
            Read<std::uint8_t>(
                detailRender.bytes.data(), 0x167) == 1,
        "capability loss closes provenance and restores 3D");
    detailView.SetUnit(
        0x1122334455667788ull,
        true,
        true,
        L"展开赠送来源");
    detailView.SetDetail(fullDetail, true);
    detailView.Detach(detailService.bytes.data());
    Check(
        Read<std::uint8_t>(
            detailRender.bytes.data(), 0x162) == 1 &&
            Read<std::uint8_t>(
                detailRender.bytes.data(), 0x163) == 1 &&
            Read<std::uint8_t>(
                detailRender.bytes.data(), 0x167) == 1,
        "detaching provenance restores the 3D viewport");

    Control service;
    Control dialog;
    Put<void*>(
        service.bytes.data(), 0x70, dialog.bytes.data());

    auto* source = NewControl(
        L"StockButton", dialog.bytes.data());
    PrepareSkinSource(source);
    Put<void*>(
        service.bytes.data(), 0xB8, source->bytes.data());

    auto* claim = NewControl(
        L"ModStorageClaimAll", dialog.bytes.data());
    Put<std::uint8_t>(claim->bytes.data(), 0x163, 0);
    Put<std::uint8_t>(claim->bytes.data(), 0x167, 0);

    std::vector<Control> stock(46);
    std::size_t stockIndex = 0;
    const auto installStock = [&](std::size_t offset) {
        auto& control = stock[stockIndex++];
        Put<std::uint8_t>(control.bytes.data(), 0x162, 1);
        Put<std::uint8_t>(control.bytes.data(), 0x163, 1);
        Put<std::uint8_t>(control.bytes.data(), 0x167, 1);
        Put<void*>(
            service.bytes.data(), offset, control.bytes.data());
    };
    for (const auto offset : {
             0x94u, 0x98u, 0xA4u, 0xA8u, 0xACu, 0xB0u}) {
        installStock(offset);
    }
    for (std::size_t row = 0; row < 5; ++row) {
        for (std::size_t field = 0; field < 8; ++field) {
            installStock(0xBC + row * 0x20 + field * 4);
        }
    }

    StorageView view;
    const bool storageAttached = view.Attach(
        service.bytes.data(), dialog.bytes.data(), bindings);
    if (!storageAttached) {
        std::wcerr << L"storage controls after attach:";
        for (const auto& entry : g_controls) {
            std::wcerr << L" [" << entry.first << L"]";
        }
        std::wcerr << L"\n";
    }
    Check(
        storageAttached,
        "storage companion attaches to the native page");
    view.SetAvailable(true);
    Check(view.ready(), "storage entry is ready");

    GiftSummary rows[2]{};
    rows[0].gift_id = 100;
    rows[0].unit_uid = 1000;
    rows[0].unit_number = 572;
    rows[0].state = GiftState::pending;
    rows[0].direction = ListDirection::inbox;
    rows[0].peer_name_code_units = 2;
    rows[0].peer_name[0] = u'好';
    rows[0].peer_name[1] = u'友';
    rows[1].gift_id = 200;
    rows[1].unit_uid = 2000;
    rows[1].unit_number = 777;
    rows[1].state = GiftState::returned;
    rows[1].direction = ListDirection::returned;

    view.ShowStatus(L"custom", true);
    view.RenderRows(rows, 2, 1, 2, 100, true, false);
    Check(
        Read<std::uint8_t>(claim->bytes.data(), 0x162) == 0,
        "native Claim All is hidden only in the custom view");
    for (const auto& control : stock) {
        Check(
            Read<std::uint8_t>(control.bytes.data(), 0x162) == 0 &&
                Read<std::uint8_t>(control.bytes.data(), 0x167) == 0,
            "stock gift rows are hidden without mutating their data");
    }
    const auto first =
        view.RowActionForGift(100);
    Check(
        first != StorageView::Action::none &&
            view.GiftFor(first) == 100,
        "custom row retains its exact gift identity");
    Check(
        view.ActionFor(
            service.bytes.data(), view.ControlFor(first)) == first,
        "row click routes through the product control");
    Check(
        view.ActionFor(
            service.bytes.data(),
            view.ControlFor(StorageView::Action::accept)) ==
            StorageView::Action::accept,
        "selected inbox row enables the accept control");
    Put<std::uint8_t>(
        view.ControlFor(StorageView::Action::accept),
        0x167,
        0);
    Check(
        view.ActionFor(
            service.bytes.data(),
            view.ControlFor(StorageView::Action::accept)) ==
            StorageView::Action::none,
        "disabled product controls cannot route native actions");
    Put<std::uint8_t>(
        view.ControlFor(StorageView::Action::accept),
        0x167,
        1);

    for (auto& control : stock) {
        Put<std::uint8_t>(control.bytes.data(), 0x162, 1);
        Put<std::uint8_t>(control.bytes.data(), 0x163, 1);
        Put<std::uint8_t>(control.bytes.data(), 0x167, 1);
    }
    Put<std::uint8_t>(claim->bytes.data(), 0x162, 1);
    view.EnforceCustomView();
    for (const auto& control : stock) {
        Check(
            Read<std::uint8_t>(control.bytes.data(), 0x162) == 0 &&
                Read<std::uint8_t>(control.bytes.data(), 0x167) == 0,
            "post-refresh enforcement hides stock rows again");
    }
    Check(
        Read<std::uint8_t>(claim->bytes.data(), 0x162) == 0,
        "post-refresh enforcement keeps Claim All hidden");

    view.SetAvailable(false);
    Check(
        Read<std::uint8_t>(claim->bytes.data(), 0x162) == 1 &&
            Read<std::uint8_t>(claim->bytes.data(), 0x163) == 0 &&
            Read<std::uint8_t>(claim->bytes.data(), 0x167) == 0,
        "capability loss restores native Claim All state exactly");
    for (const auto& control : stock) {
        Check(
            Read<std::uint8_t>(control.bytes.data(), 0x162) == 1 &&
                Read<std::uint8_t>(control.bytes.data(), 0x163) == 1 &&
                Read<std::uint8_t>(control.bytes.data(), 0x167) == 1,
            "capability loss restores stock state exactly");
    }
    Check(!view.ready(), "unsupported storage entry is hidden");

    view.Detach(service.bytes.data());
    Check(!view.attached(), "storage companion detaches cleanly");
    std::cout << "friend_unit_gift_native_ui_test: PASS\n";
    return 0;
}
