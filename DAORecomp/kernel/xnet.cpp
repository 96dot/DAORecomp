// xnet: offline XNet / Winsock stack. Startup succeeds and every socket call fails with
// WSAENETDOWN, so the game sees networking as unavailable. WSA events are real kernel
// events, so the game's network thread blocks in WSAWaitForMultipleEvents instead of spinning.
#include "stdafx.h"
#include "function.h"
#include "thread.h"

#include <random>

namespace dao::imp
{
    using namespace dao::kernel;

    // Winsock error codes. Named k* so they don't collide with winsock.h macros.
    constexpr uint32_t kWsaEnetdown = 10050;
    constexpr uint32_t kWsaInvalidParameter = 87;
    constexpr uint32_t kWsaWaitFailed = 0xFFFFFFFF;
    constexpr uint32_t kWsaWaitEvent0 = 0;
    constexpr uint32_t kWsaWaitIoCompletion = 0xC0;
    constexpr uint32_t kWsaWaitTimeout = 0x102;
    constexpr uint32_t kWsaMaxEvents = 64;

    constexpr int32_t kSocketError = -1;
    constexpr uint32_t kInvalidSocket = 0xFFFFFFFF;
    constexpr uint32_t kInaddrNone = 0xFFFFFFFF;
    constexpr uint32_t kWaitInfinite = 0xFFFFFFFF;

    constexpr uint32_t kXnAddrNone = 0x00000001;      // XNET_GET_XNADDR_NONE
    constexpr uint32_t kConnectStatusIdle = 0x00000000;

    // Per-thread WSA last error.
    static thread_local uint32_t t_wsaLastError = 0;

    static void SetWsaError(uint32_t error)
    {
        t_wsaLastError = error;
    }

    // Returns true for the first few uses of an import, so logs show it was reached without spamming loops.
    static bool ShouldLog(const char* name)
    {
        static std::mutex mutex;
        static std::unordered_map<std::string, uint32_t> counts;
        std::lock_guard lock(mutex);
        return ++counts[name] <= 3;
    }

    // ---------------------------------------------------------------- structs

    struct X_WSADATA
    {
        u16 version;
        u16 versionHigh;
        char description[256 + 1];
        char systemStatus[128 + 1];
        u16 maxSockets;
        u16 maxUdpDg;
        u32 vendorInfoPtr;
    };
    static_assert(sizeof(X_WSADATA) == 0x190);

    struct X_XNADDR
    {
        u32 ina;
        u32 inaOnline;
        u16 portOnline;
        uint8_t enetAddress[6];
        uint8_t onlineIdentity[20];
    };
    static_assert(sizeof(X_XNADDR) == 0x24);

    // ---------------------------------------------------------------- helpers

    // Socket calls: every socket fails, so report WSAENETDOWN.
    static int32_t SocketError(const char* name)
    {
        if (ShouldLog(name))
            LOGI("{} -> WSAENETDOWN (networking offline)", name);
        SetWsaError(kWsaEnetdown);
        return kSocketError;
    }

    static uint32_t InvalidSocket(const char* name)
    {
        if (ShouldLog(name))
            LOGI("{} -> WSAENETDOWN (networking offline)", name);
        SetWsaError(kWsaEnetdown);
        return kInvalidSocket;
    }

    // XNet calls that return an error code directly.
    static int32_t XNetError(const char* name)
    {
        if (ShouldLog(name))
            LOGI("{} -> WSAENETDOWN (networking offline)", name);
        SetWsaError(kWsaEnetdown);
        return int32_t(kWsaEnetdown);
    }

    // Signals an event the game passed for an async call that is failed now, so no waiter hangs.
    static void SignalEventIfPresent(uint32_t handle)
    {
        if (!handle)
            return;
        if (auto event = LookupHandle<Event>(handle, ObjectType::Event))
            event->Set();
    }

