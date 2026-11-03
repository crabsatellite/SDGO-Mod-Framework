#pragma once

#include <windows.h>

#include <cstdint>

namespace friends {
namespace friend_service_lookup {

struct UniqueService
{
    void* service;
    uint32_t matches;
    bool list_valid;
};

// The shipped UI manager owns an intrusive list at +0x44. Dialogs created
// through the unnamed modal wrapper cannot be resolved through the native
// name lookup, so inspect the current manager membership by exact class
// identity. A result is usable only when the list is structurally complete
// and contains exactly one matching live object.
inline UniqueService FindUniqueByVtable(
    void* manager,
    const void* expectedVtable) noexcept
{
    UniqueService result = {};
    if (!manager || !expectedVtable)
        return result;

    __try {
        auto* managerBytes = static_cast<uint8_t*>(manager);
        auto* sentinel =
            *reinterpret_cast<uint8_t**>(managerBytes + 0x44);
        if (!sentinel)
            return result;

        auto* node = *reinterpret_cast<uint8_t**>(sentinel);
        auto* previous = sentinel;
        if (!node ||
            !*reinterpret_cast<uint8_t**>(sentinel + 4))
            return result;

        for (uint32_t count = 0; count < 1024; ++count) {
            if (node == sentinel) {
                if (*reinterpret_cast<uint8_t**>(
                        sentinel + 4) != previous)
                    return result;
                result.list_valid = true;
                if (result.matches != 1)
                    result.service = nullptr;
                return result;
            }

            if (*reinterpret_cast<uint8_t**>(node + 4) !=
                    previous)
                return result;
            auto* next = *reinterpret_cast<uint8_t**>(node);
            if (!next ||
                *reinterpret_cast<uint8_t**>(next + 4) != node)
                return result;

            void* service =
                *reinterpret_cast<void**>(node + 8);
            if (service &&
                *reinterpret_cast<void**>(service) ==
                    expectedVtable) {
                ++result.matches;
                if (result.matches == 1)
                    result.service = service;
                else
                    result.service = nullptr;
            }

            previous = node;
            node = next;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = {};
    }
    return result;
}

} // namespace friend_service_lookup
} // namespace friends
