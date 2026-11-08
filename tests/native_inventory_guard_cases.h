// Actual-DLL ABI and ownership cases for the original menu business and frame tail.
std::array<uint8_t, 0x500> inventoryApp{};
std::array<uint8_t, 0x28> pendingInventory{};
std::array<uint8_t, 0xb0> inventoryServices{};
std::array<uint8_t, 0x64> inventoryMenu{};
std::array<uint8_t, 0x180> inventoryMenuRoot{};
std::array<std::array<uint8_t, 0x180>, 6> inventoryControls{};
std::array<uintptr_t, 3> inventoryServiceHead{}, inventoryServiceNode{};
std::array<void*, 16> inventoryMenuVtable{};
uint8_t* inventoryObject = pendingInventory.data();
uint8_t* inventoryAppObject = inventoryApp.data();
uint8_t* inventoryServiceObject = inventoryServices.data();
void* inventoryMenuHook = nullptr;
void* inventoryTailHook = nullptr;
void* inventoryCatalogHook = nullptr;
unsigned inventoryMenuCalls = 0, inventoryCatalogCalls = 0;
void* observedInventoryMenu = nullptr;
void* observedInventoryControl = nullptr;
const void* observedInventoryPayload = nullptr;
DWORD originalInventoryLastError = 0;
bool inventoryStarterBusy = false;
using InventoryMenuHook = void(__fastcall*)(void*, void*, const void*, void*);
uint32_t __stdcall InventoryNativeMenuCallback(void* menu, uint32_t event, const void* payload, void* control) {
    if (event == 0x301 && !inventoryStarterBusy) reinterpret_cast<InventoryMenuHook>(inventoryMenuHook)(menu, nullptr, payload, control);
    return 0;
}