    static void FillRandom(uint8_t* dst, uint32_t length)
    {
        if (!dst || !length)
            return;
        std::random_device rd;
        for (uint32_t i = 0; i < length; i += 4)
        {
            const uint32_t word = rd();
            memcpy(dst + i, &word, std::min<uint32_t>(4, length - i));
        }
    }

    // Dotted-quad IPv4 parser. Returns the address with the first octet in the high byte,
    // which is the big-endian value the guest reads.
    static uint32_t ParseIPv4(const char* s)
    {
        uint32_t value = 0;
        for (int part = 0; part < 4; part++)
        {
            uint32_t octet = 0;
            int digits = 0;
            while (*s >= '0' && *s <= '9')
            {
                octet = octet * 10 + uint32_t(*s - '0');
                if (octet > 255)
                    return kInaddrNone;
                s++;
                digits++;
            }
            if (!digits)
                return kInaddrNone;
            value = (value << 8) | octet;
            if (part < 3)
            {
                if (*s != '.')
                    return kInaddrNone;
                s++;
            }
        }
        return *s ? kInaddrNone : value;
    }

    // ---------------------------------------------------------------- Winsock core

    int32_t NetDll_WSAStartup(uint32_t caller, uint32_t version, X_WSADATA* data)
    {
        if (ShouldLog("NetDll_WSAStartup"))
            LOGI("NetDll_WSAStartup(version {:#x}) -> offline", version);
        if (data)
        {
            memset(data, 0, sizeof(*data));
            data->version = uint16_t(0x0202);
            data->versionHigh = uint16_t(0x0202);
        }
        return 0;
    }

    int32_t NetDll_WSACleanup(uint32_t caller)
    {
        return 0;
    }

    int32_t NetDll_WSAGetLastError()
    {
        return int32_t(t_wsaLastError);
    }

    // ---------------------------------------------------------------- WSA events

    uint32_t NetDll_WSACreateEvent()
    {
        auto event = std::make_shared<Event>(true, false); // manual-reset, initially non-signaled
        if (!AttachGuestHeader(event, sizeof(X_KEVENT)))
            return 0; // (WSA_INVALID_EVENT)
        return CreateHandle(event);
    }

    int32_t NetDll_WSACloseEvent(uint32_t event)
    {
        if (!LookupHandle<Event>(event, ObjectType::Event) || !CloseHandle(event))
        {
            SetWsaError(X_ERROR_INVALID_HANDLE);
            return 0;
        }
        return 1;
    }

    int32_t NetDll_WSASetEvent(uint32_t event)
    {
        auto e = LookupHandle<Event>(event, ObjectType::Event);
        if (!e)
        {
            SetWsaError(X_ERROR_INVALID_HANDLE);
            return 0;
        }
        e->Set();
        return 1;
    }

    int32_t NetDll_WSAResetEvent(uint32_t event)
    {
        auto e = LookupHandle<Event>(event, ObjectType::Event);
        if (!e)
        {
            SetWsaError(X_ERROR_INVALID_HANDLE);
            return 0;
        }
        e->Reset();
        return 1;
    }

    // Blocks in the kernel dispatcher. Timeout 0xFFFFFFFF is infinite; otherwise milliseconds.
    uint32_t NetDll_WSAWaitForMultipleEvents(uint32_t count, u32* events, uint32_t waitAll, uint32_t timeout, uint32_t alertable)
    {
        if (ShouldLog("NetDll_WSAWaitForMultipleEvents"))
            LOGI("NetDll_WSAWaitForMultipleEvents(count {}, waitAll {}, timeout {:#x}, alertable {})", count, waitAll, timeout, alertable);

        if (!events || !count || count > kWsaMaxEvents)
        {
            SetWsaError(kWsaInvalidParameter);
            return kWsaWaitFailed;
        }

        std::vector<ObjectRef> objects;
        objects.reserve(count);
        for (uint32_t i = 0; i < count; i++)
        {
            auto event = LookupHandle<Event>(uint32_t(events[i]), ObjectType::Event);
            if (!event)
            {
                SetWsaError(X_ERROR_INVALID_HANDLE);
                return kWsaWaitFailed;
            }
            objects.push_back(std::move(event));
        }

        // Kernel timeouts are 100 ns units; negative means relative.
        const bool infinite = timeout == kWaitInfinite;
        int64_t ticks = infinite ? 0 : -int64_t(timeout) * 10000;
        const uint32_t status = WaitMultiple(objects, waitAll != 0, infinite ? nullptr : &ticks, alertable != 0);

        if (status == X_STATUS_TIMEOUT)
            return kWsaWaitTimeout;
        if (status == X_STATUS_USER_APC)
            return kWsaWaitIoCompletion;
        if (status < count)
            return kWsaWaitEvent0 + status;

        SetWsaError(X_ERROR_INVALID_HANDLE);
        return kWsaWaitFailed;
    }

