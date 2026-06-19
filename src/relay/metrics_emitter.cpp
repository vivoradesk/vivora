#include "relay/metrics_emitter.h"

#include "common/utils/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace vivora::ops {

namespace {

// Parse "http://host:port/path" into its parts.  Only plain http:// is
// supported (the managed relay POSTs to the co-located backend over loopback,
// so no TLS).  host must be a numeric IPv4 (no DNS).
bool parse_url(const std::string& url, std::string& host, uint16_t& port,
               std::string& path) {
    const std::string pfx = "http://";
    if (url.rfind(pfx, 0) != 0) return false;
    const std::string rest = url.substr(pfx.size());
    const size_t slash = rest.find('/');
    const std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    const size_t colon = hostport.find(':');
    if (colon == std::string::npos) {
        host = hostport;
        port = 80;
    } else {
        host = hostport.substr(0, colon);
        port = static_cast<uint16_t>(std::atoi(hostport.c_str() + colon + 1));
    }
    return !host.empty() && port != 0;
}

} // namespace

MetricsEmitter::MetricsEmitter(const char* source) : source_(source) {
#ifndef _WIN32
    const char* url = std::getenv("METRICS_INGEST_URL");
    const char* tok = std::getenv("METRICS_INGEST_TOKEN");
    if (url && tok && url[0] && tok[0] && parse_url(url, host_, port_, path_)) {
        token_   = tok;
        enabled_ = true;
        log::info("Metrics", "emitter enabled: source=%s -> %s", source_.c_str(), url);
    }
#else
    (void)source;
#endif
}

void MetricsEmitter::emit(int concurrent, uint64_t egress_bytes_delta) {
#ifndef _WIN32
    if (!enabled_) return;

    char body[160];
    const int blen = std::snprintf(
        body, sizeof(body),
        "{\"source\":\"%s\",\"concurrent\":%d,\"egress_bytes\":%llu}",
        source_.c_str(), concurrent, static_cast<unsigned long long>(egress_bytes_delta));
    if (blen <= 0) return;

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;

    // Short timeouts so a wedged / down backend never stalls the relay loop.
    struct timeval tv {};
    tv.tv_sec  = 0;
    tv.tv_usec = 500000;  // 0.5s
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port_);
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return;
    }
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return;
    }

    char req[512];
    const int rlen = std::snprintf(
        req, sizeof(req),
        "POST %s HTTP/1.1\r\nHost: %s\r\nX-Metrics-Token: %s\r\n"
        "Content-Type: application/json\r\nContent-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        path_.c_str(), host_.c_str(), token_.c_str(), blen, body);
    if (rlen > 0) {
        int off = 0;
        while (off < rlen) {
            const ssize_t w = ::send(fd, req + off, static_cast<size_t>(rlen - off), 0);
            if (w <= 0) break;
            off += static_cast<int>(w);
        }
    }
    ::close(fd);  // fire-and-forget; we don't read the response
#else
    (void)concurrent;
    (void)egress_bytes_delta;
#endif
}

} // namespace vivora::ops
