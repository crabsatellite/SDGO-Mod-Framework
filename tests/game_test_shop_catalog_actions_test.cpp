// Offline native-ABI fixtures only. Passing this executable is not real-game
// Shop acceptance and never authorizes a purchase, gift, grant or DB write.
#include "../mods/game_test_client/shop_catalog_actions.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr size_t kControlName = 0x118;
constexpr size_t kControlParent = 0x15C;
constexpr size_t kControlText = 0x21C;
constexpr size_t kStringLength = 0x10;
constexpr size_t kStringCapacity = 0x14;
constexpr size_t kCallbackSlot = 0x38 / sizeof(void*);

struct Bytes
{
    explicit Bytes(size_t size = 0) : data(size) {}
    std::vector<uint8_t> data;
    uint8_t* get() { return data.data(); }
};

struct Control
{
    std::array<uint8_t, 0x300> bytes = {};
};

struct Node
{
    Node* next = nullptr;
    Node* previous = nullptr;
    void* service = nullptr;
};

int failures = 0;
unsigned downmenuCalls = 0;
unsigned roomCalls = 0;
unsigned buyCancelCalls = 0;
unsigned giftCancelCalls = 0;
bool suppressPopup = false;
bool suppressPageAdvance = false;
bool catalogRequestPending=false;
bool retainEmptyNativeSlots=false;

Bytes app(0x600);
Bytes profile(0x7000);
Bytes unitInfo(0x40);
Bytes manager(0xB0);
Bytes downmenu(0x400);
Bytes room(0x400);
Bytes buyPopup(0x200);
Bytes giftPopup(0x200);
Bytes downmenuRoot(4);
Bytes roomRoot(4);
Bytes buyRoot(4);
Bytes giftRoot(4);
Bytes gridObject(4);
std::array<void*, 20> downmenuVtable = {};
std::array<void*, 20> roomVtable = {};
std::array<void*, 20> buyVtable = {};
std::array<void*, 20> giftVtable = {};
std::array<void*, 20> staticVtable = {};
std::deque<std::wstring> strings;
std::map<std::pair<void*, std::wstring>, Control*> controls;
std::vector<Control> controlStorage(32);
size_t controlCount = 0;
Node popupSentinel;
Node popupNode;
void* profilePointer = nullptr;
void* unitInfoPointer = nullptr;
void* managerPointer = nullptr;

std::array<uint8_t, 8 * 0x11> pageRows = {};
std::vector<sdgo::shop::Option> roomOptions;
std::vector<sdgo::shop::Option> popupOptions;
std::array<void*, 2> comboItems = {};
std::array<uint8_t, 0x70> categoryPermanent = {};
std::array<uint8_t, 0x70> categoryRental = {};
std::array<uint8_t, 128> nameAlpha = {};
std::array<uint8_t, 128> nameBravo = {};
std::array<std::array<uint8_t, 128>, 4> extraNames = {};
size_t extraNameCount = 0;
std::array<uint8_t, 2> gridToken = {};
std::array<uint8_t, 0x19> briefUnit = {};
std::array<uint8_t, 0x99> fullUnit = {};

Control* shopRadio = nullptr;
Control* allRadio = nullptr;
Control* rightButton = nullptr;
Control* buyButton = nullptr;
Control* giftButton = nullptr;
Control* goodsStatic = nullptr;
Control* buyCombo = nullptr;
Control* buyCancel = nullptr;
Control* giftCombo = nullptr;
Control* giftCancel = nullptr;

template <class T>
void Put(void* object, size_t offset, const T& value)
{
    std::memcpy(
        static_cast<uint8_t*>(object) + offset,
        &value,
        sizeof(value));
}

template <class T>
T Read(const void* object, size_t offset = 0)
{
    T value = {};
    std::memcpy(
        &value,
        static_cast<const uint8_t*>(object) + offset,
        sizeof(value));
    return value;
}

void SetHostString(
    void* object,
    size_t offset,
    const std::wstring& value)
{
    uint8_t* native =
        static_cast<uint8_t*>(object) + offset;
    std::memset(native, 0, 0x18);
    Put(
        native,
        kStringLength,
        static_cast<uint32_t>(value.size()));
    if (value.size() < 8) {
        Put(native, kStringCapacity, uint32_t{7});
        std::memcpy(
            native,
            value.c_str(),
            (value.size() + 1) *
                sizeof(wchar_t));
        return;
    }
    strings.push_back(value);
    wchar_t* pointer =
        const_cast<wchar_t*>(
            strings.back().c_str());
    Put(native, 0, pointer);
    Put(
        native,
        kStringCapacity,
        static_cast<uint32_t>(value.size()));
}

