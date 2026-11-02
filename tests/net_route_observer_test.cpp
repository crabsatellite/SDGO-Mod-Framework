// Offline actual-core test for the production net_route observer export.
// No game, account, server, database, public address, or tunnel is used.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sdgo/game_test_page_observer.h"
#include "sdgo/net_route_observer.h"
#include "sdgo/sdgo_api.h"
#include "named_winsock_fixture.h"

namespace
{
    constexpr char kRunId[] = "12345678-1234-1234-1234-123456789abc";
    constexpr char kRole[] = "client";
    int g_failures = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++g_failures;
    }

    sockaddr_in IPv4(const char* ip, unsigned short port)
    {
        sockaddr_in result = {};
        result.sin_family = AF_INET;
        InetPtonA(AF_INET, ip, &result.sin_addr);
        result.sin_port = htons(port);
        return result;
    }

    bool EndpointEquals(const SdgoNetRouteEndpointV1& endpoint, const char* ip, unsigned short port)
    {
        in_addr address = {};
        return InetPtonA(AF_INET, ip, &address) == 1 &&
               std::memcmp(endpoint.ipv4, &address, sizeof(endpoint.ipv4)) == 0 &&
               endpoint.port == port && endpoint.reserved == 0;
    }

    unsigned short BoundPort(SOCKET socket)
    {
        sockaddr_in address = {};
        int length = sizeof(address);
        return getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0
            ? ntohs(address.sin_port) : 0;
    }

    SOCKET Bind(int type, const char* ip, unsigned short port = 0)
    {
        SOCKET result = socket(AF_INET, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
        if (result == INVALID_SOCKET) return result;
        BOOL exclusive = TRUE;
        setsockopt(result, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in address = IPv4(ip, port);
        if (bind(result, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) return result;
        closesocket(result);
        return INVALID_SOCKET;
    }

    void ReceiveTimeout(SOCKET socket)
    {
        DWORD milliseconds = 2000;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
    }

    std::wstring Root()
    {
        wchar_t path[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) *slash = 0;
        return path;
    }

    bool Setting(const std::wstring& path, const wchar_t* section,
                 const wchar_t* key, const std::wstring& value)
    {
        return WritePrivateProfileStringW(section, key, value.c_str(), path.c_str()) != FALSE;
    }

    bool Configure(const std::wstring& root, unsigned short logicalTcp,
                   unsigned short physicalTcp, unsigned short logicalUdp,
                   unsigned short physicalUdp)
    {
        const std::wstring manifest = root + L"\\sdgo\\mods\\net_route\\mod.ini";
        const std::wstring global = root + L"\\sdgo\\sdgo.ini";
        wchar_t route[160] = {};
        bool ok = Setting(manifest, L"mod", L"hosts", L"*") &&
                  Setting(manifest, L"mod", L"enabled", L"1") &&
                  Setting(manifest, L"settings", L"LoginIPv4", L"") &&
                  Setting(manifest, L"settings", L"LoginPort", L"") &&
                  Setting(manifest, L"settings", L"RouteCount", L"2") &&
                  Setting(manifest, L"settings", L"Trace", L"0");
        swprintf_s(route, L"tcp,127.0.0.2,%u,127.0.0.1,%u", logicalTcp, physicalTcp);
        ok = Setting(manifest, L"settings", L"Route1", route) && ok;
        swprintf_s(route, L"udp,127.0.0.2,%u,127.0.0.1,%u", logicalUdp, physicalUdp);
        ok = Setting(manifest, L"settings", L"Route2", route) && ok;
        DeleteFileW(global.c_str());
        ok = Setting(global, L"core", L"log_level", L"0") && ok;
        return ok;
    }

    bool StartCore(const std::wstring& root)
    {
        HMODULE core = LoadLibraryW((root + L"\\sdgo\\core.dll").c_str());
        auto init = core ? reinterpret_cast<SdgoCoreInitFn>(GetProcAddress(core, "SdgoCoreInit")) : nullptr;
        Check(init != nullptr, "real core export is available");
        if (!init) return false;
        SdgoBoot boot = {sizeof(SdgoBoot), "net_route_observer_test", GetModuleHandleW(nullptr)};
        Check(init(&boot) != 0, "real core loads the production net_route DLL");
        return g_failures == 0;
    }

    uint64_t RunHash()
    {
        return sdgo::game_test_page_observer::ClientRunHash(
            kRunId, sizeof(kRunId) - 1, kRole, sizeof(kRole) - 1);
    }

    bool SameBytes(const SdgoNetRouteSnapshotV1& first,
                   const SdgoNetRouteSnapshotV1& second)
    {
        return std::memcmp(&first, &second, sizeof(first)) == 0;
    }

    void BadParameterChecks(SdgoNetRouteGetSnapshotV1Fn get, uint64_t runHash)
    {
        SdgoNetRouteSnapshotV1 value = {};
        std::memset(&value, 0xA5, sizeof(value));
        value.size = sizeof(value);
        value.version = SDGO_NET_ROUTE_OBSERVER_VERSION;
        const auto original = value;
        Check(get(runHash, nullptr) == 0, "null snapshot is rejected");
        Check(get(0, &value) == 0 && SameBytes(value, original),
              "zero run hash is rejected without modifying output");
        Check(get(runHash ^ 1, &value) == 0 && SameBytes(value, original),
              "wrong run hash is rejected without modifying output");
        value = original;
        value.size = sizeof(value) - 1;
        const auto shortOriginal = value;
        Check(get(runHash, &value) == 0 && SameBytes(value, shortOriginal),
              "short snapshot is rejected without modifying output");
        value = original;
        value.version = SDGO_NET_ROUTE_OBSERVER_VERSION + 1;
        const auto versionOriginal = value;
        Check(get(runHash, &value) == 0 && SameBytes(value, versionOriginal),
              "unknown snapshot version is rejected without modifying output");
    }

    SdgoNetRouteSnapshotV1 Snapshot(SdgoNetRouteGetSnapshotV1Fn get, uint64_t runHash)
    {
        SdgoNetRouteSnapshotV1 result = {};
        result.size = sizeof(result);
        result.version = SDGO_NET_ROUTE_OBSERVER_VERSION;
        Check(get(runHash, &result) == 1, "same-run snapshot is readable on the main thread");
        return result;
    }
}

int wmain(int argc, wchar_t** argv)
{
    const bool inactive = argc > 1 && wcscmp(argv[1], L"inactive") == 0;
    Check(NameClientWinsockImports(), "fixture models GOnline named Winsock imports");
    WSADATA winsock = {};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        std::puts("FAIL WSAStartup");
        return 2;
    }

    SOCKET physicalTcp = Bind(SOCK_STREAM, "127.0.0.1");
    SOCKET logicalTcpGuard = Bind(SOCK_STREAM, "127.0.0.2");
    SOCKET physicalUdp = Bind(SOCK_DGRAM, "127.0.0.1");
    SOCKET logicalUdpGuard = Bind(SOCK_DGRAM, "127.0.0.2");
    SOCKET unmatchedUdp = Bind(SOCK_DGRAM, "127.0.0.1");
    if (physicalTcp == INVALID_SOCKET || logicalTcpGuard == INVALID_SOCKET ||
        physicalUdp == INVALID_SOCKET || logicalUdpGuard == INVALID_SOCKET ||
        unmatchedUdp == INVALID_SOCKET) {
        std::printf("FAIL loopback fixture bind (Winsock %d)\n", WSAGetLastError());
        return 2;
    }
    ReceiveTimeout(physicalUdp);
    ReceiveTimeout(unmatchedUdp);
    Check(listen(physicalTcp, 1) == 0, "physical TCP fixture listens");

    const unsigned short physicalTcpPort = BoundPort(physicalTcp);
    const unsigned short logicalTcpPort = BoundPort(logicalTcpGuard);
    const unsigned short physicalUdpPort = BoundPort(physicalUdp);
    const unsigned short logicalUdpPort = BoundPort(logicalUdpGuard);
    const unsigned short unmatchedUdpPort = BoundPort(unmatchedUdp);
    Check(physicalTcpPort && logicalTcpPort && physicalUdpPort &&
              logicalUdpPort && unmatchedUdpPort,
          "loopback fixture endpoints have concrete ports");

    const std::wstring root = Root();
    Check(Configure(root, logicalTcpPort, physicalTcpPort, logicalUdpPort, physicalUdpPort),
          "fixture writes a Trace=0 exact route profile");
    if (inactive) {
        SetEnvironmentVariableA("SDGO_GAME_TEST_RUN", nullptr);
        SetEnvironmentVariableA("SDGO_GAME_TEST_ROLE", nullptr);
    } else {
        SetEnvironmentVariableA("SDGO_GAME_TEST_RUN", kRunId);
        SetEnvironmentVariableA("SDGO_GAME_TEST_ROLE", kRole);
    }
    if (g_failures == 0) StartCore(root);

    HMODULE routeModule = GetModuleHandleW(L"net_route.dll");
    auto get = routeModule ? reinterpret_cast<SdgoNetRouteGetSnapshotV1Fn>(
        GetProcAddress(routeModule, SDGO_NET_ROUTE_SNAPSHOT_EXPORT)) : nullptr;
    Check(get != nullptr, "production DLL exports the frozen V1 observer");
    const uint64_t runHash = RunHash();

    if (get && inactive) {
        SdgoNetRouteSnapshotV1 value = {};
        std::memset(&value, 0x5A, sizeof(value));
        value.size = sizeof(value);
        value.version = SDGO_NET_ROUTE_OBSERVER_VERSION;
        const auto original = value;
        Check(get(runHash, &value) == 0 && SameBytes(value, original),
              "ordinary process without GameTest identity exposes no snapshot or output changes");
    } else if (get) {
        BadParameterChecks(get, runHash);
        const auto before = Snapshot(get, runHash);
        Check(before.size == sizeof(before) &&
                  before.version == SDGO_NET_ROUTE_OBSERVER_VERSION &&
                  before.run_hash == runHash &&
                  before.process_id == GetCurrentProcessId() &&
                  before.observation_active == 1 &&
                  before.route_count == 2,
              "snapshot is bound to this process and exact GameTest run");
        Check(before.routes[0].route_index == 1 &&
                  before.routes[0].socket_type == SOCK_STREAM &&
                  EndpointEquals(before.routes[0].logical, "127.0.0.2", logicalTcpPort) &&
                  EndpointEquals(before.routes[0].physical, "127.0.0.1", physicalTcpPort),
              "TCP route exposes immutable logical and physical endpoints");
        Check(before.routes[1].route_index == 2 &&
                  before.routes[1].socket_type == SOCK_DGRAM &&
                  EndpointEquals(before.routes[1].logical, "127.0.0.2", logicalUdpPort) &&
                  EndpointEquals(before.routes[1].physical, "127.0.0.1", physicalUdpPort),
              "UDP route exposes immutable logical and physical endpoints");

        SOCKET tcpClient = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in logicalTcp = IPv4("127.0.0.2", logicalTcpPort);
        const int connectResult = connect(tcpClient, reinterpret_cast<const sockaddr*>(&logicalTcp),
                                          sizeof(logicalTcp));
        Check(connectResult == 0, "observed connect preserves the successful native return");
        SOCKET tcpPeer = accept(physicalTcp, nullptr, nullptr);
        Check(tcpPeer != INVALID_SOCKET, "translated TCP connect reaches the physical listener");
        if (tcpPeer != INVALID_SOCKET) closesocket(tcpPeer);
        closesocket(tcpClient);
        closesocket(physicalTcp);
        physicalTcp = INVALID_SOCKET;
        tcpClient = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        WSASetLastError(0);
        const int failedConnect = connect(tcpClient, reinterpret_cast<const sockaddr*>(&logicalTcp),
                                          sizeof(logicalTcp));
        const int failedConnectError = WSAGetLastError();
        Check(failedConnect == SOCKET_ERROR && failedConnectError == WSAECONNREFUSED,
              "observed failed connect preserves SOCKET_ERROR and WSAECONNREFUSED");

        SOCKET udpClient = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in clientBind = IPv4("127.0.0.1", 0);
        Check(bind(udpClient, reinterpret_cast<const sockaddr*>(&clientBind), sizeof(clientBind)) == 0,
              "UDP client binds locally");
        ReceiveTimeout(udpClient);
        const char request[] = "observer-request";
        sockaddr_in logicalUdp = IPv4("127.0.0.2", logicalUdpPort);
        const int sendResult = sendto(udpClient, request, sizeof(request), 0,
                                      reinterpret_cast<const sockaddr*>(&logicalUdp), sizeof(logicalUdp));
        Check(sendResult == sizeof(request), "observed sendto preserves the successful native return");
        std::vector<char> oversized(70000, 0x2A);
        WSASetLastError(0);
        const int oversizedResult = sendto(udpClient, oversized.data(),
                                           static_cast<int>(oversized.size()), 0,
                                           reinterpret_cast<const sockaddr*>(&logicalUdp), sizeof(logicalUdp));
        const int oversizedError = WSAGetLastError();
        Check(oversizedResult == SOCKET_ERROR && oversizedError == WSAEMSGSIZE,
              "observed oversized send preserves SOCKET_ERROR and WSAEMSGSIZE");
        char buffer[64] = {};
        sockaddr_in clientAddress = {};
        int clientLength = sizeof(clientAddress);
        const int requestCount = recvfrom(physicalUdp, buffer, sizeof(buffer), 0,
                                          reinterpret_cast<sockaddr*>(&clientAddress), &clientLength);
        Check(requestCount == sizeof(request), "translated UDP request reaches the physical endpoint");

        const char reply[] = "observer-reply";
        Check(sendto(physicalUdp, reply, sizeof(reply), 0,
                     reinterpret_cast<const sockaddr*>(&clientAddress), clientLength) == sizeof(reply),
              "physical UDP endpoint replies");
        sockaddr_in reported = {};
        int reportedLength = sizeof(reported);
        const int replyCount = recvfrom(udpClient, buffer, sizeof(buffer), 0,
                                        reinterpret_cast<sockaddr*>(&reported), &reportedLength);
        Check(replyCount == sizeof(reply) &&
                  reported.sin_addr.s_addr == logicalUdp.sin_addr.s_addr &&
                  reported.sin_port == logicalUdp.sin_port,
              "observed recvfrom preserves the successful return and restores the logical source");
        const char truncated[] = "observer-truncated-reply";
        Check(sendto(physicalUdp, truncated, sizeof(truncated), 0,
                     reinterpret_cast<const sockaddr*>(&clientAddress), clientLength) == sizeof(truncated),
              "physical UDP endpoint sends a truncation fixture");
        char shortBuffer[4] = {};
        std::memset(&reported, 0xA5, sizeof(reported));
        reportedLength = sizeof(reported);
        WSASetLastError(0);
        const int truncatedCount = recvfrom(udpClient, shortBuffer, sizeof(shortBuffer), 0,
                                            reinterpret_cast<sockaddr*>(&reported), &reportedLength);
        const int truncatedError = WSAGetLastError();
        Check(truncatedCount == SOCKET_ERROR && truncatedError == WSAEMSGSIZE &&
                  reported.sin_addr.s_addr == logicalUdp.sin_addr.s_addr &&
                  reported.sin_port == logicalUdp.sin_port,
              "observed truncation preserves WSAEMSGSIZE and restores the logical source");

        const char unmatched[] = "unmatched";
        sockaddr_in unmatchedDestination = IPv4("127.0.0.1", unmatchedUdpPort);
        Check(sendto(udpClient, unmatched, sizeof(unmatched), 0,
                     reinterpret_cast<const sockaddr*>(&unmatchedDestination),
                     sizeof(unmatchedDestination)) == sizeof(unmatched),
              "unmatched UDP send keeps the native path");
        sockaddr_in unmatchedClient = {};
        int unmatchedClientLength = sizeof(unmatchedClient);
        Check(recvfrom(unmatchedUdp, buffer, sizeof(buffer), 0,
                       reinterpret_cast<sockaddr*>(&unmatchedClient),
                       &unmatchedClientLength) == sizeof(unmatched),
              "unmatched listener receives unchanged data");
        Check(sendto(unmatchedUdp, unmatched, sizeof(unmatched), 0,
                     reinterpret_cast<const sockaddr*>(&unmatchedClient),
                     unmatchedClientLength) == sizeof(unmatched),
              "unmatched physical source replies");
        reportedLength = sizeof(reported);
        Check(recvfrom(udpClient, buffer, sizeof(buffer), 0,
                       reinterpret_cast<sockaddr*>(&reported), &reportedLength) == sizeof(unmatched) &&
                  reported.sin_port == htons(unmatchedUdpPort),
              "unmatched receive source remains physical");

        const auto after = Snapshot(get, runHash);
        Check(after.routes[0].translated_send_calls == 2 &&
                  after.routes[0].translated_send_bytes == 0,
              "successful and failed TCP translations are counted without inventing payload bytes");
        Check(after.routes[1].translated_send_calls == 2 &&
                  after.routes[1].translated_send_bytes == sizeof(request),
              "UDP translated send records only the actual successful bytes with Trace=0");
        Check(after.routes[1].translated_receive_calls == 2 &&
                  after.routes[1].translated_receive_bytes == sizeof(reply) + sizeof(shortBuffer),
              "UDP receive counts include the actual truncated buffer bytes with Trace=0");
        Check(after.routes[1].source_restore_successes == 2 &&
                  after.routes[1].source_restore_failures == 0,
              "source restoration is counted only after exact sockaddr readback");
        Check(after.unmatched_send_calls == 4 && after.unmatched_receive_calls == 3,
              "unmatched send and receive paths are counted without changing routing");

        closesocket(tcpClient);
        closesocket(udpClient);
    }

    closesocket(unmatchedUdp);
    closesocket(logicalUdpGuard);
    closesocket(physicalUdp);
    closesocket(logicalTcpGuard);
    if (physicalTcp != INVALID_SOCKET) closesocket(physicalTcp);
    WSACleanup();
    std::puts(g_failures ? "NET ROUTE OBSERVER CHECKS FAILED"
                         : "ALL NET ROUTE OBSERVER CHECKS PASSED");
    return g_failures ? 1 : 0;
}
