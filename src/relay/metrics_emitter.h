#pragma once

#include <cstdint>
#include <string>

namespace vivora::ops {

// Opt-in operational metrics push for the managed relay / rendezvous (VIV-72).
//
// Reads METRICS_INGEST_URL + METRICS_INGEST_TOKEN from the environment; if
// either is unset (or not a plain http://IP:port/path URL), enabled() stays
// false and emit() is a no-op — so AGPL self-host instances NEVER phone home.
// Only the managed instance, configured via env, reports.
//
// POSIX + localhost only: a fire-and-forget plain-HTTP POST with a short
// timeout; any failure is ignored so the UDP loop is never stalled.
class MetricsEmitter {
public:
    explicit MetricsEmitter(const char* source);
    bool enabled() const { return enabled_; }

    // POST {"source","concurrent","egress_bytes"} to the ingest URL.
    void emit(int concurrent, uint64_t egress_bytes_delta);

private:
    bool        enabled_ = false;
    std::string source_;
    std::string host_;     // numeric IP, e.g. 127.0.0.1
    std::string path_;     // e.g. /metrics/ingest
    uint16_t    port_ = 0;
    std::string token_;
};

} // namespace vivora::ops
