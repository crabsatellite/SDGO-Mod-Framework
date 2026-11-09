#include "../mods/friend_unit_gift_client/native_ui.h"
#include "../mods/friend_list_client_fix/community_menu.h"
#include "../mods/friend_unit_gift_client/card_layout.h"

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

    // Give native-owned fixture children separate pages so destruction can be
    // represented by inaccessible memory without protecting the fixture heap.
    static void* operator new(std::size_t size) {
        auto* result = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!result) throw std::bad_alloc();
        return result;
    }
    static void operator delete(void* pointer) noexcept {
        if (pointer) VirtualFree(pointer, 0, MEM_RELEASE);
    }
};

bool DetachRetiredStorage(StorageView* view, void* service) {
    __try {
        view->Detach(service);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool DetachRetiredUnitDetail(UnitDetailView* view, void* service) {
    __try {
        view->Detach(service);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

volatile LONG g_watch_retired_access = 0;
volatile LONG g_retired_access_faults = 0;

LONG CALLBACK CountRetiredAccessFault(
    EXCEPTION_POINTERS* exception) {
    if (InterlockedCompareExchange(
            &g_watch_retired_access, 0, 0) != 0 &&
        exception && exception->ExceptionRecord &&
        exception->ExceptionRecord->ExceptionCode ==
            EXCEPTION_ACCESS_VIOLATION) {
        InterlockedIncrement(&g_retired_access_faults);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

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
std::map<void*, std::array<std::array<std::uint8_t, 0x100>, 16>>
    g_added_sprites;
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
    auto& sprite = g_added_sprites[control][count];
    sprite.fill(0);
    Put<void*>(
        entry.data(),
        0,
        sprite.data());
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
    for (const bool received:{false,true}) {
        const auto card=InformationCardLayout({330,270,48,48},{525,270,48,48},
            {334,231,150,12},{134,421,167,2},received);
        Check(card.valid && card.panel.x==315 && card.panel.right()==588 &&
              card.primary.y==399 && card.primary.bottom()==430 && card.cancel.right()==card.panel.right(),
              "gift footer aligns inside the shipped information-card content border");
        Check(card.note.x>=card.panel.x+12 && card.note.right()<=card.panel.right()-12 &&
              card.note.y>=card.label.bottom()+4 && card.note.bottom()<=card.panel.bottom()-8 &&
              card.panel.bottom()+8==card.primary.y && card.primary.right()+10==card.cancel.x,
              "note caption, editable text and native buttons have distinct padded rectangles");
        Check(card.background.x==card.panel.x && card.background.right()==card.panel.right() &&
              card.background.y==card.panel.y && card.background.bottom()>=card.cancel.bottom()+8 &&
              card.background.bottom()<452,
              "one opaque native backing covers the old PARTS slots and every footer gap inside the stock frame");
    }
    Check(!InformationCardLayout({}, {}, {}, {},false).valid,
          "missing native anchors cannot position controls on an unknown canvas");
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

    // Home USER LIST has six named controls and +4C is a button, not a dialog.
    Control homeService,homeDialog;
    Put<int>(homeDialog.bytes.data(),0x154,126);
    Put<int>(homeDialog.bytes.data(),0x158,146);
    auto* homeFirst=NewControl(L"SubMenu01",homeDialog.bytes.data());
    auto* homeLast=NewControl(L"SubMenu06",homeDialog.bytes.data());
    Put<void*>(homeService.bytes.data(),0x4C,homeFirst->bytes.data());
    Put<float>(homeLast->bytes.data(),0x14C,124.0f);
    Put<int>(homeLast->bytes.data(),0x154,126);Put<int>(homeLast->bytes.data(),0x158,21);
    PrepareSkinSource(homeLast);
    auto* homeFrame=NewControl(L"BgIcon",homeDialog.bytes.data());
    Put<int>(homeFrame->bytes.data(),0x158,146);
    FriendEntry homeEntry;
    Check(homeEntry.AttachCommunity(homeService.bytes.data(),bindings),
          "home context resolves the native button parent instead of treating +4C as a dialog");
    homeEntry.SetAvailable(true);
    Check(homeEntry.ready() && Read<int>(homeDialog.bytes.data(),0x158)==168 &&
          Read<int>(homeFrame->bytes.data(),0x158)==168 &&
          Read<std::uint8_t>(homeEntry.button(),0x168)==0,
          "home gift row extends its existing blue frame, keeps bottom padding and disables native debug drawing");
    homeEntry.Detach(homeService.bytes.data());
    Check(Read<int>(homeFrame->bytes.data(),0x158)==146,
          "home context destruction restores native frame geometry");

    Control captionService;
    std::array<Control*,6> menuButtons{};
    std::array<std::array<std::uint8_t,0x80>,6> menuLayers{};
    std::array<std::uint8_t*,6> menuPointers{};
    for(unsigned i=0;i<6;++i) {
        menuButtons[i]=NewControl(L"HomeCaption"+std::to_wstring(i),homeDialog.bytes.data());
        menuPointers[i]=menuLayers[i].data();
        Put<void*>(menuButtons[i]->bytes.data(),0x190,&menuPointers[i]);
        Put<void*>(menuButtons[i]->bytes.data(),0x194,&menuPointers[i]+1);
        Put<void*>(captionService.bytes.data(),0x4C+i*4,menuButtons[i]->bytes.data());
    }
    Put<std::uint32_t>(captionService.bytes.data(),0x64,2);
    Put<std::uint32_t>(captionService.bytes.data(),0x8C,7);
    friend_list_client_fix::LocalizeCommunityMenu(captionService.bytes.data(),
        reinterpret_cast<friend_list_client_fix::MenuText>(&TextShim),false);
    Check(g_captions[menuButtons[0]->bytes.data()]==L"私聊" &&
          g_captions[menuButtons[2]->bytes.data()]==L"机体资料" &&
          g_captions[menuButtons[4]->bytes.data()]==L"删除好友" &&
          g_captions[menuButtons[5]->bytes.data()]==L"屏蔽",
          "home menu localizes all actions and preserves the accepted-friend delete action");
    Put<std::uint32_t>(captionService.bytes.data(),0x8C,2);
    friend_list_client_fix::LocalizeCommunityMenu(captionService.bytes.data(),
        reinterpret_cast<friend_list_client_fix::MenuText>(&TextShim),true);
    Check(g_captions[menuButtons[4]->bytes.data()]==L"添加好友" &&
          g_captions[menuButtons[5]->bytes.data()]==L"取消屏蔽",
          "pending friendship and existing block state retain their native meanings");

    Control detailService;
    Control detailRoot;
    Control detailRender;
    const auto prepareRender = [&](Control& render, Control& root, Control& owner) {
        Put<void*>(render.bytes.data(), 0x15C, root.bytes.data());
        Put<float>(render.bytes.data(), 0x148, 118.0f);
        Put<float>(render.bytes.data(), 0x14C, 41.0f);
        Put<int>(render.bytes.data(), 0x154, 284);
        Put<int>(render.bytes.data(), 0x158, 227);
        for (const std::size_t flag : {0x162u, 0x163u, 0x167u})
            Put<std::uint8_t>(render.bytes.data(), flag, 1);
        Put<void*>(owner.bytes.data(), 0x4C, render.bytes.data());
    };
    prepareRender(detailRender, detailRoot, detailService);
    const auto renderBefore = detailRender.bytes;
    UnitDetailView detailView;
    Check(
        detailView.Attach(
            detailService.bytes.data(),
            detailRoot.bytes.data(),
            bindings),
        "gift origin attaches to the native unit-information page");
    const auto rect = [](void* control) {
        return std::array<int, 4>{static_cast<int>(Read<float>(control, 0x148)),
            static_cast<int>(Read<float>(control, 0x14C)),
            Read<int>(control, 0x154), Read<int>(control, 0x158)};
    };
    Check(
        rect(detailView.title()) == std::array<int, 4>{126, 232, 186, 15} &&
            rect(detailView.dates()) == std::array<int, 4>{126, 249, 186, 15} &&
            rect(detailView.button()) == std::array<int, 4>{318, 248, 76, 17},
        "origin caption sits in the lower corners of the 1334 viewport like its rank caption");
    Check(
        g_skin_calls[detailView.button()] == 3,
        "the link keeps a transparent normal state and stock blue hover/pressed skins");
    const auto hidden = [&]() {
        for (void* control : {detailView.title(), detailView.dates(), detailView.button()})
            if (Read<std::uint8_t>(control, 0x162) || Read<std::uint8_t>(control, 0x167)) return false;
        return true;
    };
    Check(hidden() && !detailView.ready() && detailRender.bytes == renderBefore,
          "nothing is shown before the server identifies a gift");
    detailView.SetUnit(0x1122334455667788ull);
    Check(hidden() && !detailView.ready() &&
              !detailView.Click(detailService.bytes.data(), detailView.button()),
          "an ordinary or not yet checked unit shows no gift origin");
    detailView.ShowOrigin(L"好友赠送", L"来自 好友 · 2026-11-03", true);
    Check(
        detailView.ready() && detailView.origin_visible() &&
            Read<std::uint8_t>(detailView.title(), 0x162) == 1 &&
            Read<std::uint8_t>(detailView.title(), 0x167) == 0 &&
            Read<std::uint8_t>(detailView.button(), 0x167) == 1 &&
            g_captions[detailView.title()] == L"好友赠送" &&
            g_captions[detailView.dates()] == L"来自 好友 · 2026-11-03" &&
            g_captions[detailView.button()] == L"查看寄语 >" &&
            detailView.Click(detailService.bytes.data(), detailView.button()) &&
            !detailView.Click(detailService.bytes.data(), detailView.title()),
        "a gifted unit shows the caption, sender, date and one open link");
    Check(detailRender.bytes == renderBefore,
          "the 3D viewport keeps its own state while the origin is shown");
    detailView.ShowOrigin(L"好友赠送", L"来自 好友 · 2026-11-03", false);
    Check(!detailView.ready() && detailView.origin_visible() &&
              Read<std::uint8_t>(detailView.button(), 0x162) == 1 &&
              Read<std::uint8_t>(detailView.button(), 0x167) == 0,
          "the open button stays visible and disabled while a gift request is busy");
    detailView.SetUnit(0x1122334455667799ull);
    Check(hidden() && !detailView.origin_visible(),
          "switching to another unit hides the previous unit's origin");
    detailView.SetUnit(0x1122334455667788ull);
    detailView.ShowOrigin(L"好友赠送", L"来自 好友 · 2026-11-03", true);
    auto* retiredDetailButton = detailView.button();
    auto* retiredDetailTitle = detailView.title();
    std::array<std::uint8_t, sizeof(Control)> retiredButtonBytes{};
    std::array<std::uint8_t, sizeof(Control)> retiredTitleBytes{};
    std::memcpy(retiredButtonBytes.data(), retiredDetailButton, retiredButtonBytes.size());
    std::memcpy(retiredTitleBytes.data(), retiredDetailTitle, retiredTitleBytes.size());
    const auto retiredRenderBytes = detailRender.bytes;
    DWORD retiredButtonProtection = 0;
    DWORD retiredTitleProtection = 0;
    Check(
        VirtualProtect(retiredDetailButton, sizeof(Control), PAGE_NOACCESS, &retiredButtonProtection) != 0 &&
            VirtualProtect(retiredDetailTitle, sizeof(Control), PAGE_NOACCESS, &retiredTitleProtection) != 0,
        "retired unit-detail children are inaccessible");
    auto* retiredFaultHandler = AddVectoredExceptionHandler(1, &CountRetiredAccessFault);
    Check(retiredFaultHandler != nullptr, "retired unit-detail access observer is installed");
    InterlockedExchange(&g_retired_access_faults, 0);
    InterlockedExchange(&g_watch_retired_access, 1);
    const bool detachedRetiredDetail = DetachRetiredUnitDetail(&detailView, detailService.bytes.data());
    InterlockedExchange(&g_watch_retired_access, 0);
    Check(RemoveVectoredExceptionHandler(retiredFaultHandler) != 0, "retired unit-detail access observer is removed");
    DWORD restoredProtection = 0;
    Check(
        VirtualProtect(retiredDetailButton, sizeof(Control), retiredButtonProtection, &restoredProtection) != 0 &&
            VirtualProtect(retiredDetailTitle, sizeof(Control), retiredTitleProtection, &restoredProtection) != 0,
        "fixture restores retired unit-detail allocations");
    Check(
        detachedRetiredDetail && InterlockedCompareExchange(&g_retired_access_faults, 0, 0) == 0,
        "retired unit-detail detach does not attempt SetState or any other child access");
    Check(
        detailView.service() == nullptr && detailView.button() == nullptr &&
            detailView.title() == nullptr && !detailView.ready(),
        "retired unit-detail detach invalidates every cached owner handle");
    Check(
        std::memcmp(retiredButtonBytes.data(), retiredDetailButton, retiredButtonBytes.size()) == 0 &&
            std::memcmp(retiredTitleBytes.data(), retiredDetailTitle, retiredTitleBytes.size()) == 0 &&
            detailRender.bytes == retiredRenderBytes,
        "retired unit-detail detach performs no UI writes");

    Control freshDetailService;
    Control freshDetailRoot;
    Control freshDetailRender;
    prepareRender(freshDetailRender, freshDetailRoot, freshDetailService);
    Check(
        detailView.Attach(freshDetailService.bytes.data(), freshDetailRoot.bytes.data(), bindings),
        "unit-detail view rebuilds on a fresh native page");
    detailView.SetUnit(0x8877665544332211ull);
    detailView.ShowOrigin(L"好友赠送", L"来自 新好友 · 2026-11-04", true);
    Check(
        detailView.ready() && detailView.service() == freshDetailService.bytes.data() &&
            detailView.button() != retiredDetailButton && detailView.title() != retiredDetailTitle,
        "fresh unit-detail controls accept normal state after retirement");
    Control smallRoot, smallRender, smallService;
    prepareRender(smallRender, smallRoot, smallService);
    Put<int>(smallRender.bytes.data(), 0x154, 150);
    UnitDetailView smallView;
    Check(!smallView.Attach(smallService.bytes.data(), smallRoot.bytes.data(), bindings),
          "an unexpected viewport size keeps the stock page unchanged");
    detailView.Detach(freshDetailService.bytes.data());

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
    {
        auto* count=Read<void*>(service.bytes.data(),0x94);
        Put<float>(count,0x148,35.0f);Put<float>(count,0x14C,86.0f);Put<int>(count,0x154,380);Put<int>(count,0x158,15);
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
    Check(
        storageAttached &&
            static_cast<int>(Read<float>(view.button(),0x148))==35+380-96-4 &&
            static_cast<int>(Read<float>(view.button(),0x14C))==85 &&
            Read<int>(view.button(),0x154)==96 &&
            g_captions[view.button()]==L"机体赠礼 >",
        "storage entry is a text link at the right end of the native count line");
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

    // The native owner may have destroyed the old page before the next init
    // notifies its subscribers. Detach is invalidation, not a live UI action.
    view.SetAvailable(true);
    view.ShowStatus(L"retiring page", true);
    auto* retiredButton = view.button();
    const auto retiredCaption = g_captions[retiredButton];
    const auto stockBeforeDetach = stock;
    const auto claimBeforeDetach = claim->bytes;
    DWORD oldProtection = 0;
    Check(VirtualProtect(retiredButton, sizeof(Control), PAGE_NOACCESS, &oldProtection) != 0,
          "retired native child is inaccessible");
    const bool detachedRetired = DetachRetiredStorage(&view, service.bytes.data());
    DWORD ignoredProtection = 0;
    Check(VirtualProtect(retiredButton, sizeof(Control), oldProtection, &ignoredProtection) != 0,
          "fixture restores its retired allocation after the check");
    Check(detachedRetired, "retired storage detach must not dereference freed controls");
    Check(!view.attached(), "storage companion detaches cleanly");
    Check(g_captions[retiredButton] == retiredCaption,
          "detach does not send a caption update to the retired native page");
    Check(claim->bytes == claimBeforeDetach,
          "detach does not restore visibility through retired Claim All pointers");
    for (std::size_t index = 0; index < stock.size(); ++index)
        Check(stock[index].bytes == stockBeforeDetach[index].bytes,
              "detach does not write through retired stock control pointers");
    Check(view.Attach(service.bytes.data(), dialog.bytes.data(), bindings) && view.ready(),
          "the same native service can bind a fresh page after retirement");
    view.ShowStatus(L"fresh page", true);
    view.RenderRows(rows, 2, 1, 2, 100, true, false);
    Check(view.GiftFor(view.RowActionForGift(100)) == 100,
          "fresh gift rows retain their operation identity after reattachment");
    view.SetAvailable(false);
    view.Detach(service.bytes.data());
    std::cout << "friend_unit_gift_native_ui_test: PASS\n";
    return 0;
}