    int32_t NetDll_WSAGetOverlappedResult(uint32_t caller, uint32_t socket, uint32_t overlapped, u32* transferred, uint32_t wait, u32* flags)
    {
        if (transferred)
            *transferred = 0;
        if (flags)
            *flags = 0;
        if (ShouldLog("NetDll_WSAGetOverlappedResult"))
            LOGI("NetDll_WSAGetOverlappedResult -> WSAENETDOWN (networking offline)");
        SetWsaError(kWsaEnetdown);
        return 0; // FALSE
    }

    int32_t NetDll_WSARecv(uint32_t caller, uint32_t socket, uint32_t buffers, uint32_t bufferCount, u32* bytesReceived, u32* flags, uint32_t overlapped, uint32_t completionRoutine)
    {
        if (bytesReceived)
            *bytesReceived = 0;
        if (flags)
            *flags = 0;
        return SocketError("NetDll_WSARecv");
    }

    int32_t NetDll_WSARecvFrom(uint32_t caller, uint32_t socket, uint32_t buffers, uint32_t bufferCount, u32* bytesReceived, u32* flags, uint32_t from, u32* fromLength, uint32_t overlapped, uint32_t completionRoutine)
    {
        if (bytesReceived)
            *bytesReceived = 0;
        if (flags)
            *flags = 0;
        return SocketError("NetDll_WSARecvFrom");
    }

    // ---------------------------------------------------------------- XNet

    int32_t NetDll_XNetStartup(uint32_t caller, uint32_t params)
    {
        if (ShouldLog("NetDll_XNetStartup"))
            LOGI("NetDll_XNetStartup -> offline");
        return 0;
    }

    int32_t NetDll_XNetCleanup(uint32_t caller, uint32_t params)
    {
        return 0;
    }

    int32_t NetDll_XNetRandom(uint32_t caller, uint8_t* buffer, uint32_t length)
    {
        FillRandom(buffer, length);
        return 0;
    }

    uint32_t NetDll_XNetGetEthernetLinkStatus(uint32_t caller)
    {
        return 0; // no link
    }

    uint32_t NetDll_XNetGetTitleXnAddr(uint32_t caller, X_XNADDR* addr)
    {
        if (addr)
            memset(addr, 0, sizeof(*addr));
        if (ShouldLog("NetDll_XNetGetTitleXnAddr"))
            LOGI("NetDll_XNetGetTitleXnAddr -> XNET_GET_XNADDR_NONE");
        return kXnAddrNone;
    }

    uint32_t NetDll_XNetGetConnectStatus(uint32_t caller, uint32_t ina)
    {
        return kConnectStatusIdle;
    }

    int32_t NetDll_XNetConnect(uint32_t caller, uint32_t ina)
    {
        return XNetError("NetDll_XNetConnect");
    }

    int32_t NetDll_XNetCreateKey(uint32_t caller, uint8_t* keyId, uint8_t* exchangeKey)
    {
        FillRandom(keyId, 8);
        FillRandom(exchangeKey, 16);
        return 0;
    }