std::wstring HostString(const void* native)
{
    const uint8_t* bytes =
        static_cast<const uint8_t*>(native);
    const uint32_t length =
        Read<uint32_t>(bytes, kStringLength);
    const uint32_t capacity =
        Read<uint32_t>(bytes, kStringCapacity);
    const wchar_t* value =
        capacity < 8
            ? reinterpret_cast<const wchar_t*>(
                  bytes)
            : Read<wchar_t*>(bytes);
    return value
        ? std::wstring(value, length)
        : std::wstring();
}

Control* AddControl(
    void* root,
    const wchar_t* name,
    void* vtable = reinterpret_cast<void*>(0x12345678))
{
    Control* control =
        &controlStorage.at(controlCount++);
    control->bytes.fill(0);
    Put(control->bytes.data(), 0, vtable);
    Put(
        control->bytes.data(),
        kControlParent,
        root);
    SetHostString(
        control->bytes.data(),
        kControlName,
        name);
    controls.emplace(
        std::make_pair(
            root, std::wstring(name)),
        control);
    return control;
}

void SetVector(
    void* object,
    size_t beginOffset,
    size_t endOffset,
    void* begin,
    size_t bytes)
{
    const uintptr_t first =
        reinterpret_cast<uintptr_t>(begin);
    Put(object, beginOffset, first);
    Put(object, endOffset, first + bytes);
}

void EmptyVector(
    void* object,
    size_t beginOffset,
    size_t endOffset)
{
    const uintptr_t value =
        reinterpret_cast<uintptr_t>(
            pageRows.data());
    Put(object, beginOffset, value);
    Put(object, endOffset, value);
}

void DetachPopup()
{
    popupSentinel.next = &popupSentinel;
    popupSentinel.previous = &popupSentinel;
    popupNode = {};
    Put(manager.get(), 0x58, uint32_t{0});
}

void AttachPopup(void* service)
{
    popupNode.next = &popupSentinel;
    popupNode.previous = &popupSentinel;
    popupNode.service = service;
    popupSentinel.next = &popupNode;
    popupSentinel.previous = &popupNode;
    Put(manager.get(), 0x58, uint32_t{1});
}

void LoadPage(uint32_t page)
{
    std::memset(pageRows.data(), 0, pageRows.size());
    const uint32_t product =
        page == 0 ? 1019001 : 1019002;
    Put(pageRows.data(), 0, product);
    SetVector(
        room.get(),
        0x78,
        0x7C,
        pageRows.data(),
        retainEmptyNativeSlots ? pageRows.size() : 0x11);
    Put(room.get(), 0x68, page);
    Put(room.get(), 0x6C, uint32_t{1});
    SetHostString(
        goodsStatic->bytes.data(),
        kControlText,
        page == 0 ? L"Alpha" : L"Bravo");
}

void PreparePopup(
    void* popup,
    bool gift,
    uint32_t product)
{
    popupOptions.clear();
    for (const auto& option : roomOptions) {
        if (option.productId == product)
            popupOptions.push_back(option);
    }
    const size_t beginOffset =
        gift ? 0xB4 : 0xB0;
    const size_t endOffset =
        gift ? 0xB8 : 0xB4;
    SetVector(
        popup,
        beginOffset,
        endOffset,
        popupOptions.data(),
        popupOptions.size() *
            sizeof(sdgo::shop::Option));

    Control* combo =
        gift ? giftCombo : buyCombo;
    const std::wstring duration =
        product == 1019001
            ? L"永久"
            : L"租赁 7 天";
    SetHostString(
        combo->bytes.data(),
        kControlText,
        duration);
    comboItems[0] =
        combo->bytes.data() +
        kControlText;
    SetVector(
        combo->bytes.data(),
        0x190,
        0x194,
        comboItems.data(),
        sizeof(void*));
}

uint8_t* __cdecl GameApp()
{
    return app.get();
}

void* __fastcall NativeStringCtor(
    void* self,
    void*,
    const wchar_t* text)
{
    SetHostString(
        self,
        0,
        text ? text : L"");
    return self;
}

void __fastcall NativeStringDtor(void*, void*)
{
}

void Check(bool,const char*);
uint32_t __fastcall RequestPending(void* self,void*,ShopCatalogActions::NativeRequestName name)
{
    const auto text=HostString(name.bytes.data());
    Check(self==manager.get() && text==L"REQ_GET_STORE_ITEM_LIST",
          "native pending-query receives the exact manager and by-value string ABI");
    return catalogRequestPending ? 1u : 0u;
}

void* __fastcall ServiceFind(
    void*,
    void*,
    const void* descriptor)
{
    const std::wstring name =
        HostString(
            static_cast<const uint8_t*>(
                descriptor) +
            4);
    if (name == L"downmenu")
        return downmenu.get();
    if (name == L"roommgr" &&
        Read<uint32_t>(app.get(), 0x4A0) == 9)
        return room.get();
    return nullptr;
}

void* __fastcall FindControl(
    void* root,
    void*,
    const void* nativeName,
    uint32_t)
{
    const auto found =
        controls.find(
            {root, HostString(nativeName)});
    return found == controls.end()
        ? nullptr
        : found->second->bytes.data();
}

