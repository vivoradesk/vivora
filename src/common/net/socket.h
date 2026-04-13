#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>

namespace deskbeam::net {

struct SocketAddr {
    uint32_t ip = 0;      // network byte order
    uint16_t port = 0;    // host byte order

    bool operator==(const SocketAddr& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const SocketAddr& o) const { return !(*this == o); }
    bool operator<(const SocketAddr& o) const {
        return ip < o.ip || (ip == o.ip && port < o.port);
    }
};

// Parse "1.2.3.4" -> network byte order IP. Returns 0 on failure.
uint32_t parse_ip(const char* str);

class IUdpSocket {
public:
    virtual ~IUdpSocket() = default;

    virtual bool bind(uint16_t port) = 0;
    virtual bool set_nonblocking(bool enabled) = 0;
    virtual bool set_sendbuf(int size_bytes) = 0;
    virtual bool set_recvbuf(int size_bytes) = 0;

    // Returns bytes sent, or -1 on error.
    virtual int send_to(const uint8_t* data, size_t len, const SocketAddr& dest) = 0;

    // Returns bytes received, or -1 on error (including EWOULDBLOCK).
    // On EWOULDBLOCK, returns 0.
    virtual int recv_from(uint8_t* buf, size_t buf_len, SocketAddr& sender) = 0;

    virtual void close() = 0;

    static std::unique_ptr<IUdpSocket> create();
};

} // namespace deskbeam::net