    int32_t NetDll_XNetRegisterKey(uint32_t caller, uint8_t* keyId, uint8_t* exchangeKey)
    {
        return 0;
    }

    int32_t NetDll_XNetUnregisterKey(uint32_t caller, uint8_t* keyId)
    {
        return 0;
    }

    int32_t NetDll_XNetDnsLookup(uint32_t caller, uint32_t host, uint32_t eventHandle, u32* dnsOut)
    {
        if (dnsOut)
            *dnsOut = 0;
        SignalEventIfPresent(eventHandle);
        return XNetError("NetDll_XNetDnsLookup");
    }

    int32_t NetDll_XNetDnsRelease(uint32_t caller, uint32_t dns)
    {
        return 0;
    }

    uint32_t NetDll_XNetInAddrToString(uint32_t caller, uint32_t inAddr, char* out, uint32_t maxLength)
    {
        if (!out || !maxLength)
            return 0;
        const std::string text = fmt::format("{}.{}.{}.{}", inAddr >> 24, (inAddr >> 16) & 0xFF, (inAddr >> 8) & 0xFF, inAddr & 0xFF);
        const size_t length = std::min<size_t>(text.size(), maxLength - 1);
        memcpy(out, text.data(), length);
        out[length] = '\0';
        return 0;
    }

    int32_t NetDll_XNetServerToInAddr(uint32_t caller, uint32_t ina, uint32_t serviceId, u32* inAddrOut)
    {
        if (inAddrOut)
            *inAddrOut = 0;
        return XNetError("NetDll_XNetServerToInAddr");
    }

    int32_t NetDll_XNetQosListen(uint32_t caller, uint32_t id, uint32_t data, uint32_t dataSize, uint32_t unknown, uint32_t flags)
    {
        return XNetError("NetDll_XNetQosListen");
    }

    // 12 arguments after the caller; the last two are hEvent and the XNQOS** output.
    int32_t NetDll_XNetQosLookup(uint32_t caller, uint32_t cxna, uint32_t apxna, uint32_t apxkid, uint32_t apxkey,
        uint32_t cina, uint32_t aina, uint32_t serviceIds, uint32_t cPorts, uint32_t aPorts,
        uint32_t flags, uint32_t eventHandle, u32* qosOut)
    {
        if (qosOut)
            *qosOut = 0;
        SignalEventIfPresent(eventHandle);
        return XNetError("NetDll_XNetQosLookup");
    }

    int32_t NetDll_XNetQosRelease(uint32_t caller, uint32_t qos)
    {
        return 0;
    }

    int32_t NetDll_XNetQosServiceLookup(uint32_t caller, uint32_t flags, uint32_t eventHandle, u32* qosOut)
    {
        if (qosOut)
            *qosOut = 0;
        SignalEventIfPresent(eventHandle);
        return XNetError("NetDll_XNetQosServiceLookup");
    }

    // ---------------------------------------------------------------- sockets

    uint32_t NetDll_socket(uint32_t caller, uint32_t af, uint32_t type, uint32_t protocol)
    {
        return InvalidSocket("NetDll_socket");
    }

    int32_t NetDll_closesocket(uint32_t caller, uint32_t socket)
    {
        return SocketError("NetDll_closesocket");
    }

    int32_t NetDll_bind(uint32_t caller, uint32_t socket, uint32_t name, uint32_t namelen)
    {
        return SocketError("NetDll_bind");
    }

    int32_t NetDll_connect(uint32_t caller, uint32_t socket, uint32_t name, uint32_t namelen)
    {
        return SocketError("NetDll_connect");
    }

    int32_t NetDll_listen(uint32_t caller, uint32_t socket, uint32_t backlog)
    {
        return SocketError("NetDll_listen");
    }

    uint32_t NetDll_accept(uint32_t caller, uint32_t socket, uint32_t addr, u32* addrLength)
    {
        return InvalidSocket("NetDll_accept");
    }

