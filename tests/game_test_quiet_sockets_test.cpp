#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "../mods/game_test_client/quiet_sockets.h"
#include <cstdio>

namespace {
QuietSockets::BindFn detour=nullptr;
QuietSockets::GetHostByNameFn hostDetour=nullptr;
int delegatedHostLookups=0;
hostent* delegatedHostResult=nullptr;
hostent* WSAAPI OriginalGetHostByName(const char* name) {
    ++delegatedHostLookups;
    if (name && !std::strcmp(name,"forced-delegation-failure.invalid")) {
        WSASetLastError(WSAHOST_NOT_FOUND);
        delegatedHostResult=nullptr;
        return nullptr;
    }
    delegatedHostResult=::gethostbyname(name);
    return delegatedHostResult;
}
int __cdecl Hook(SdgoMod*,const char* library,const char* name,void* replacement,void** original) {
    if (std::strcmp(library,"ws2_32.dll"))return 0;
    if (!std::strcmp(name,"bind")) {
        detour=reinterpret_cast<QuietSockets::BindFn>(replacement);
        *original=reinterpret_cast<void*>(&::bind);
        return 1;
    }
    if (!std::strcmp(name,"gethostbyname")) {
        hostDetour=reinterpret_cast<QuietSockets::GetHostByNameFn>(replacement);
        *original=reinterpret_cast<void*>(&OriginalGetHostByName);
        return 1;
    }
    return 0;
}
}
int main() {
    WSADATA data{};if (WSAStartup(MAKEWORD(2,2),&data))return 1;
    SdgoApi api{};api.HookImport=&Hook;
    QuietSockets guard;int failed=0;
    if (!guard.Install(&api,reinterpret_cast<SdgoMod*>(1)) || !detour || !hostDetour)return 1;
    char localName[256]{};
    if (gethostname(localName,static_cast<int>(sizeof(localName))) ||
        !localName[0])return 1;
    char expectedLocalName[256]{};
    strcpy_s(expectedLocalName,localName);
    auto HasAddress=[](hostent* entry,uint32_t expected) {
        if (!entry || entry->h_addrtype!=AF_INET ||
            entry->h_length!=static_cast<short>(sizeof(uint32_t)) ||
            !entry->h_addr_list ||
            !entry->h_addr_list[0])return false;
        uint32_t address=0;
        std::memcpy(&address,entry->h_addr_list[0],sizeof(address));
        return ntohl(address)==expected;
    };
    auto ValidLoopbackHost=[&](hostent* entry) {
        return HasAddress(entry,INADDR_LOOPBACK) &&
            entry->h_name && !std::strcmp(entry->h_name,expectedLocalName) &&
            entry->h_aliases && !entry->h_aliases[0] &&
            !entry->h_addr_list[1];
    };
    hostent* localHost=hostDetour(localName);
    if (!ValidLoopbackHost(localHost))++failed;
    uint32_t localAddress=0;
    if (localHost && localHost->h_addr_list && localHost->h_addr_list[0])
        std::memcpy(&localAddress,localHost->h_addr_list[0],sizeof(localAddress));
    _strupr_s(localName);
    if (!ValidLoopbackHost(hostDetour(localName)))++failed;
    if (!ValidLoopbackHost(hostDetour("")))++failed;
    if (!ValidLoopbackHost(hostDetour(nullptr)))++failed;
    hostent* remote=hostDetour("127.222.0.1");
    if (!HasAddress(remote,0x7FDE0001u) || remote!=delegatedHostResult ||
        delegatedHostLookups!=1)++failed;
    WSASetLastError(0);
    if (hostDetour("forced-delegation-failure.invalid") ||
        WSAGetLastError()!=WSAHOST_NOT_FOUND || delegatedHostLookups!=2)++failed;

    uint32_t expectedBinds=0,expectedRedirected=0,expectedFailed=0;
    auto CallBind=[&](SOCKET handle,const sockaddr* address,int length,bool redirected) {
        ++expectedBinds;
        if (redirected)++expectedRedirected;
        const int result=detour(handle,address,length);
        if (result==SOCKET_ERROR)++expectedFailed;
        return result;
    };

    // Exercise the returned hostent through real loopback UDP traffic.
    SOCKET receiver=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    SOCKET sender=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if (receiver==INVALID_SOCKET || sender==INVALID_SOCKET)return 1;
    const DWORD timeout=1000;
    setsockopt(receiver,SOL_SOCKET,SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    sockaddr_in receiverInput{};
    receiverInput.sin_family=AF_INET;
    receiverInput.sin_addr.s_addr=htonl(INADDR_ANY);
    const auto savedReceiverInput=receiverInput;
    if (CallBind(receiver,reinterpret_cast<const sockaddr*>(&receiverInput),
                 sizeof(receiverInput),true))++failed;
    sockaddr_in receiverAddress{};int receiverSize=sizeof(receiverAddress);
    if (getsockname(receiver,reinterpret_cast<sockaddr*>(&receiverAddress),&receiverSize) ||
        !receiverAddress.sin_port ||
        std::memcmp(&receiverInput,&savedReceiverInput,sizeof(receiverInput)))++failed;
    sockaddr_in senderInput{};
    senderInput.sin_family=AF_INET;
    senderInput.sin_addr.s_addr=htonl(INADDR_ANY);
    const auto savedSenderInput=senderInput;
    if (CallBind(sender,reinterpret_cast<const sockaddr*>(&senderInput),
                 sizeof(senderInput),true))++failed;
    if (std::memcmp(&senderInput,&savedSenderInput,sizeof(senderInput)))++failed;
    sockaddr_in destination{};
    destination.sin_family=AF_INET;
    destination.sin_addr.s_addr=localAddress;
    destination.sin_port=receiverAddress.sin_port;
    const uint32_t sentPayload=0x5344474Fu;
    uint32_t receivedPayload=0;
    sockaddr_in source{};int sourceSize=sizeof(source);
    if (sendto(sender,reinterpret_cast<const char*>(&sentPayload),sizeof(sentPayload),0,
               reinterpret_cast<const sockaddr*>(&destination),sizeof(destination))!=
            static_cast<int>(sizeof(sentPayload)) ||
        recvfrom(receiver,reinterpret_cast<char*>(&receivedPayload),sizeof(receivedPayload),0,
                 reinterpret_cast<sockaddr*>(&source),&sourceSize)!=
            static_cast<int>(sizeof(receivedPayload)) ||
        receivedPayload!=sentPayload ||
        (ntohl(source.sin_addr.s_addr)>>24)!=127)++failed;
    closesocket(sender);
    closesocket(receiver);

    // Preserve the native adjacent-port allocation contract: an occupied port
    // fails normally, while the next free requested port is kept unchanged.
    SOCKET reserved=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if (reserved==INVALID_SOCKET)return 1;
    sockaddr_in reservedInput{};
    reservedInput.sin_family=AF_INET;
    reservedInput.sin_addr.s_addr=htonl(INADDR_ANY);
    if (CallBind(reserved,reinterpret_cast<const sockaddr*>(&reservedInput),
                 sizeof(reservedInput),true))++failed;
    sockaddr_in reservedAddress{};int reservedSize=sizeof(reservedAddress);
    if (getsockname(reserved,reinterpret_cast<sockaddr*>(&reservedAddress),&reservedSize) ||
        !reservedAddress.sin_port)++failed;
    SOCKET conflict=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if (conflict==INVALID_SOCKET)return 1;
    sockaddr_in conflictInput=reservedInput;
    conflictInput.sin_port=reservedAddress.sin_port;
    const auto savedConflictInput=conflictInput;
    if (CallBind(conflict,reinterpret_cast<const sockaddr*>(&conflictInput),
                 sizeof(conflictInput),true)!=SOCKET_ERROR ||
        WSAGetLastError()!=WSAEADDRINUSE ||
        std::memcmp(&conflictInput,&savedConflictInput,sizeof(conflictInput)))++failed;
    closesocket(conflict);
    SOCKET adjacent=INVALID_SOCKET;
    const uint16_t basePort=ntohs(reservedAddress.sin_port);
    for (uint16_t offset=1;offset<=32 && adjacent==INVALID_SOCKET;++offset) {
        const uint16_t candidatePort=static_cast<uint16_t>(basePort+offset);
        if (!candidatePort)continue;
        SOCKET candidate=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        if (candidate==INVALID_SOCKET)return 1;
        sockaddr_in candidateInput=reservedInput;
        candidateInput.sin_port=htons(candidatePort);
        const auto savedCandidateInput=candidateInput;
        const int result=CallBind(candidate,reinterpret_cast<const sockaddr*>(&candidateInput),
                                  sizeof(candidateInput),true);
        if (std::memcmp(&candidateInput,&savedCandidateInput,sizeof(candidateInput)))++failed;
        if (!result) {
            sockaddr_in actual{};int actualSize=sizeof(actual);
            if (getsockname(candidate,reinterpret_cast<sockaddr*>(&actual),&actualSize) ||
                ntohs(actual.sin_port)!=candidatePort ||
                (ntohl(actual.sin_addr.s_addr)>>24)!=127)++failed;
            adjacent=candidate;
        } else {
            if (WSAGetLastError()!=WSAEADDRINUSE)++failed;
            closesocket(candidate);
        }
    }
    if (adjacent==INVALID_SOCKET)++failed;
    else closesocket(adjacent);
    closesocket(reserved);

    // These are real Windows UDP sockets. Public/wildcard addresses never reach
    // the real bind; no game, database or desktop is opened by this test.
    const uint32_t addresses[]={INADDR_ANY,0xC0000201u,0x7F00005Au};
    for (uint32_t requested:addresses) {
        SOCKET handle=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        sockaddr_in input{};input.sin_family=AF_INET;input.sin_addr.s_addr=htonl(requested);
        const auto saved=input;
        if (handle==INVALID_SOCKET ||
            CallBind(handle,reinterpret_cast<sockaddr*>(&input),sizeof(input),
                     (requested>>24)!=127))++failed;
        sockaddr_in actual{};int size=sizeof(actual);
        if (getsockname(handle,reinterpret_cast<sockaddr*>(&actual),&size) ||
            (ntohl(actual.sin_addr.s_addr)>>24)!=127 || !actual.sin_port || std::memcmp(&saved,&input,sizeof(input)))++failed;
        closesocket(handle);
    }
    if (CallBind(INVALID_SOCKET,nullptr,0,false)!=SOCKET_ERROR ||
        WSAGetLastError()!=WSAEFAULT)++failed;
    sockaddr_in input{};input.sin_family=AF_INET;
    if (CallBind(INVALID_SOCKET,reinterpret_cast<sockaddr*>(&input),sizeof(input),true)!=SOCKET_ERROR ||
        WSAGetLastError()!=WSAENOTSOCK)++failed;
    const auto observed=guard.State();
    if (!observed.active || observed.binds!=expectedBinds ||
        observed.redirected!=expectedRedirected ||
        observed.failed!=expectedFailed || observed.escaped ||
        observed.host_lookups!=6 || observed.host_redirected!=4 ||
        observed.host_delegated!=2)++failed;
    WSACleanup();std::printf("Native loopback bind guard: %d failures\n",failed);return failed ? 1:0;
}
