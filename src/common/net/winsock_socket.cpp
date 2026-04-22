#ifdef DESKBEAM_WINDOWS

#include "common/net/winsock_socket.h"
#include "common/utils/log.h"

#pragma comment(lib, "ws2_32.lib")

namespace deskbeam::net {

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
        log::error(TAG, "sendto failed: %d", err);
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

} // namespace deskbeam::net

#endif // DESKBEAM_WINDOWS
