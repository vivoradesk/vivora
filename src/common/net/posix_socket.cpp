#if defined(DESKBEAM_MACOS) || defined(DESKBEAM_LINUX)

#include "common/net/posix_socket.h"
#include "common/utils/log.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
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
