#include "common/net/socket.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace vivora::net {

SocketAddr resolve_host_port(const char* host_port) {
    SocketAddr out;
    if (!host_port || !*host_port) return out;

    const char* colon = std::strrchr(host_port, ':');
    if (!colon || colon == host_port || *(colon + 1) == '\0') return out;

    int port_i = std::atoi(colon + 1);
    if (port_i <= 0 || port_i > 65535) return out;

    std::string host(host_port, colon - host_port);
    return resolve_host(host.c_str(), static_cast<uint16_t>(port_i));
}

} // namespace vivora::net
