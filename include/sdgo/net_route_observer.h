#pragma once

// Optional read-only GameTest receipt for net_route.
//
// The production route behavior does not depend on this ABI. Observation is
// active only when the process starts with a valid client GameTest run
// identity. No packet payload, credential, or packet digest is retained.

#include <stddef.h>
#include <stdint.h>

#define SDGO_NET_ROUTE_OBSERVER_VERSION 1u
#define SDGO_NET_ROUTE_OBSERVER_MAX_ROUTES 32u
#define SDGO_NET_ROUTE_SNAPSHOT_EXPORT "SdgoNetRouteGetSnapshotV1"

#pragma pack(push, 8)

typedef struct SdgoNetRouteEndpointV1 {
    // Four IPv4 octets in network display order, followed by a host-order port.
    uint8_t ipv4[4];
    uint16_t port;
    uint16_t reserved;
} SdgoNetRouteEndpointV1;

typedef struct SdgoNetRouteEntryV1 {
    // One-based immutable config order and the Windows SOCK_STREAM/SOCK_DGRAM value.
    uint32_t route_index;
    uint32_t socket_type;
    SdgoNetRouteEndpointV1 logical;
    SdgoNetRouteEndpointV1 physical;
    // Send calls count translated connect/sendto attempts. Byte counts include
    // only successful sendto return values; connect contributes zero bytes.
    uint64_t translated_send_calls;
    uint64_t translated_send_bytes;
    // Receive bytes are the successful recvfrom return, or the copied buffer
    // size for a translated WSAEMSGSIZE datagram.
    uint64_t translated_receive_calls;
    uint64_t translated_receive_bytes;
    // Success requires an exact sockaddr readback after the production
    // WriteSource operation. Failures retain the native return and last error.
    uint64_t source_restore_successes;
    uint64_t source_restore_failures;
    uint32_t reserved[2];
} SdgoNetRouteEntryV1;

typedef struct SdgoNetRouteSnapshotV1 {
    // Callers initialize size/version and pass the same client GameTest
    // run_hash to the export. A rejected call does not alter this structure.
    uint32_t size;
    uint32_t version;
    uint64_t run_hash;
    uint32_t process_id;
    uint32_t route_count;
    uint32_t observation_active;
    uint32_t reserved0;
    uint64_t unmatched_send_calls;
    uint64_t unmatched_receive_calls;
    uint64_t reserved1[2];
    SdgoNetRouteEntryV1 routes[SDGO_NET_ROUTE_OBSERVER_MAX_ROUTES];
} SdgoNetRouteSnapshotV1;

#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(SdgoNetRouteEndpointV1) == 8, "net route endpoint observer ABI");
static_assert(sizeof(SdgoNetRouteEntryV1) == 80, "net route entry observer ABI");
static_assert(offsetof(SdgoNetRouteEntryV1, translated_send_calls) == 24,
              "net route counter observer ABI");
static_assert(sizeof(SdgoNetRouteSnapshotV1) == 2624, "net route snapshot observer ABI");
#endif

typedef int (__cdecl *SdgoNetRouteGetSnapshotV1Fn)(
    uint64_t run_hash, SdgoNetRouteSnapshotV1* output);