uint8_t __fastcall GridFind(
    void*,
    void*,
    const uint8_t** output,
    uint32_t id,
    int type,
    char)
{
    *output = nullptr;
    if (type == 0x22 && id == 1100001)
        *output = categoryPermanent.data();
    else if (type == 0x22 && id == 1100002)
        *output = categoryRental.data();
    else if (type == 0x23 && id == 210001)
        *output = nameAlpha.data();
    else if (type == 0x23 && id == 210002)
        *output = nameBravo.data();
    else if (type == 0x23) {
        for (size_t index = 0;
             index < extraNameCount;
             ++index) {
            if (Read<uint32_t>(
                    extraNames[index].data()) == id) {
                *output =
                    extraNames[index].data();
                break;
            }
        }
    }
    return *output ? 1 : 0;
}

void* __cdecl GridData()
{
    return gridToken.data();
}

void __fastcall UnitBrief(
    void*,
    void*,
    void* output,
    uint16_t slot)
{
    std::memset(output, 0, 0x19);
    if (slot == 1)
        std::memcpy(output, briefUnit.data(), 0x19);
}

void __fastcall UnitFull(
    void*,
    void*,
    void* output,
    uint16_t slot)
{
    std::memset(output, 0, 0x99);
    if (slot == 1)
        std::memcpy(output, fullUnit.data(), 0x99);
}

void* __fastcall ButtonTextItem(
    void* combo,
    void*,
    uint32_t index)
{
    if (index != 0 ||
        (combo != buyCombo->bytes.data() &&
         combo != giftCombo->bytes.data()))
        return nullptr;
    return comboItems[0];
}

uint32_t __stdcall DownmenuCallback(
    void* service,
    uint32_t event,
    const void*,
    void* control)
{
    ++downmenuCalls;
    if (service == downmenu.get() &&
        event == 0x301 &&
        control == shopRadio->bytes.data())
        Put(app.get(), 0x4A0, uint32_t{9});
    return 0;
}

uint32_t __stdcall RoomCallback(
    void* service,
    uint32_t event,
    const void*,
    void* control)
{
    ++roomCalls;
    if (service != room.get())
        return 0;
    if (event == 0x301 &&
        control == allRadio->bytes.data()) {
        LoadPage(0);
        return 0;
    }
    if (event != 0x101)
        return 0;
    if (control == rightButton->bytes.data()) {
        if (!suppressPageAdvance)
            LoadPage(1);
        return 0;
    }
    const uint32_t page =
        Read<uint32_t>(room.get(), 0x68);
    const uint32_t product =
        page == 0 ? 1019001 : 1019002;
    if (control == buyButton->bytes.data()) {
        if (!suppressPopup) {
            PreparePopup(
                buyPopup.get(),
                false,
                product);
            AttachPopup(buyPopup.get());
        }
    } else if (
        control == giftButton->bytes.data()) {
        if (!suppressPopup) {
            PreparePopup(
                giftPopup.get(),
                true,
                product);
            AttachPopup(giftPopup.get());
        }
    }
    return 0;
}

void __stdcall BuyCallback(
    void* service,
    uint32_t event,
    const void*,
    void* control)
{
    if (service == buyPopup.get() &&
        event == 0x101 &&
        control == buyCancel->bytes.data()) {
        ++buyCancelCalls;
        DetachPopup();
    }
}

void __stdcall GiftCallback(
    void* service,
    uint32_t event,
    const void*,
    void* control)
{
    if (service == giftPopup.get() &&
        event == 0x101 &&
        control == giftCancel->bytes.data()) {
        ++giftCancelCalls;
        DetachPopup();
    }
}

void Check(bool value, const char* message)
{
    if (!value) {
        ++failures;
        std::fprintf(
            stderr,
            "FAIL: %s\n",
            message);
    }
}

std::wstring TemporaryFile(
    const wchar_t* suffix)
{
    wchar_t path[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, path);
    return std::wstring(path) +
           L"SDGO-shop-native-" +
           std::to_wstring(
               GetCurrentProcessId()) +
           suffix;
}

void WriteUtf8(
    const std::wstring& path,
    const char* text)
{
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    Check(
        file != INVALID_HANDLE_VALUE,
        "temporary fixture opens");
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    const DWORD size =
        static_cast<DWORD>(
            std::strlen(text));
    Check(
        WriteFile(
            file,
            text,
            size,
            &written,
            nullptr) &&
            written == size,
        "temporary fixture writes");
    CloseHandle(file);
}

