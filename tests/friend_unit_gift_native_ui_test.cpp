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
std::map<void*,std::map<int,std::wstring>> g_text_layers;
std::map<void*,std::wstring> g_static_captions;
std::map<void*, std::size_t> g_skin_calls;
std::map<
    void*,
    std::array<std::array<std::uint8_t, 0x50>, 16>>
    g_added_skin_entries;
std::map<void*, std::array<std::uint8_t*, 16>>
    g_added_skin_pointers;
void* g_button_vtable = reinterpret_cast<void*>(0x12345678);
void* g_static_vtable = reinterpret_cast<void*>(0x1234ABCD);
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
    Put<std::uint32_t>(control->bytes.data(),0x134,4);
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
    int index,
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
    g_text_layers[control][index]=NativeText(value);
}

void __fastcall StaticCaptionShim(void* control,void*,const NativeString* value) {
    g_static_captions[control]=NativeText(value);
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
        NativeString path;
        std::memcpy(path.data.small,L"skin",5*sizeof(wchar_t));
        path.length = 4;
        Put<NativeString>(entries[index].data(),4,path);
    }
    Put<std::uint8_t**>(
        source->bytes.data(), 0x1A4, pointers.data());
    Put<std::uint8_t**>(
        source->bytes.data(), 0x1A8,
        pointers.data() + pointers.size());
}

void __fastcall EmptySkinShim(void* control,void*) {
    auto& count = g_skin_calls[control];
    if (count >= 16) return;
    auto& entry = g_added_skin_entries[control][count];
    entry.fill(0);
    Put<std::uint32_t>(entry.data(),0x18,7);
    auto& pointers = g_added_skin_pointers[control];
    pointers[count] = entry.data();
    ++count;
    Put<std::uint8_t**>(control,0x1A4,pointers.data());
    Put<std::uint8_t**>(control,0x1A8,pointers.data()+count);
}