    int32_t NetDll_shutdown(uint32_t caller, uint32_t socket, uint32_t how)
    {
        return SocketError("NetDll_shutdown");
    }

    int32_t NetDll_send(uint32_t caller, uint32_t socket, uint32_t buffer, uint32_t length, uint32_t flags)
    {
        return SocketError("NetDll_send");
    }

    int32_t NetDll_sendto(uint32_t caller, uint32_t socket, uint32_t buffer, uint32_t length, uint32_t flags, uint32_t to, uint32_t toLength)
    {
        return SocketError("NetDll_sendto");
    }

    int32_t NetDll_recv(uint32_t caller, uint32_t socket, uint32_t buffer, uint32_t length, uint32_t flags)
    {
        return SocketError("NetDll_recv");
    }

    int32_t NetDll_recvfrom(uint32_t caller, uint32_t socket, uint32_t buffer, uint32_t length, uint32_t flags, uint32_t from, u32* fromLength)
    {
        return SocketError("NetDll_recvfrom");
    }

    int32_t NetDll_select(uint32_t caller, uint32_t nfds, uint32_t readfds, uint32_t writefds, uint32_t exceptfds, uint32_t timeout)
    {
        return SocketError("NetDll_select");
    }

    int32_t NetDll_ioctlsocket(uint32_t caller, uint32_t socket, uint32_t cmd, uint32_t argp)
    {
        return SocketError("NetDll_ioctlsocket");
    }

    int32_t NetDll_setsockopt(uint32_t caller, uint32_t socket, uint32_t level, uint32_t optname, uint32_t optval, uint32_t optlen)
    {
        return SocketError("NetDll_setsockopt");
    }

    int32_t NetDll_getsockopt(uint32_t caller, uint32_t socket, uint32_t level, uint32_t optname, uint32_t optval, u32* optlen)
    {
        return SocketError("NetDll_getsockopt");
    }

    int32_t NetDll_getsockname(uint32_t caller, uint32_t socket, uint32_t name, u32* namelen)
    {
        return SocketError("NetDll_getsockname");
    }

    int32_t NetDll_getpeername(uint32_t caller, uint32_t socket, uint32_t name, u32* namelen)
    {
        return SocketError("NetDll_getpeername");
    }

    // No caller argument on this one. Dotted-quad only; the empty string gives 0 as on console.
    uint32_t NetDll_inet_addr(const char* text)
    {
        if (!text)
            return kInaddrNone;
        if (!text[0])
            return 0;
        return ParseIPv4(text);
    }
}