void WriteConfiguration(
    const std::wstring& assertions,
    const std::wstring& display)
{
    WriteUtf8(
        assertions,
        "[shop]\n"
        "Schema=1\n"
        "Scope=StockAssertions\n"
        "CatalogId=stock-assertions\n"
        "Revision=rev-1\n"
        "\n"
        "[product:alpha]\n"
        "ProductId=1019001\n"
        "ItemId=190001\n"
        "TemplateId=1019001\n"
        "Category=1100001\n"
        "Price=500\n"
        "Currency=GB\n"
        "Validity=1\n"
        "Enabled=1\n"
        "Name=Alpha\n"
        "Label=Permanent\n"
        "\n"
        "[product:bravo]\n"
        "ProductId=1019002\n"
        "ItemId=190002\n"
        "TemplateId=1019002\n"
        "Category=1100002\n"
        "Price=900\n"
        "Currency=MB\n"
        "Validity=7\n"
        "Enabled=1\n"
        "Name=Bravo\n"
        "Label=Rental\n");
    WriteUtf8(
        display,
        "[shop]\n"
        "Revision=rev-1\n"
        "\n"
        "[option:3001]\n"
        "Category=1100001\n"
        "Validity=1\n"
        "Payment=1\n"
        "Price=500\n"
        "Text=Permanent\n"
        "\n"
        "[option:3002]\n"
        "Category=1100002\n"
        "Validity=7\n"
        "Payment=2\n"
        "Price=900\n"
        "Text=Rental\n");
}

void SetExtraName(
    size_t index,
    uint32_t gridId,
    uint32_t productId,
    const wchar_t* name)
{
    Check(
        index < extraNames.size() &&
            name &&
            wcslen(name) < 50,
        "extra native title fixture is bounded");
    if (index >= extraNames.size() || !name)
        return;
    extraNames[index].fill(0);
    Put(
        extraNames[index].data(),
        0,
        gridId);
    Put(
        extraNames[index].data(),
        5,
        productId);
    std::memcpy(
        extraNames[index].data() + 9,
        name,
        (wcslen(name) + 1) *
            sizeof(wchar_t));
    extraNameCount =
        (std::max)(extraNameCount, index + 1);
}

SdgoGameTestContextV1 Context(
    uint64_t elapsed = 0,
    const char* role = "client")
{
    SdgoGameTestContextV1 context = {};
    context.size = sizeof(context);
    context.version =
        SDGO_GAME_TEST_VERSION;
    context.run_id =
        "12345678-1234-1234-1234-123456789abc";
    context.role = role;
    context.case_id = "shop-catalog";
    context.elapsed_ms = elapsed;
    return context;
}

ShopCatalogActions::Bindings Bindings()
{
    ShopCatalogActions::Bindings value = {};
    value.game_app = &GameApp;
    value.profile_pointer =
        reinterpret_cast<uint8_t**>(
            &profilePointer);
    value.unit_info_pointer =
        &unitInfoPointer;
    value.unit_brief_by_slot =
        reinterpret_cast<
            ShopCatalogActions::UnitBySlotFn>(
            &UnitBrief);
    value.unit_full_by_slot =
        reinterpret_cast<
            ShopCatalogActions::UnitBySlotFn>(
            &UnitFull);
    value.service_managed_pointer =
        &managerPointer;
    value.request_pending=reinterpret_cast<ShopCatalogActions::RequestPendingFn>(&RequestPending);
    value.service_find_by_name =
        reinterpret_cast<
            ShopCatalogActions::
                ServiceFindByNameFn>(
            &ServiceFind);
    value.native_wstring_ctor =
        reinterpret_cast<
            ShopCatalogActions::
                NativeWStringCtorFn>(
            &NativeStringCtor);
    value.native_wstring_dtor =
        reinterpret_cast<
            ShopCatalogActions::
                NativeWStringDtorFn>(
            &NativeStringDtor);
    value.native_ui_find_control =
        reinterpret_cast<
            ShopCatalogActions::
                NativeUiFindControlFn>(
            &FindControl);
    value.grid_data = &GridData;
    value.grid_find =
        reinterpret_cast<
            ShopCatalogActions::GridFindFn>(
            &GridFind);
    value.button_text_item =
        reinterpret_cast<
            ShopCatalogActions::
                ButtonTextItemFn>(
            &ButtonTextItem);
    value.downmenu_vtable =
        downmenuVtable.data();
    value.downmenu_callback =
        &DownmenuCallback;
    value.room_vtable =
        roomVtable.data();
    value.room_callback =
        &RoomCallback;
    value.buy_vtable =
        buyVtable.data();
    value.buy_callback =
        &BuyCallback;
    value.gift_vtable =
        giftVtable.data();
    value.gift_callback =
        &GiftCallback;
    value.static_text_vtable =
        staticVtable.data();
    return value;
}

