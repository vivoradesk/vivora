#ifdef DESKBEAM_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "common/net/winsock_socket.h"
#include "host/capture/screen_capture.h"
#include "host/capture/dxgi_capture.h"
#include "host/encode/video_encoder.h"
#include "host/session/video_sender.h"
#include "client/net/video_receiver.h"
#include "common/net/socket.h"
#include "common/net/frame_assembler.h"
#include "common/utils/log.h"
#include <cassert>
#include <cstdio>
#include <chrono>
#include <thread>

using namespace deskbeam;

int main() {
    log::info("TEST", "=== Transport Test: Capture -> Encode -> UDP Loopback -> Reassemble ===");

    // Initialize Winsock
    net::WinsockInit wsa;
    if (!wsa.ok) {
        log::error("TEST", "Failed to init Winsock");
        return 1;
    }

    // --- Set up capture + encoder (host side) ---
    auto capture = IScreenCapture::create();
    auto* dxgi = dynamic_cast<DxgiCapture*>(capture.get());
    assert(dxgi);
    if (!capture->init(0)) {
        log::error("TEST", "Failed to init capture");
        return 1;
    }
    auto res = capture->get_resolution();
    log::info("TEST", "Capture: %ux%u", res.width, res.height);

    auto encoder = IVideoEncoder::create();
    EncoderConfig cfg;
    cfg.width = res.width;
    cfg.height = res.height;
    cfg.fps = 60;
    cfg.bitrate_bps = 15'000'000;
    cfg.idr_period = 60;
    cfg.input_format = dxgi->get_capture_format();
    if (!encoder->init(cfg, dxgi->get_device())) {
        log::error("TEST", "Failed to init encoder");
        return 1;
    }

    // --- Set up UDP sockets (loopback) ---
    auto send_sock = net::IUdpSocket::create();
    auto recv_sock = net::IUdpSocket::create();
    assert(send_sock && recv_sock);

    const uint16_t TEST_PORT = 19876;
    if (!recv_sock->bind(TEST_PORT)) {
        log::error("TEST", "Failed to bind recv socket to port %u", TEST_PORT);
        return 1;
    }
    recv_sock->set_nonblocking(true);
    recv_sock->set_recvbuf(1024 * 1024);

    send_sock->set_nonblocking(true);
    send_sock->set_sendbuf(512 * 1024);

    net::SocketAddr dest;
    dest.ip = net::parse_ip("127.0.0.1");
    dest.port = TEST_PORT;

    host::VideoSender sender(*send_sock);
    client::VideoReceiver receiver(*recv_sock);

    // --- Open output file ---
    FILE* outfile = fopen("transport_test.h265", "wb");
    if (!outfile) {
        log::error("TEST", "Failed to open output file");
        return 1;
    }

    // Force mouse movement
    INPUT mi = {};
    mi.type = INPUT_MOUSE;
    mi.mi.dwFlags = MOUSEEVENTF_MOVE;
    mi.mi.dx = 1;
    SendInput(1, &mi, sizeof(INPUT));

    // --- Main loop: capture -> encode -> send -> recv -> reassemble ---
    const int TARGET_FRAMES = 60;
    int encoded = 0;
    int received = 0;
    uint16_t frame_seq = 0;
    uint64_t total_sent_bytes = 0;
    uint64_t total_recv_bytes = 0;
    double total_encode_ms = 0.0;
    double max_rtt_ms = 0.0;
    auto start = Clock::now();

    for (int attempt = 0; attempt < TARGET_FRAMES * 3 && received < TARGET_FRAMES; ++attempt) {
        // --- Host side: capture + encode + send ---
        CapturedFrame frame;
        if (capture->capture_frame(frame, 50)) {
            auto t_enc_start = Clock::now();
            uint64_t pts = std::chrono::duration_cast<std::chrono::microseconds>(
                frame.capture_time.time_since_epoch()).count();

            bool ok = encoder->encode(frame.texture.Get(), pts);
            capture->release_frame(frame);

            if (ok) {
                auto t_enc_end = Clock::now();
                double enc_ms = std::chrono::duration<double, std::milli>(t_enc_end - t_enc_start).count();
                total_encode_ms += enc_ms;

                EncodedPacket pkt;
                while (encoder->get_packet(pkt)) {
                    uint32_t timestamp = static_cast<uint32_t>(pkt.pts & 0xFFFFFFFF);
                    auto t_send = Clock::now();

                    int sent = sender.send_frame(pkt.data.data(), pkt.data.size(),
                                                 frame_seq, timestamp, pkt.keyframe, dest);
                    if (sent > 0) {
                        total_sent_bytes += pkt.data.size();
                        log::debug("TEST", "Sent frame %u: %zu bytes -> %d packets, enc=%.1fms",
                                   frame_seq, pkt.data.size(), sent, enc_ms);
                    }
                    frame_seq++;
                    encoded++;
                }
            }
        }

        // --- Client side: receive + reassemble ---
        // Small delay to let packets arrive through loopback
        std::this_thread::sleep_for(std::chrono::microseconds(100));

        receiver.poll();

        net::AssembledFrame assembled;
        while (receiver.pop_frame(assembled)) {
            fwrite(assembled.data.data(), 1, assembled.data.size(), outfile);
            total_recv_bytes += assembled.data.size();
            received++;
            log::debug("TEST", "Received frame seq=%u: %zu bytes %s",
                       assembled.seq_no, assembled.data.size(),
                       assembled.keyframe ? "IDR" : "P");
        }

        // Wiggle mouse
        if (attempt % 10 == 0) {
            mi.mi.dx = (attempt % 20 == 0) ? 1 : -1;
            SendInput(1, &mi, sizeof(INPUT));
        }
    }

    // Final drain: poll a few more times to catch stragglers
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        receiver.poll();
        net::AssembledFrame assembled;
        while (receiver.pop_frame(assembled)) {
            fwrite(assembled.data.data(), 1, assembled.data.size(), outfile);
            total_recv_bytes += assembled.data.size();
            received++;
        }
    }

    fclose(outfile);
    send_sock->close();
    recv_sock->close();

    auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();

    log::info("TEST", "=== Results ===");
    log::info("TEST", "Encoded frames: %d", encoded);
    log::info("TEST", "Received frames: %d", received);
    log::info("TEST", "Loss: %d frames (%.1f%%)", encoded - received,
              encoded > 0 ? 100.0 * (encoded - received) / encoded : 0.0);
    log::info("TEST", "Sent: %llu KB", (unsigned long long)(total_sent_bytes / 1024));
    log::info("TEST", "Received: %llu KB", (unsigned long long)(total_recv_bytes / 1024));
    log::info("TEST", "Average encode: %.2f ms", total_encode_ms / std::max(encoded, 1));
    log::info("TEST", "Packets sent: %llu", (unsigned long long)sender.packets_sent());
    log::info("TEST", "Packets received: %llu", (unsigned long long)receiver.packets_received());
    log::info("TEST", "Assembler dropped: %llu", (unsigned long long)receiver.frames_dropped());
    log::info("TEST", "Total time: %.2f s", elapsed);
    log::info("TEST", "Output: transport_test.h265");
    log::info("TEST", "  Play with: ffplay transport_test.h265");

    if (received > 0) {
        log::info("TEST", "=== PASS ===");
    } else {
        log::error("TEST", "=== FAIL: no frames received ===");
        return 1;
    }

    return 0;
}

#else
#include <cstdio>
int main() {
    std::printf("Transport test is Windows-only\n");
    return 0;
}
#endif
