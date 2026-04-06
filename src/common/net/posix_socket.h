#pragma once

#if defined(DESKBEAM_MACOS) || defined(DESKBEAM_LINUX)

#include "common/net/socket.h"

namespace deskbeam::net {

class PosixUdpSocket : public IUdpSocket {
public:
    PosixUdpSocket();
    ~PosixUdpSocket() override;

    bool bind(uint16_t port) override;
    bool set_nonblocking(bool enabled) override;
    bool set_sendbuf(int size_bytes) override;
    bool set_recvbuf(int size_bytes) override;
    int send_to(const uint8_t* data, size_t len, const SocketAddr& dest) override;
    int recv_from(uint8_t* buf, size_t buf_len, SocketAddr& sender) override;
    void close() override;

private:
    int sock_ = -1;
};

} // namespace deskbeam::net

#endif