void Setup()
{
    catalogRequestPending=false;
    retainEmptyNativeSlots=false;
    app.data.assign(app.data.size(), 0);
    profile.data.assign(profile.data.size(), 0);
    unitInfo.data.assign(unitInfo.data.size(), 0);
    manager.data.assign(manager.data.size(), 0);
    downmenu.data.assign(downmenu.data.size(), 0);
    room.data.assign(room.data.size(), 0);
    buyPopup.data.assign(buyPopup.data.size(), 0);
    giftPopup.data.assign(giftPopup.data.size(), 0);
    strings.clear();
    controls.clear();
    controlCount = 0;
    downmenuCalls = roomCalls = 0;
    buyCancelCalls = giftCancelCalls = 0;
    suppressPopup = false;
    suppressPageAdvance = false;
    popupSentinel = {};
    popupNode = {};
    DetachPopup();

    downmenuVtable.fill(nullptr);
    roomVtable.fill(nullptr);
    buyVtable.fill(nullptr);
    giftVtable.fill(nullptr);
    staticVtable.fill(nullptr);
    downmenuVtable[kCallbackSlot] =
        reinterpret_cast<void*>(
            &DownmenuCallback);
    roomVtable[kCallbackSlot] =
        reinterpret_cast<void*>(
            &RoomCallback);
    buyVtable[kCallbackSlot] =
        reinterpret_cast<void*>(
            &BuyCallback);
    giftVtable[kCallbackSlot] =
        reinterpret_cast<void*>(
            &GiftCallback);
    Put(
        downmenu.get(),
        0,
        downmenuVtable.data());
    Put(
        room.get(),
        0,
        roomVtable.data());
    Put(
        buyPopup.get(),
        0,
        buyVtable.data());
    Put(
        giftPopup.get(),
        0,
        giftVtable.data());

    profilePointer = profile.get();
    unitInfoPointer = unitInfo.get();
    managerPointer = manager.get();
    Put(
        manager.get(),
        0x54,
        &popupSentinel);
    Put(app.get(), 0x4A0, uint32_t{6});
    Put(profile.get(), 0x18F, uint32_t{2000});
    Put(profile.get(), 0x58C, uint32_t{3000});
    Put(profile.get(), 0x1AD, uint16_t{1});
    Put(unitInfo.get(), 0x0C, int32_t{2});
    Put(unitInfo.get(), 0x20, uint8_t{1});
    briefUnit.fill(0);
    fullUnit.fill(0);
    Put(briefUnit.data(), 0, uint32_t{101});
    Put(briefUnit.data(), 4, uint64_t{10001});
    Put(briefUnit.data(), 12, uint16_t{1});
    Put(fullUnit.data(), 0, uint32_t{101});
    Put(fullUnit.data(), 4, uint32_t{1});
    Put(fullUnit.data(), 8, uint64_t{10001});

    Put(
        downmenu.get(),
        0x44,
        downmenuRoot.get());
    Put(room.get(), 0x64, roomRoot.get());
    Put(
        buyPopup.get(),
        0x4C,
        buyRoot.get());
    Put(
        giftPopup.get(),
        0x4C,
        giftRoot.get());

    shopRadio = AddControl(
        downmenuRoot.get(),
        L"ShopRoomRadio");
    Put(
        downmenu.get(),
        0x50,
        shopRadio->bytes.data());
    Put(downmenu.get(), 0x60, uint32_t{0});

    allRadio = AddControl(
        roomRoot.get(),
        L"AllSerchRadio");
    rightButton = AddControl(
        roomRoot.get(),
        L"RightBtn");
    buyButton = AddControl(
        roomRoot.get(),
        L"BuyBtn0");
    giftButton = AddControl(
        roomRoot.get(),
        L"GiftBtn0");
    goodsStatic = AddControl(
        roomRoot.get(),
        L"GoodsStatic0",
        staticVtable.data());
    Put(
        room.get(),
        0x2F4,
        allRadio->bytes.data());
    EmptyVector(room.get(), 0x78, 0x7C);
    EmptyVector(room.get(), 0x88, 0x8C);

    buyCombo = AddControl(
        buyRoot.get(),
        L"Combo");
    buyCancel = AddControl(
        buyRoot.get(),
        L"BuyCancelBtn");
    giftCombo = AddControl(
        giftRoot.get(),
        L"Combo");
    giftCancel = AddControl(
        giftRoot.get(),
        L"BuyCancelBtn");
    Put(
        buyPopup.get(),
        0x50,
        buyCombo->bytes.data());
    Put(
        buyPopup.get(),
        0x60,
        buyCancel->bytes.data());
    Put(
        giftPopup.get(),
        0xAC,
        giftCombo->bytes.data());
    Put(
        giftPopup.get(),
        0xA8,
        giftCancel->bytes.data());

    roomOptions.clear();
    sdgo::shop::Option alpha = {};
    alpha.productId = 1019001;
    alpha.id = 3001;
    alpha.price = 500;
    alpha.validity = 1;
    alpha.category = 1100001;
    alpha.payment = 1;
    strcpy_s(alpha.text, "Permanent");
    roomOptions.push_back(alpha);
    sdgo::shop::Option bravo = {};
    bravo.productId = 1019002;
    bravo.id = 3002;
    bravo.price = 900;
    bravo.validity = 7;
    bravo.category = 1100002;
    bravo.payment = 2;
    strcpy_s(bravo.text, "Rental");
    roomOptions.push_back(bravo);
    SetVector(
        room.get(),
        0x88,
        0x8C,
        roomOptions.data(),
        roomOptions.size() *
            sizeof(sdgo::shop::Option));

    categoryPermanent.fill(0);
    categoryRental.fill(0);
    Put(
        categoryPermanent.data(),
        0,
        uint32_t{1100001});
    Put(
        categoryRental.data(),
        0,
        uint32_t{1100002});
    categoryPermanent[0x6D] = 0;
    categoryRental[0x6D] = 1;

    nameAlpha.fill(0);
    nameBravo.fill(0);
    for (auto& row : extraNames)
        row.fill(0);
    extraNameCount = 0;
    Put(nameAlpha.data(), 0, uint32_t{210001});
    Put(nameAlpha.data(), 5, uint32_t{1019001});
    const wchar_t alphaName[] = L"Alpha";
    std::memcpy(
        nameAlpha.data() + 9,
        alphaName,
        sizeof(alphaName));
    Put(nameBravo.data(), 0, uint32_t{210002});
    Put(nameBravo.data(), 5, uint32_t{1019002});
    const wchar_t bravoName[] = L"Bravo";
    std::memcpy(
        nameBravo.data() + 9,
        bravoName,
        sizeof(bravoName));
}

