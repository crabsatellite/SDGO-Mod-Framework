#pragma once

#include "gift_policy.h"
#include "server_epoch.h"

#include "sdgo/friend_unit_gift_protocol.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace sdgo::friend_unit_gift {

struct NativeDatabaseSessions {
    void* member = nullptr;
    void* game = nullptr;
};

class ServerDatabase {
public:
    ServerDatabase() = default;
    ServerDatabase(const ServerDatabase&) = delete;
    ServerDatabase& operator=(const ServerDatabase&) = delete;

    bool Configure(
        std::wstring expected_game_db,
        std::wstring common_db,
        EligibilityPolicy policy);

    bool EnsureOwnedPolicy(
        const NativeDatabaseSessions& sessions,
        std::wstring* error = nullptr) noexcept;

    bool ValidateSessionsReadOnly(
        const NativeDatabaseSessions& sessions,
        std::wstring* error = nullptr) noexcept;

    Reply ExecuteWireRequest(
        const NativeDatabaseSessions& sessions,
        std::uint32_t authenticated_actor,
        const Request& request) noexcept;

private:
    struct Configuration {
        std::wstring expected_game_db;
        std::wstring common_db;
        EligibilityPolicy policy{};
        bool configured = false;
    };

    Configuration Snapshot() const;

    mutable std::mutex configuration_mutex_;
    Configuration configuration_{};
    std::atomic<bool> ready_{false};
    std::atomic<bool> schema_present_{false};
};

} // namespace sdgo::friend_unit_gift