void __fastcall OriginalInventoryMenu(void* menu, void*, const void* payload, void* control) {
    ++inventoryMenuCalls;
    observedInventoryMenu = menu; observedInventoryPayload = payload; observedInventoryControl = control;
    originalInventoryLastError = GetLastError();
    SetLastError(0x1234ABCD);
}
void __cdecl OriginalInventoryCatalog() { ++inventoryCatalogCalls; SetLastError(0xAAAABBBB); }
__declspec(naked) void InventoryTailOriginal() {
    __asm { pop esi }
    __asm { mov esp, ebp }
    __asm { pop ebp }
    __asm { ret }
}
__declspec(naked) void InvokeInventoryTail() {
    __asm { push ebp }
    __asm { mov ebp, esp }
    __asm { push esi }
    __asm { jmp dword ptr [inventoryTailHook] }
}
void* InventoryGuardSymbol(const char* key) {
    if (!std::strcmp(key, "native_data_guard.inventory_pointer")) return &inventoryObject;
    if (!std::strcmp(key, "native_data_guard.app_pointer")) return &inventoryAppObject;
    if (!std::strcmp(key, "native_data_guard.service_manager_pointer")) return &inventoryServiceObject;
    if (!std::strcmp(key, "native_data_guard.menu_vtable")) return inventoryMenuVtable.data();
    if (!std::strcmp(key, "native_data_guard.menu_callback")) return reinterpret_cast<void*>(&InventoryNativeMenuCallback);
    return nullptr;
}
bool InventoryGuardCodeHook(const char* key, void* detour, void** original) {
    if (!std::strcmp(key, "native_data_guard.catalog_open")) {
        inventoryCatalogHook = detour;
        *original = reinterpret_cast<void*>(&OriginalInventoryCatalog); return true;
    }
    if (!std::strcmp(key, "native_data_guard.menu_business")) {
        inventoryMenuHook = detour;
        *original = reinterpret_cast<void*>(&OriginalInventoryMenu); return true;
    }
    return false;
}
bool InventoryGuardSiteHook(const char* key, void* detour, void** original) {
    if (std::strcmp(key, "native_data_guard.frame_tail")) return false;
    inventoryTailHook = detour;
    *original = reinterpret_cast<void*>(&InventoryTailOriginal); return true;
}
void RunInventoryGuardCases(HMODULE dll) {
    Check(inventoryMenuHook && inventoryTailHook && inventoryCatalogHook,
          "actual DLL guards both original navigation business entries and the native frame tail");
    Put(inventoryServices.data(), 0x44, inventoryServiceHead.data());
    inventoryServiceHead[0] = reinterpret_cast<uintptr_t>(inventoryServiceNode.data());
    inventoryServiceNode[0] = reinterpret_cast<uintptr_t>(inventoryServiceHead.data());
    inventoryServiceNode[2] = reinterpret_cast<uintptr_t>(inventoryMenu.data());
    Put(inventoryMenu.data(), 0, inventoryMenuVtable.data());
    Put(inventoryMenu.data(), 0x44, inventoryMenuRoot.data());
    for (size_t i = 0; i < inventoryControls.size(); ++i) {
        Put(inventoryMenu.data(), 0x48 + i * 4, inventoryControls[i].data());
        Put(inventoryControls[i].data(), 0x15c, inventoryMenuRoot.data());
        inventoryControls[i][0x162] = inventoryControls[i][0x167] = 1;
    }
    using MenuHook = void(__fastcall*)(void*, void*, const void*, void*);
    const auto menu = reinterpret_cast<MenuHook>(inventoryMenuHook);
    const auto open = reinterpret_cast<void(__cdecl*)()>(inventoryCatalogHook);
    const auto stage = [&](uint32_t state, uint32_t capacity, uint8_t ready) {
        Put(inventoryApp.data(), 0x4a0, state);
        Put(inventoryApp.data(), 0x4b0, uint32_t{0});
        Put(pendingInventory.data(), 0x0c, capacity);
        Put(pendingInventory.data(), 0x18, uint64_t{0x1234567812345678});
        pendingInventory[0x20] = ready;
    };
    const void* payload = reinterpret_cast<void*>(0xDEAD2468);
    for (size_t i : {size_t{0}, size_t{1}, size_t{2}, size_t{4}, size_t{5}}) {
        stage(6, i ? 120 : 0, 0);
        const auto beforeApp = inventoryApp;
        const auto beforeInventory = pendingInventory;
        const unsigned before = inventoryMenuCalls;
        SetLastError(0x55112233);
        const unsigned mxcsr = _mm_getcsr(); clobberFp = true;
        menu(inventoryMenu.data(), nullptr, payload, inventoryControls[i].data());
        clobberFp = false;
        Check(inventoryMenuCalls == before && inventoryApp == beforeApp && pendingInventory == beforeInventory &&
              GetLastError() == 0x55112233 && _mm_getcsr() == mxcsr,
              "early menu input retains only its action without issuing commands or changing native data, FP state or LastError");
        InvokeInventoryTail();
        Check(inventoryMenuCalls == before, "native frames do not resume the request before real readiness");
        pendingInventory[0x20] = 1; InvokeInventoryTail(); InvokeInventoryTail();
        Check(inventoryMenuCalls == before + 1 && observedInventoryMenu == inventoryMenu.data() &&
              observedInventoryControl == inventoryControls[i].data() && !observedInventoryPayload &&
              originalInventoryLastError == 0x55112233 && GetLastError() == 0x55112233,
              "a ready frame resolves the live registered control and delegates once with the verified unused payload");
    }
    stage(5, 0, 0); const unsigned stateFiveBefore = inventoryMenuCalls;
    menu(inventoryMenu.data(), nullptr, nullptr, inventoryControls[1].data());
    InvokeInventoryTail();
    Check(inventoryMenuCalls == stateFiveBefore, "state-five capacity loading holds page departure before issuing commands");
    pendingInventory[0x20] = 1; inventoryStarterBusy = true; InvokeInventoryTail();
    Check(inventoryMenuCalls == stateFiveBefore, "the original callback retains a pending request while starter busy");
    inventoryStarterBusy = false; InvokeInventoryTail();
    Check(inventoryMenuCalls == stateFiveBefore + 1, "clearing native callback busy state resumes the deferred action once");
    stage(6, 0, 0); const unsigned prior = inventoryMenuCalls;
    menu(inventoryMenu.data(), nullptr, payload, inventoryControls[3].data());
    Check(inventoryMenuCalls == prior + 1 && observedInventoryPayload == payload,
          "MyRoom bootstrap remains native even while slot capacity is pending");
    stage(4, 120, 0); menu(inventoryMenu.data(), nullptr, payload, inventoryControls[1].data());
    Check(inventoryMenuCalls == prior + 2, "login initialization is never held by a menu request guard");
    stage(6, 120, 2); menu(inventoryMenu.data(), nullptr, payload, inventoryControls[1].data());
    Check(inventoryMenuCalls == prior + 3, "unknown inventory state preserves the original call and payload");
    stage(6, 120, 0); menu(inventoryMenu.data(), nullptr, nullptr, inventoryControls[1].data());
    Put(inventoryApp.data(), 0x4a0, uint32_t{8}); InvokeInventoryTail();
    stage(6, 120, 1); InvokeInventoryTail();
    Check(inventoryMenuCalls == prior + 3, "changing page cancels a pending action instead of replaying it later");
    stage(6, 120, 0); menu(inventoryMenu.data(), nullptr, nullptr, inventoryControls[1].data());
    pendingInventory[0x20] = 1;
    inventoryServiceHead[0] = reinterpret_cast<uintptr_t>(inventoryServiceHead.data()); InvokeInventoryTail();
    inventoryServiceHead[0] = reinterpret_cast<uintptr_t>(inventoryServiceNode.data()); InvokeInventoryTail();
    Check(inventoryMenuCalls == prior + 4, "a temporarily absent menu service resumes only after live registration returns");
    std::array<uint8_t, 0x64> replacementMenu = inventoryMenu;
    std::array<uint8_t, 0x180> replacementRoot{}, replacementControl{};
    stage(6, 120, 0); menu(inventoryMenu.data(), nullptr, nullptr, inventoryControls[1].data());
    Put(replacementMenu.data(), 0x44, replacementRoot.data());
    Put(replacementMenu.data(), 0x4c, replacementControl.data());
    Put(replacementControl.data(), 0x15c, replacementRoot.data());
    replacementControl[0x162] = replacementControl[0x167] = 1;
    inventoryServiceNode[2] = reinterpret_cast<uintptr_t>(replacementMenu.data());
    pendingInventory[0x20] = 1; InvokeInventoryTail();
    Check(inventoryMenuCalls == prior + 5 && observedInventoryMenu == replacementMenu.data() &&
          observedInventoryControl == replacementControl.data(),
          "a rebuilt live menu and root supply fresh controls instead of retained widget pointers");
    inventoryServiceNode[2] = reinterpret_cast<uintptr_t>(inventoryMenu.data());
    stage(6, 120, 0); open(); open(); InvokeInventoryTail();
    Check(inventoryCatalogCalls == 0, "catalog clicks coalesce without commands while inventory is incomplete");
    pendingInventory[0x20] = 1; InvokeInventoryTail(); InvokeInventoryTail();
    Check(inventoryCatalogCalls == 1, "catalog opens exactly once from a ready normal frame");
    stage(6, 120, 0); open(); Put(inventoryApp.data(), 0x4a0, uint32_t{8}); InvokeInventoryTail();
    pendingInventory[0x20] = 1; InvokeInventoryTail();
    Check(inventoryCatalogCalls == 1, "another page cancels a stale catalog intent");
    stage(6, 120, 0); open(); inventoryObject = nullptr; InvokeInventoryTail();
    inventoryObject = pendingInventory.data(); pendingInventory[0x20] = 1; InvokeInventoryTail();
    Check(inventoryCatalogCalls == 1, "inventory owner replacement cancels a pending catalog safely");
    stage(6, 120, 0); menu(inventoryMenu.data(), nullptr, nullptr, inventoryControls[1].data()); open();
    pendingInventory[0x20] = 1; InvokeInventoryTail();
    Check(inventoryMenuCalls == prior + 5 && inventoryCatalogCalls == 2,
          "the latest catalog intent replaces an earlier pending menu choice");
    stage(6, 120, 0); open(); menu(inventoryMenu.data(), nullptr, nullptr, inventoryControls[1].data());
    pendingInventory[0x20] = 1; InvokeInventoryTail();
    Check(inventoryMenuCalls == prior + 6 && inventoryCatalogCalls == 2,
          "the latest native menu choice cancels the earlier pending catalog");
    using Status = int(__cdecl*)(SdgoNativeDataGuardStatusV6*);
    auto status = reinterpret_cast<Status>(GetProcAddress(dll, "SdgoNativeDataGuardGetStatusV6"));
    SdgoNativeDataGuardStatusV6 observed{sizeof(observed), 6};
    Check(status && status(&observed) && observed.deferred_inventory_transition_calls >= 9 &&
          observed.resumed_inventory_transitions == 9 && observed.deferred_inventory_catalog_requests == 5 &&
          observed.resumed_inventory_catalog_requests == 2 && observed.cancelled_inventory_catalog_requests == 3,
          "additive read-only observer reports actual deferred actions, resumes and cancellations");
}