ShopCatalogActions Driver(
    const std::wstring& assertions,
    const std::wstring& display)
{
    ShopCatalogActions actions;
    Check(
        actions.Bind(Bindings()),
        "exact native ABI bindings accepted");
    Check(
        actions.Configure(assertions, display),
        "stock assertions and runtime expectations accepted");
    return actions;
}

ShopCatalogActions::Result PollTo(
    ShopCatalogActions& actions,
    ShopCatalogActions::Result terminal,
    uint64_t* elapsed = nullptr)
{
    uint64_t now = elapsed ? *elapsed : 0;
    ShopCatalogActions::Result result =
        ShopCatalogActions::kUnbound;
    for (unsigned index = 0;
         index < 200;
         ++index) {
        const auto context = Context(now);
        result = actions.Poll(&context);
        now += 100;
        if (result == terminal ||
            result >=
                ShopCatalogActions::
                    kInvalidContext)
            break;
    }
    if (elapsed)
        *elapsed = now;
    return result;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc == 3) {
        ShopCatalogActions checkedIn;
        Check(
            checkedIn.Configure(argv[1], argv[2]) &&
                checkedIn.State().configured == 2 &&
                checkedIn.State().
                        stock_assertions ==
                    1,
            "exact stock assertion helper paths parse as two read-only targets");
    } else {
        Check(
            argc == 1,
            "optional arguments are assertion and runtime expectation paths");
    }

    const std::wstring assertions =
        TemporaryFile(L"-stock-assertions.ini");
    const std::wstring display =
        TemporaryFile(L"-display.ini");
    WriteConfiguration(assertions, display);

    Setup();
    auto actions = Driver(assertions, display);
    const auto completed =
        PollTo(
            actions,
            ShopCatalogActions::kComplete);
    const auto state = actions.State();
    Check(
        completed ==
            ShopCatalogActions::kComplete,
        "ordinary shop, page, buy preview, cancel, gift preview and cancel complete");
    Check(
        state.configured == 2 &&
            state.stock_assertions == 1 &&
            state.verified == 2 &&
            state.buy_previews == 2 &&
            state.gift_previews == 2 &&
            state.cancellations == 4,
        "both stock assertions retain native preview receipts");
    Check(
        state.wallet_unchanged &&
            state.inventory_unchanged,
        "exact wallet and full inventory image remain unchanged");
    Check(
        downmenuCalls == 1 &&
            roomCalls == 6 &&
            buyCancelCalls == 2 &&
            giftCancelCalls == 2,
        "one original action per transition without retries");
    Check(
        state.option_label == L"租赁 7 天" &&
            state.payment == 2 &&
            state.price == 900 &&
            state.validity == 7,
        "native popup duration, price, currency and validity are read back");

    Setup();
    Put(app.get(),0x4A0,uint32_t{5});
    fullUnit[6]=1;fullUnit[7]=0xA5;
    actions=Driver(assertions,display);
    Check(PollTo(actions,ShopCatalogActions::kComplete)==ShopCatalogActions::kComplete &&
              downmenuCalls==1 && actions.State().wallet_unchanged && actions.State().inventory_unchanged,
          "hangar shop entry preserves adjacent full-unit flag bytes instead of treating them as slot bits");

    Setup();
    actions = Driver(assertions, display);
    for(uint64_t tick=0;tick<3;++tick){const auto step=Context(tick*100);actions.Poll(&step);}
    catalogRequestPending=true;
    const auto waitingRequest=Context(300);
    Check(actions.Poll(&waitingRequest)==ShopCatalogActions::kWaitingForCatalog && roomCalls==0,
        "native pending-list ownership prevents an ignored one-shot AllSearch callback");
    catalogRequestPending=false;
    uint64_t resumedRequest=400;
    Check(PollTo(actions,ShopCatalogActions::kComplete,&resumedRequest)==ShopCatalogActions::kComplete,
        "the real selection callback runs after the prior native query finishes");

    Setup();
    actions = Driver(assertions, display);
    auto context = Context(0);
    Check(
        actions.Poll(&context) ==
            ShopCatalogActions::kWaitingForLobby,
        "baseline captured before any shop action");
    Put(profile.get(), 0x18F, uint32_t{1999});
    context.elapsed_ms = 100;
    Check(
        actions.Poll(&context) ==
            ShopCatalogActions::kWalletChanged &&
            downmenuCalls == 0,
        "wallet mutation fails before another original action");

    Setup();
    actions = Driver(assertions, display);
    context = Context(0);
    actions.Poll(&context);
    fullUnit[20] ^= 1;
    context.elapsed_ms = 100;
    Check(
        actions.Poll(&context) ==
            ShopCatalogActions::kInventoryChanged &&
            downmenuCalls == 0,
        "exact full unit mutation fails closed");

    Setup();
    actions = Driver(assertions, display);
    suppressPopup = true;
    uint64_t elapsed = 0;
    ShopCatalogActions::Result pending =
        ShopCatalogActions::kUnbound;
    for (unsigned index = 0; index < 20; ++index) {
        context = Context(elapsed);
        pending = actions.Poll(&context);
        elapsed += 100;
        if (pending ==
            ShopCatalogActions::
                kWaitingForBuyPreview)
            break;
    }
    const unsigned callsBefore = roomCalls;
    for (unsigned index = 0; index < 5; ++index) {
        context = Context(elapsed);
        Check(
            actions.Poll(&context) ==
                ShopCatalogActions::
                    kWaitingForBuyPreview,
            "asynchronous popup wait remains pending");
        elapsed += 100;
    }
    Check(
        roomCalls == callsBefore,
        "popup wait never repeats the buy action");

    Setup();
    actions = Driver(assertions, display);
    elapsed = 0;
    for (unsigned index = 0; index < 30; ++index) {
        context = Context(elapsed);
        const auto result =
            actions.Poll(&context);
        elapsed += 100;
        if (result ==
            ShopCatalogActions::
                kWaitingForBuyPreview &&
            actions.State().buy_previews == 1)
            break;
    }
    SetHostString(
        buyCancel->bytes.data(),
        kControlName,
        L"BuyBtn");
    context = Context(elapsed);
    Check(
        actions.Poll(&context) ==
            ShopCatalogActions::kControlMismatch &&
            buyCancelCalls == 0,
        "cancel requires the exact live BuyCancelBtn identity");

    Setup();
    actions = Driver(assertions, display);
    std::array<uint8_t, 16> foreign = {};
    Put(
        foreign.data(),
        0,
        buyVtable.data());
    AttachPopup(foreign.data());
    context = Context(0);
    actions.Poll(&context);
    context.elapsed_ms = 100;
    Check(
        actions.Poll(&context) ==
            ShopCatalogActions::
                kUnsafeModalPresent &&
            downmenuCalls == 0,
        "pre-existing modal blocks shop entry");

    Setup();
    actions = Driver(assertions, display);
    context = Context(0, "server");
    Check(
        actions.Poll(&context) ==
            ShopCatalogActions::kInvalidContext,
        "non-client GameTest context rejected");

    const std::wstring malformed =
        TemporaryFile(L"-malformed.ini");
    WriteUtf8(
        malformed,
        "[shop]\n"
        "Schema=1\n"
        "CatalogId=host-shop\n"
        "Revision=rev-1\n"
        "[product:alpha]\n"
        "ProductId=1019001\n"
        "ItemId=190001\n"
        "TemplateId=1010001\n"
        "Category=1100001\n"
        "Price=0\n"
        "Currency=GB\n"
        "Validity=1\n"
        "Name=Alpha\n"
        "Label=Permanent\n");
    Setup();
    ShopCatalogActions invalid;
    Check(
        invalid.Bind(Bindings()),
        "malformed contract fixture still binds native ABI");
    Check(
        !invalid.Configure(
            malformed,
            display) &&
            invalid.State().result ==
                ShopCatalogActions::
                    kConfigurationRejected,
        "invalid numeric assertion contract rejected");

    const std::wstring misdeclaredStock =
        TemporaryFile(L"-misdeclared-stock.ini");
    WriteUtf8(
        misdeclaredStock,
        "[shop]\n"
        "Schema=1\n"
        "Scope=StockAssertions\n"
        "CatalogId=stock-assertions\n"
        "Revision=rev-1\n"
        "[product:alpha]\n"
        "ProductId=1019001\n"
        "ItemId=190001\n"
        "TemplateId=1010001\n"
        "Category=1100001\n"
        "Price=500\n"
        "Currency=GB\n"
        "Validity=1\n"
        "Name=Alpha\n"
        "Label=Permanent\n");
    Check(
        !invalid.Configure(
            misdeclaredStock,
            display),
        "stock assertions require self identity instead of claiming an owner template");

    const std::wstring stale =
        TemporaryFile(L"-stale.ini");
    WriteUtf8(
        stale,
        "[shop]\nRevision=old\n"
        "[option:3001]\n"
        "Category=1100001\nValidity=1\n"
        "Payment=1\nPrice=500\nText=Permanent\n");
    Check(
        !invalid.Configure(assertions, stale),
        "stale committed revision rejected");

    const std::wstring duplicate =
        TemporaryFile(L"-duplicate.ini");
    WriteUtf8(
        duplicate,
        "[shop]\nRevision=rev-1\n"
        "[option:3001]\n"
        "Category=1100001\nValidity=1\n"
        "Payment=1\nPrice=500\nText=Permanent\n"
        "[option:3999]\n"
        "Category=1100001\nValidity=1\n"
        "Payment=1\nPrice=500\nText=Permanent\n"
        "[option:3002]\n"
        "Category=1100002\nValidity=7\n"
        "Payment=2\nPrice=900\nText=Rental\n");
    Check(
        !invalid.Configure(assertions, duplicate),
        "ambiguous generated option mapping rejected");

    Setup();
    SetExtraName(0,210003,9999999,L"x");
    const std::wstring fullWidthTitle(50,L'x');
    std::memcpy(extraNames[0].data()+9,fullWidthTitle.data(),100);
    actions=Driver(assertions,display);
    Check(PollTo(actions,ShopCatalogActions::kComplete)==ShopCatalogActions::kComplete,
          "a full-width unrelated native title cannot block the selected stock assertions");

    Setup();retainEmptyNativeSlots=true;LoadPage(0);
    actions=Driver(assertions,display);
    Check(PollTo(actions,ShopCatalogActions::kComplete)==ShopCatalogActions::kComplete &&
        actions.State().native_page_slots==8 && actions.State().native_page_nonempty==1,
        "eight native display slots can retain blank entries while row identities and previews remain exact");

    Setup();
    SetExtraName(
        0,
        210003,
        9999999,
        L"Unrelated A");
    SetExtraName(
        1,
        210004,
        9999999,
        L"Unrelated B");
    actions = Driver(assertions, display);
    const auto unrelatedDuplicateResult =
        PollTo(
            actions,
            ShopCatalogActions::kComplete);
    const auto unrelatedDuplicateState =
        actions.State();
    Check(
        unrelatedDuplicateResult ==
                ShopCatalogActions::kComplete &&
            unrelatedDuplicateState.
                    duplicate_native_title_rows ==
                1 &&
            unrelatedDuplicateState.
                    duplicate_native_product_ids.
                    size() ==
                1 &&
            unrelatedDuplicateState.
                    duplicate_native_product_ids[0] ==
                9999999,
        "unrelated duplicate native ProductId is recorded without blocking unique targets");

    Setup();
    SetExtraName(
        0,
        210003,
        1019001,
        L"Alpha");
    actions = Driver(assertions, display);
    const auto targetDuplicateResult =
        PollTo(
            actions,
            ShopCatalogActions::
                kNativeNameMismatch);
    Check(
        targetDuplicateResult ==
                ShopCatalogActions::
                    kNativeNameMismatch &&
            actions.State().buy_previews == 0 &&
            actions.State().gift_previews == 0,
        "duplicate target ProductId fails closed before any preview even when both titles match");

    Setup();
    actions = Driver(assertions, display);
    SetHostString(
        goodsStatic->bytes.data(),
        kControlText,
        L"Wrong");
    // AllSearch repaints the fixture name, so change the native catalog name
    // instead; production requires native and visible sources to agree.
    const wchar_t wrongName[] = L"Other";
    std::memcpy(
        nameAlpha.data() + 9,
        wrongName,
        sizeof(wrongName));
    const auto wrongNameResult =
        PollTo(
            actions,
            ShopCatalogActions::
                kNativeNameMismatch);
    Check(
        wrongNameResult ==
            ShopCatalogActions::
                kNativeNameMismatch,
        "asserted stock name must agree with the shipped native catalog");

    DeleteFileW(assertions.c_str());
    DeleteFileW(display.c_str());
    DeleteFileW(malformed.c_str());
    DeleteFileW(misdeclaredStock.c_str());
    DeleteFileW(stale.c_str());
    DeleteFileW(duplicate.c_str());
    std::printf(
        "shop native catalog adapter: %d failures\n",
        failures);
    return failures ? 1 : 0;
}
