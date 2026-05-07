#if defined(DESKBEAM_MACOS) || defined(DESKBEAM_LINUX)

#include "common/net/posix_socket.h"
#include "common/utils/log.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>

namespace deskbeam::net {

static const char* TAG = "NET";

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
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(host, nullptr, &hints, &res);
    if (rc != 0 || !res) {
        log::warn(TAG, "getaddrinfo(%s) failed: %d", host, rc);
        return out;
    }
    for (addrinfo* it = res; it; it = it->ai_next) {
        if (it->ai_family == AF_INET && it->ai_addr &&
            it->ai_addrlen >= sizeof(sockaddr_in)) {
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
    ifaddrs* ifa_list = nullptr;
    if (getifaddrs(&ifa_list) != 0 || !ifa_list) return out;
    for (ifaddrs* ifa = ifa_list; ifa && out.size() < max_count; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        const auto* sa = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
        const uint32_t ip = sa->sin_addr.s_addr;  // network byte order
        // Skip 169.254.0.0/16 (link-local) and 0.0.0.0.
        const uint32_t host_order = ntohl(ip);
        if (host_order == 0)                                 continue;
        if ((host_order & 0xFFFF0000u) == 0xA9FE0000u)        continue;
        out.push_back(ip);
    }
    freeifaddrs(ifa_list);
    return out;
}

// --- PosixUdpSocket ---

PosixUdpSocket::PosixUdpSocket() {
    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ < 0) {
        log::error(TAG, "socket() failed: %s", std::strerror(errno));
    }
}

PosixUdpSocket::~PosixUdpSocket() {
    close();
}

bool PosixUdpSocket::bind(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        log::error(TAG, "bind(%u) failed: %s", port, std::strerror(errno));
        return false;
    }
    return true;
}

uint16_t PosixUdpSocket::local_port() const {
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    if (::getsockname(sock_, reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

bool PosixUdpSocket::set_nonblocking(bool enabled) {
    int flags = ::fcntl(sock_, F_GETFL, 0);
    if (flags < 0) return false;
    if (enabled) flags |= O_NONBLOCK;
    else         flags &= ~O_NONBLOCK;
    if (::fcntl(sock_, F_SETFL, flags) < 0) {
        log::error(TAG, "set_nonblocking failed: %s", std::strerror(errno));
        return false;
    }
    return true;
}

bool PosixUdpSocket::set_sendbuf(int size_bytes) {
    if (::setsockopt(sock_, SOL_SOCKET, SO_SNDBUF,
                     &size_bytes, sizeof(size_bytes)) < 0) {
        log::error(TAG, "set_sendbuf(%d) failed: %s", size_bytes, std::strerror(errno));
        return false;
    }
    return true;
}

bool PosixUdpSocket::set_recvbuf(int size_bytes) {
    if (::setsockopt(sock_, SOL_SOCKET, SO_RCVBUF,
                     &size_bytes, sizeof(size_bytes)) < 0) {
        log::error(TAG, "set_recvbuf(%d) failed: %s", size_bytes, std::strerror(errno));
        return false;
    }
    return true;
}

int PosixUdpSocket::send_to(const uint8_t* data, size_t len, const SocketAddr& dest) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dest.port);
    addr.sin_addr.s_addr = dest.ip;

    ssize_t ret = ::sendto(sock_, data, len, 0,
                           reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        log::error(TAG, "sendto failed: %s", std::strerror(errno));
        return -1;
    }
    return static_cast<int>(ret);
}

int PosixUdpSocket::recv_from(uint8_t* buf, size_t buf_len, SocketAddr& sender) {
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);

    ssize_t ret = ::recvfrom(sock_, buf, buf_len, 0,
                             reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        log::error(TAG, "recvfrom failed: %s", std::strerror(errno));
        return -1;
    }

    sender.ip = addr.sin_addr.s_addr;
    sender.port = ntohs(addr.sin_port);
    return static_cast<int>(ret);
}

void PosixUdpSocket::close() {
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
}

// --- Factory ---

std::unique_ptr<IUdpSocket> IUdpSocket::create() {
    return std::make_unique<PosixUdpSocket>();
}

} // namespace deskbeam::net

#endif
