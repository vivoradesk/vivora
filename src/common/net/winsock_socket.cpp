// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifdef VIVORA_WINDOWS

#include "common/net/winsock_socket.h"
#include "common/utils/log.h"

#include <iphlpapi.h>
#include <vector>
#include <chrono>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace vivora::net {

static const char* TAG = "NET";

// --- WinsockInit ---

WinsockInit::WinsockInit() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log::error(TAG, "WSAStartup failed: %d", WSAGetLastError());
        return;
    }
    ok = true;
}

WinsockInit::~WinsockInit() {
    if (ok) WSACleanup();
}

// --- parse_ip ---

uint32_t parse_ip(const char* str) {
    struct in_addr addr;
    if (inet_pton(AF_INET, str, &addr) == 1)
        return addr.s_addr;
    return 0;
}

SocketAddr resolve_host(const char* host, uint16_t port) {
    SocketAddr out;
    if (!host || !*host) return out;
    uint32_t lit = parse_ip(host);
    if (lit != 0) {
        out.ip   = lit;
        out.port = port;
        return out;
    }
    addrinfo hints = {};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(host, nullptr, &hints, &res);
    if (rc != 0 || !res) {
        log::warn(TAG, "getaddrinfo(%s) failed: %d", host, rc);
        return out;
    }
    for (addrinfo* it = res; it; it = it->ai_next) {
        if (it->ai_family == AF_INET && it->ai_addr && it->ai_addrlen >= sizeof(sockaddr_in)) {
            auto* sa = reinterpret_cast<sockaddr_in*>(it->ai_addr);
            out.ip   = sa->sin_addr.s_addr;
            out.port = port;
            break;
        }
    }
    freeaddrinfo(res);
    return out;
}

// --- enumerate_local_ipv4 ---

std::vector<uint32_t> enumerate_local_ipv4(size_t max_count) {
    std::vector<uint32_t> out;
    ULONG buflen = 16 * 1024;
    std::vector<BYTE> buf(buflen);
    PIP_ADAPTER_ADDRESSES list = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
    DWORD rc = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, list, &buflen);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(buflen);
        list = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
        rc = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, list, &buflen);
    }
    if (rc != NO_ERROR) return out;
    for (auto* adapter = list; adapter && out.size() < max_count; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp)             continue;
        if (adapter->IfType    == IF_TYPE_SOFTWARE_LOOPBACK)   continue;
        for (auto* ua = adapter->FirstUnicastAddress;
             ua && out.size() < max_count;
             ua = ua->Next) {
            if (!ua->Address.lpSockaddr) continue;
            if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            const uint32_t ip = sa->sin_addr.s_addr;
            const uint32_t host_order = ntohl(ip);
            if (host_order == 0)                                  continue;
            if ((host_order & 0xFFFF0000u) == 0xA9FE0000u)         continue; // 169.254/16
            out.push_back(ip);
        }
    }
    return out;
}

// --- WinsockUdpSocket ---

WinsockUdpSocket::WinsockUdpSocket() {
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) {
        log::error(TAG, "socket() failed: %d", WSAGetLastError());
    }
}

WinsockUdpSocket::~WinsockUdpSocket() {
    close();
}

bool WinsockUdpSocket::bind(uint16_t port) {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(sock_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        log::error(TAG, "bind(%u) failed: %d", port, WSAGetLastError());
        return false;
    }
    return true;
}

uint16_t WinsockUdpSocket::local_port() const {
    sockaddr_in addr = {};
    int addr_len = sizeof(addr);
    if (getsockname(sock_, (sockaddr*)&addr, &addr_len) == SOCKET_ERROR) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

bool WinsockUdpSocket::set_nonblocking(bool enabled) {
    u_long mode = enabled ? 1 : 0;
    if (ioctlsocket(sock_, FIONBIO, &mode) == SOCKET_ERROR) {
        log::error(TAG, "set_nonblocking failed: %d", WSAGetLastError());
        return false;
    }
    return true;
}

bool WinsockUdpSocket::set_sendbuf(int size_bytes) {
    if (setsockopt(sock_, SOL_SOCKET, SO_SNDBUF,
                   (const char*)&size_bytes, sizeof(size_bytes)) == SOCKET_ERROR) {
        log::error(TAG, "set_sendbuf(%d) failed: %d", size_bytes, WSAGetLastError());
        return false;
    }
    return true;
}

bool WinsockUdpSocket::set_recvbuf(int size_bytes) {
    if (setsockopt(sock_, SOL_SOCKET, SO_RCVBUF,
                   (const char*)&size_bytes, sizeof(size_bytes)) == SOCKET_ERROR) {
        log::error(TAG, "set_recvbuf(%d) failed: %d", size_bytes, WSAGetLastError());
        return false;
    }
    return true;
}

int WinsockUdpSocket::send_to(const uint8_t* data, size_t len, const SocketAddr& dest) {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dest.port);
    addr.sin_addr.s_addr = dest.ip;

    int ret = ::sendto(sock_, (const char*)data, (int)len, 0,
                       (sockaddr*)&addr, sizeof(addr));
    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return 0;
        // Throttle: WiFi drop fires sendto failures at packet rate
        // (~100/s) which can fill the log file in seconds.  Atomic because the
        // PacedSender thread and the host main thread (relay keepalive/bind,
        // which bypass the pacer) can both land here concurrently — plain
        // statics would be a data race / UB (VIV-96).  A relaxed CAS on the
        // timestamp keeps at most one line per second; a lost race just merges
        // into the suppressed count, which is fine for a log throttle.
        static std::atomic<int64_t>  last_log_ns{0};
        static std::atomic<uint64_t> suppressed{0};
        using namespace std::chrono;
        const int64_t now_ns =
            duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
        int64_t prev = last_log_ns.load(std::memory_order_relaxed);
        if (now_ns - prev >= 1'000'000'000
            && last_log_ns.compare_exchange_strong(prev, now_ns,
                                                   std::memory_order_relaxed)) {
            const uint64_t n = suppressed.exchange(0, std::memory_order_relaxed);
            log::error(TAG, "sendto failed: %d (x%llu suppressed)",
                       err, static_cast<unsigned long long>(n));
        } else {
            suppressed.fetch_add(1, std::memory_order_relaxed);
        }
        return -1;
    }
    return ret;
}

int WinsockUdpSocket::recv_from(uint8_t* buf, size_t buf_len, SocketAddr& sender) {
    sockaddr_in addr = {};
    int addr_len = sizeof(addr);

    int ret = ::recvfrom(sock_, (char*)buf, (int)buf_len, 0,
                         (sockaddr*)&addr, &addr_len);
    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return 0;
        if (err == WSAECONNRESET) return 0;  // remote closed, treat as no data
        log::error(TAG, "recvfrom failed: %d", err);
        return -1;
    }

    sender.ip = addr.sin_addr.s_addr;
    sender.port = ntohs(addr.sin_port);
    return ret;
}

void WinsockUdpSocket::close() {
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

// --- Factory ---

std::unique_ptr<IUdpSocket> IUdpSocket::create() {
    return std::make_unique<WinsockUdpSocket>();
}

} // namespace vivora::net

#endif // VIVORA_WINDOWS