unsigned g_geometry_updates = 0;
unsigned g_native_placements = 0;
void __fastcall PlacePopupShim(void*,void*,std::uint32_t) { ++g_native_placements; }
void __fastcall GeometryShim(void* control,void*) {
    ++g_geometry_updates;
    const int x=static_cast<int>(Read<float>(control,0x148));
    const int y=static_cast<int>(Read<float>(control,0x14C));
    const RECT rect{x,y,x+Read<int>(control,0x154),y+Read<int>(control,0x158)};
    Put<RECT>(control,0x138,rect);
}
std::uint32_t __fastcall AddIconShim(void* dialog,void*,const NativeString* key,
                                    int x,int y,int width,int height) {
    const auto result=AddShim(dialog,nullptr,key,x,y,width,height,0);
    auto* control=g_controls[NativeText(key)]->bytes.data();
    Put<std::uint8_t>(control,0x168,1); // native constructor's debug-rectangle default
    return result;
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
    bindings.empty_skin = reinterpret_cast<Bindings::EmptySkin>(&EmptySkinShim);
    bindings.refresh_geometry = reinterpret_cast<Bindings::RefreshGeometry>(&GeometryShim);
    bindings.add_icon = reinterpret_cast<Bindings::AddIcon>(&AddIconShim);
    bindings.place_popup = reinterpret_cast<Bindings::PlacePopup>(&PlacePopupShim);
    bindings.button_vtable = g_button_vtable;
    bindings.static_vtable=g_static_vtable;
    bindings.static_caption=reinterpret_cast<Bindings::Caption>(&StaticCaptionShim);
    bindings.unit_name=[](std::uint32_t)->std::wstring{return L"测试机体";};
    Control radio;
    Put<std::uint32_t>(radio.bytes.data(),0x134,12);
    std::array<std::array<std::uint8_t,0x7C>,3> radioItems{};
    std::array<std::uint8_t*,3> radioText{};
    for (std::size_t i=0;i<radioText.size();++i) radioText[i]=radioItems[i].data();
    Put<void*>(radio.bytes.data(),0x190,radioText.data());
    Put<void*>(radio.bytes.data(),0x194,radioText.data()+radioText.size());
    bindings.SetText(radio.bytes.data(),L"收到的机体");
    for (int i=0;i<3;++i)
        Check(g_text_layers[radio.bytes.data()][i]==L"收到的机体",
              "native radio caption remains readable in every selected and hover state");
    Control rendered;
    Put<void*>(rendered.bytes.data(),0,g_button_vtable);
    const std::wstring renderedText=L"发送方：好友\n赠送时间：2026-11-03 00:01 UTC\n寄语：出击顺利！";
    NativeString renderedString;
    renderedString.data.pointer=renderedText.c_str();
    renderedString.length=static_cast<std::uint32_t>(renderedText.size());
    renderedString.capacity=renderedString.length;
    std::array<NativeString*,1> textItems{&renderedString};
    Put<void*>(rendered.bytes.data(),0x190,textItems.data());
    Put<void*>(rendered.bytes.data(),0x194,textItems.data()+1);
    std::wstring readText;
    Check(bindings.ReadText(rendered.bytes.data(),&readText) && readText==renderedText,
          "provenance observer reads the exact native control text, including line breaks and time");
    Put<void*>(rendered.bytes.data(),0x194,textItems.data());
    Check(!bindings.ReadText(rendered.bytes.data(),&readText),"missing native text cannot be replaced by model output");

    Control staticReadback;
    Put<void*>(staticReadback.bytes.data(),0,g_static_vtable);
    NativeString staticValue{};
    const std::wstring senderText=L"赠送者：测试玩家";
    staticValue.data.pointer=senderText.c_str();
    staticValue.length=static_cast<std::uint32_t>(senderText.size());
    staticValue.capacity=staticValue.length;
    Put<NativeString>(staticReadback.bytes.data(),0x21C,staticValue);
    Check(bindings.ReadText(staticReadback.bytes.data(),&readText) && readText==senderText,
          "native card provenance reads the displayed static sender caption");
    staticValue.capacity=1;
    Put<NativeString>(staticReadback.bytes.data(),0x21C,staticValue);
    Check(!bindings.ReadText(staticReadback.bytes.data(),&readText),
          "invalid native static string storage is rejected");

    Control layered;
    std::array<std::array<std::uint8_t,0x80>,2> layerRecords{};
    std::array<std::uint8_t*,2> layerPointers{layerRecords[0].data(),layerRecords[1].data()};
    Put<void*>(layered.bytes.data(),0x190,layerPointers.data());
    Put<void*>(layered.bytes.data(),0x194,layerPointers.data()+2);
    Put<int>(layered.bytes.data(),0x154,109);Put<int>(layered.bytes.data(),0x158,12);
    g_text_layers[layered.bytes.data()][1]=L"9999999999";
    bindings.SetText(layered.bytes.data(),L"Lv.12");
    Check(g_text_layers[layered.bytes.data()][0]==L"Lv.12" &&
              g_text_layers[layered.bytes.data()][1].empty(),
          "replacing a stock caption clears every leftover placeholder text layer");

    Control staticLabel;
    Put<void*>(staticLabel.bytes.data(),0,g_static_vtable);
    Put<std::uint32_t>(staticLabel.bytes.data(),0x134,3);
    Put<std::uint32_t>(staticLabel.bytes.data(),0x21C,0x12345678);
    g_static_captions[staticLabel.bytes.data()]=L"9999999999";
    bindings.SetState(staticLabel.bytes.data(),false,false);
    Check(Read<std::uint32_t>(staticLabel.bytes.data(),0x21C)==0x12345678,
          "hiding a static control never corrupts its owned string pointer");
    bindings.SetState(staticLabel.bytes.data(),true,false);
    Check(Read<std::uint8_t>(staticLabel.bytes.data(),0x162)==1 &&
          Read<std::uint8_t>(staticLabel.bytes.data(),0x163)==1 &&
          Read<std::uint8_t>(staticLabel.bytes.data(),0x167)==0 &&
          Read<std::uint32_t>(staticLabel.bytes.data(),0x21C)==0x12345678,
          "read-only static text remains drawable without accepting input or changing string ownership");
    bindings.SetText(staticLabel.bytes.data(),L"Lv.12");
    Check(g_static_captions[staticLabel.bytes.data()]==L"Lv.12" && g_text_layers[staticLabel.bytes.data()].empty(),
          "native static caption replacement removes the placeholder instead of adding an overlay");
    Control comboState;
    Put<std::uint32_t>(comboState.bytes.data(),0x134,11);
    Put<std::int32_t>(comboState.bytes.data(),0x21C,7);
    bindings.SetState(comboState.bytes.data(),true,false);
    Check(Read<std::int32_t>(comboState.bytes.data(),0x21C)==7,
          "temporarily disabling a combo does not reset its selected item");

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
                friendDialog.bytes.data(), 0x148) == 750.0f &&
            Read<float>(
                friendDialog.bytes.data(), 0x14C) == 500.0f && g_native_placements==1,
        "visible gift entry expands and delegates cursor positioning to the original popup");
    Put<float>(friendDialog.bytes.data(),0x148,320.0f);
    Put<float>(friendDialog.bytes.data(),0x14C,240.0f);
    friendEntry.SetAvailable(true);
    Check(Read<float>(friendDialog.bytes.data(),0x148)==320.0f &&
              Read<float>(friendDialog.bytes.data(),0x14C)==240.0f && g_native_placements==1,
          "later availability ticks cannot reset the native right-click position");
    friendEntry.SetAvailable(false);
    Check(
        Read<int>(friendDialog.bytes.data(), 0x158) == 125 &&
            Read<float>(
                friendDialog.bytes.data(), 0x148) == 320.0f &&
            Read<float>(
                friendDialog.bytes.data(), 0x14C) == 240.0f,
        "hiding the gift entry restores height without moving the native popup");
    friendEntry.Detach(friendService.bytes.data());

    // Native 0572 context menus have a deliberately blank normal skin.
    Control blankDialog;
    Control blankService;
    Put<void*>(blankService.bytes.data(),0x4C,blankDialog.bytes.data());
    Put<int>(blankDialog.bytes.data(),0x154,126);
    Put<int>(blankDialog.bytes.data(),0x158,132);
    auto* blankSource = NewControl(L"Btn5",blankDialog.bytes.data());
    Put<float>(blankSource->bytes.data(),0x14C,105.0f);
    Put<int>(blankSource->bytes.data(),0x154,126);
    Put<int>(blankSource->bytes.data(),0x158,21);
    auto* bottom = NewControl(L"Icon1",blankDialog.bytes.data());
    Put<float>(bottom->bytes.data(),0x14C,127.0f);
    Put<int>(bottom->bytes.data(),0x158,6);
    auto* strip = NewControl(L"Icon8",blankDialog.bytes.data());
    PrepareSkinSource(strip);
    auto** stripSkins = Read<std::uint8_t**>(strip->bytes.data(),0x1A4);
    std::array<std::uint8_t,0x50> stripEntry{};
    std::memcpy(stripEntry.data(),*stripSkins,stripEntry.size());
    std::array<std::uint8_t*,1> stripPointer{stripEntry.data()};
    Put<std::uint8_t**>(strip->bytes.data(),0x1A4,stripPointer.data());
    Put<std::uint8_t**>(strip->bytes.data(),0x1A8,stripPointer.data()+1);
    PrepareSkinSource(blankSource);
    auto** blankSkins = Read<std::uint8_t**>(blankSource->bytes.data(),0x1A4);
    std::memset(blankSkins[0],0,0x50);
    Put<std::uint32_t>(blankSkins[0],0x18,7);
    FriendEntry transparentEntry;
    Check(transparentEntry.Attach(blankService.bytes.data(),bindings),
          "stock transparent normal skin is preserved through the native blank appender");
    auto** clonedSkins = Read<std::uint8_t**>(transparentEntry.button(),0x1A4);
    Check(g_skin_calls[transparentEntry.button()] == 3 &&
              !Read<void*>(clonedSkins[0]) && Read<void*>(clonedSkins[1]) &&
              Read<void*>(clonedSkins[2]),
          "normal, hover and pressed slots keep native ordering including the blank slot");
    transparentEntry.SetAvailable(true);
    Check(transparentEntry.Click(blankService.bytes.data(),transparentEntry.button()),
          "gift entry with a transparent normal state remains interactive");
    Check(Read<float>(bottom->bytes.data(),0x14C)==149.0f && g_geometry_updates>0 &&
              Read<RECT>(blankDialog.bytes.data(),0x138).bottom==155 &&
              g_controls.count(L"ModFriendUnitGiftBackground") &&
              Read<std::uint8_t>(g_controls[L"ModFriendUnitGiftBackground"]->bytes.data(),0x167)==0 &&
              Read<std::uint8_t>(g_controls[L"ModFriendUnitGiftBackground"]->bytes.data(),0x168)==0,
          "native background strip, lower border and hit rectangle follow the added row");
    transparentEntry.Detach(blankService.bytes.data());
    Check(Read<float>(bottom->bytes.data(),0x14C)==127.0f,
          "detaching restores the original lower border");

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
                detailView.detail(), 0x163) == 1 &&
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

    std::vector<Control> stock(47);
    std::size_t stockIndex = 0;
    const auto installStock = [&](std::size_t offset) {
        auto& control = stock[stockIndex++];
        Put<void*>(control.bytes.data(),0,g_button_vtable);
        Put<std::uint32_t>(control.bytes.data(),0x134,4);
        Put<void*>(control.bytes.data(),0x15C,dialog.bytes.data());
        Put<std::uint8_t>(control.bytes.data(),0x162,1);
        Put<std::uint8_t>(control.bytes.data(),0x163,1);
        Put<std::uint8_t>(control.bytes.data(),0x167,1);
        Put<void*>(service.bytes.data(),offset,control.bytes.data());
    };
    for (const auto offset : {
             0x94u, 0x98u, 0xA4u, 0xA8u, 0xACu, 0xB0u, 0xB4u}) {
        installStock(offset);
    }
    for (std::size_t row = 0; row < 5; ++row) {
        for (std::size_t field = 0; field < 8; ++field) {
            installStock(0xBC + row * 0x20 + field * 4);
            auto* native=Read<void*>(service.bytes.data(),0xBC+row*0x20+field*4);
            const std::uint32_t kinds[]={12,1,3,3,3,4,4,1};
            Put<std::uint32_t>(native,0x134,kinds[field]);
            if (kinds[field]==3) Put<void*>(native,0,g_static_vtable);
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
    rows[0].level = 12;
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
    Check(view.ControlFor(view.RowActionForGift(100))==Read<void*>(service.bytes.data(),0xBC),
          "received gifts reuse the actual native Storage row rather than an overlay button");
    for (std::size_t row=2;row<5;++row) for (std::size_t field=0;field<8;++field) {
        const auto* control=Read<void*>(service.bytes.data(),0xBC+row*0x20+field*4);
        Check(Read<std::uint8_t>(control,0x162)==0 && Read<std::uint8_t>(control,0x167)==0,
              "unused native rows are hidden and cannot route actions");
    }
    const auto first =
        view.RowActionForGift(100);
    const auto caption=g_static_captions[Read<void*>(service.bytes.data(),0xC4)];
    Check(caption.find(L"Lv.12 · 测试机体")!=std::wstring::npos &&
              caption.find(L"UID")==std::wstring::npos && caption.find(L"#572")==std::wstring::npos,
          "player gift rows show the concrete name and level without internal identifiers");
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
    Check(Read<std::uint8_t>(view.ControlFor(first),0x162)==1,
          "post-refresh enforcement retains the original visible gift row");
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
