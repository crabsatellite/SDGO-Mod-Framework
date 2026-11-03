#include "../mods/friend_unit_gift_client/client_runtime_policy.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <type_traits>

namespace {

using namespace sdgo::friend_unit_gift;
using namespace sdgo::friend_unit_gift::client::runtime;

void Check(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
}

template <class T>
void Put(void* pointer, std::size_t offset, T value) {
    std::memcpy(
        static_cast<std::uint8_t*>(pointer) + offset,
        &value,
        sizeof(value));
}

void* g_service = nullptr;
std::uint32_t g_argument = 0;

void __declspec(naked) NativeInit() {
    __asm {
        mov g_service, ecx
        mov eax, dword ptr [esp + 4]
        mov g_argument, eax
        ret 4
    }
}

void __fastcall InitAdapter(
    void* service,
    void*,
    std::uint32_t argument) {
    ForwardUnitDetailInit(
        reinterpret_cast<UnitDetailInit>(&NativeInit),
        service,
        argument);
}

static_assert(
    std::is_same_v<
        decltype(&InitAdapter),
        UnitDetailInitHook>);

} // namespace

int main() {
    Check(
        LocalInventoryEffect(
            Command::offer,
            WireStatus::success) ==
            InventoryEffect::remove_local,
        "a confirmed offer removes one local native unit");
    Check(
        LocalInventoryEffect(
            Command::accept,
            WireStatus::success) ==
            InventoryEffect::add_local &&
            LocalInventoryEffect(
                Command::takeback,
                WireStatus::success) ==
                InventoryEffect::add_local,
        "confirmed accept and takeback add one local native unit");
    Check(
        LocalInventoryEffect(
            Command::return_to_sender,
            WireStatus::success) ==
            InventoryEffect::none,
        "recipient return does not mutate the recipient native inventory");
    Check(
        LocalInventoryEffect(
            Command::takeback,
            WireStatus::returning_waiting_capacity) ==
            InventoryEffect::none &&
            LocalInventoryEffect(
                Command::offer,
                WireStatus::outcome_unknown) ==
                InventoryEffect::none,
        "waiting-capacity and uncertain outcomes cannot trigger "
        "a fabricated native inventory change");

    std::array<std::uint8_t, 0x500> app{};
    std::array<std::uint8_t, 0x200> profile{};
    auto* profilePointer = profile.data();
    AuthenticatedContext context;

    Check(
        !ReadAuthenticatedContext(
            nullptr, &profilePointer, &context),
        "a missing native app rejects the probe");
    Put<std::uint32_t>(app.data(), 0x4A0, 4);
    Put<std::uint32_t>(profile.data(), 0x138, 6228);
    Check(
        !ReadAuthenticatedContext(
            app.data(), &profilePointer, &context) &&
            context.app_state == 4 &&
            context.pilot_id == 6228,
        "a pre-menu app state rejects the probe");
    Put<std::uint32_t>(app.data(), 0x4A0, 5);
    Put<std::uint32_t>(profile.data(), 0x138, 0);
    Check(
        !ReadAuthenticatedContext(
            app.data(), &profilePointer, &context),
        "an unbound pilot rejects the probe");
    profilePointer = nullptr;
    Check(
        !ReadAuthenticatedContext(
            app.data(), &profilePointer, &context),
        "a missing authenticated profile rejects the probe");
    profilePointer = profile.data();
    Put<std::uint32_t>(profile.data(), 0x138, 6228);
    Check(
        ReadAuthenticatedContext(
            app.data(), &profilePointer, &context) &&
            context.app_state == 5 &&
            context.pilot_id == 6228,
        "an authenticated native menu accepts the probe");
    Put<std::uint32_t>(app.data(), 0x4A0, 29);
    Check(
        ReadAuthenticatedContext(
            app.data(), &profilePointer, &context),
        "the final supported native menu state accepts the probe");
    Put<std::uint32_t>(app.data(), 0x4A0, 30);
    Check(
        !ReadAuthenticatedContext(
            app.data(), &profilePointer, &context),
        "a non-menu app state rejects the probe");

    std::uint32_t before = 0;
    std::uint32_t after = 0;
    std::uint8_t service{};
    __asm mov before, esp
    InitAdapter(
        &service, nullptr, 0xA1B2C3D4u);
    __asm mov after, esp
    Check(
        before == after &&
            g_service == &service &&
            g_argument == 0xA1B2C3D4u,
        "the ret-4 UnitDetail init ABI forwards its stack argument "
        "without corrupting the caller stack");

    std::cout << "friend unit gift client runtime policy passed\n";
    return 0;
}
