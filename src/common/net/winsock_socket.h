#pragma once

#ifdef DESKBEAM_WINDOWS

#include "common/net/socket.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

namespace deskbeam::net {

// RAII Winsock init/cleanup — one instance per process
struct WinsockInit {
    WinsockInit();
    ~WinsockInit();
    bool ok = false;
};

class WinsockUdpSocket : public IUdpSocket {
public:
    WinsockUdpSocket();
    ~WinsockUdpSocket() override;

    bool bind(uint16_t port) override;
    bool set_nonblocking(bool enabled) override;
    bool set_sendbuf(int size_bytes) override;
    bool set_recvbuf(int size_bytes) override;
    int send_to(const uint8_t* data, size_t len, const SocketAddr& dest) override;
    int recv_from(uint8_t* buf, size_t buf_len, SocketAddr& sender) override;
    void close() override;

private:
    SOCKET sock_ = INVALID_SOCKET;
};

} // namespace deskbeam::net

#endif // DESKBEAM_WINDOWS