DAO_IMPORT_AS(NetDll_WSAStartup, dao::imp::NetDll_WSAStartup)
DAO_IMPORT_AS(NetDll_WSACleanup, dao::imp::NetDll_WSACleanup)
DAO_IMPORT_AS(NetDll_WSAGetLastError, dao::imp::NetDll_WSAGetLastError)
DAO_IMPORT_AS(NetDll_WSACreateEvent, dao::imp::NetDll_WSACreateEvent)
DAO_IMPORT_AS(NetDll_WSACloseEvent, dao::imp::NetDll_WSACloseEvent)
DAO_IMPORT_AS(NetDll_WSASetEvent, dao::imp::NetDll_WSASetEvent)
DAO_IMPORT_AS(NetDll_WSAResetEvent, dao::imp::NetDll_WSAResetEvent)
DAO_IMPORT_AS(NetDll_WSAWaitForMultipleEvents, dao::imp::NetDll_WSAWaitForMultipleEvents)
DAO_IMPORT_AS(NetDll_WSAGetOverlappedResult, dao::imp::NetDll_WSAGetOverlappedResult)
DAO_IMPORT_AS(NetDll_WSARecv, dao::imp::NetDll_WSARecv)
DAO_IMPORT_AS(NetDll_WSARecvFrom, dao::imp::NetDll_WSARecvFrom)
DAO_IMPORT_AS(NetDll_XNetStartup, dao::imp::NetDll_XNetStartup)
DAO_IMPORT_AS(NetDll_XNetCleanup, dao::imp::NetDll_XNetCleanup)
DAO_IMPORT_AS(NetDll_XNetRandom, dao::imp::NetDll_XNetRandom)
DAO_IMPORT_AS(NetDll_XNetGetEthernetLinkStatus, dao::imp::NetDll_XNetGetEthernetLinkStatus)
DAO_IMPORT_AS(NetDll_XNetGetTitleXnAddr, dao::imp::NetDll_XNetGetTitleXnAddr)
DAO_IMPORT_AS(NetDll_XNetGetConnectStatus, dao::imp::NetDll_XNetGetConnectStatus)
DAO_IMPORT_AS(NetDll_XNetConnect, dao::imp::NetDll_XNetConnect)
DAO_IMPORT_AS(NetDll_XNetCreateKey, dao::imp::NetDll_XNetCreateKey)
DAO_IMPORT_AS(NetDll_XNetRegisterKey, dao::imp::NetDll_XNetRegisterKey)
DAO_IMPORT_AS(NetDll_XNetUnregisterKey, dao::imp::NetDll_XNetUnregisterKey)
DAO_IMPORT_AS(NetDll_XNetDnsLookup, dao::imp::NetDll_XNetDnsLookup)
DAO_IMPORT_AS(NetDll_XNetDnsRelease, dao::imp::NetDll_XNetDnsRelease)
DAO_IMPORT_AS(NetDll_XNetInAddrToString, dao::imp::NetDll_XNetInAddrToString)
DAO_IMPORT_AS(NetDll_XNetServerToInAddr, dao::imp::NetDll_XNetServerToInAddr)
DAO_IMPORT_AS(NetDll_XNetQosListen, dao::imp::NetDll_XNetQosListen)
DAO_IMPORT_AS(NetDll_XNetQosLookup, dao::imp::NetDll_XNetQosLookup)
DAO_IMPORT_AS(NetDll_XNetQosRelease, dao::imp::NetDll_XNetQosRelease)
DAO_IMPORT_AS(NetDll_XNetQosServiceLookup, dao::imp::NetDll_XNetQosServiceLookup)
DAO_IMPORT_AS(NetDll_socket, dao::imp::NetDll_socket)
DAO_IMPORT_AS(NetDll_closesocket, dao::imp::NetDll_closesocket)
DAO_IMPORT_AS(NetDll_bind, dao::imp::NetDll_bind)
DAO_IMPORT_AS(NetDll_connect, dao::imp::NetDll_connect)
DAO_IMPORT_AS(NetDll_listen, dao::imp::NetDll_listen)
DAO_IMPORT_AS(NetDll_accept, dao::imp::NetDll_accept)
DAO_IMPORT_AS(NetDll_shutdown, dao::imp::NetDll_shutdown)
DAO_IMPORT_AS(NetDll_send, dao::imp::NetDll_send)
DAO_IMPORT_AS(NetDll_sendto, dao::imp::NetDll_sendto)
DAO_IMPORT_AS(NetDll_recv, dao::imp::NetDll_recv)
DAO_IMPORT_AS(NetDll_recvfrom, dao::imp::NetDll_recvfrom)
DAO_IMPORT_AS(NetDll_select, dao::imp::NetDll_select)
DAO_IMPORT_AS(NetDll_ioctlsocket, dao::imp::NetDll_ioctlsocket)
DAO_IMPORT_AS(NetDll_setsockopt, dao::imp::NetDll_setsockopt)
DAO_IMPORT_AS(NetDll_getsockopt, dao::imp::NetDll_getsockopt)
DAO_IMPORT_AS(NetDll_getsockname, dao::imp::NetDll_getsockname)
DAO_IMPORT_AS(NetDll_getpeername, dao::imp::NetDll_getpeername)
DAO_IMPORT_AS(NetDll_inet_addr, dao::imp::NetDll_inet_addr)
