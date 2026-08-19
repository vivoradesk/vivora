# Vivora architecture

How the pieces fit together, for people reading the code for the first time.
`STATUS.md` tracks what works on which platform; this document explains *why*
the code is shaped the way it is.

Latency is the design constraint everything else bends around. The target is
under 15 ms glass-to-glass on a LAN, which rules out most of the obvious
architectural conveniences: no intermediate buffering, no CPU round-trips for
video frames, no TCP for media, no retransmit-everything reliability.

---

## 1. Binaries

The build produces three executables:

| Binary | What it is |
| ------ | ---------- |
| `vivora` | The application. Qt GUI by default; `--host` / `--view` drop into the headless CLI paths. Host and client are the same binary because most users are both. |
| `vivora-rendezvous` | Stateless UDP signalling server. Maps a peer's public key to its last known address so two machines behind NAT can find each other. No media passes through it. |
| `vivora-relay` | Stateless UDP forwarder for the cases hole punching cannot solve (symmetric NAT, CGNAT, restrictive firewalls). Media *does* pass through it, so it is the expensive one to run. |

Both servers are Qt-free and depend on nothing beyond libstdc++ — deliberately,
so self-hosting is a single binary plus a systemd unit (see `deploy/`).

---

## 2. Host pipeline

```
capture ──► encode ──► fragment ──► FEC ──► encrypt ──► UDP
 (GPU)      (GPU)              (CPU, header-only)
```

The frame stays in GPU memory from capture through encode. On Windows that
means an `ID3D11Texture2D` handed straight to NVENC/AMF/QSV; nothing is mapped
into system memory until the encoder emits a compressed bitstream.

| Stage | Windows | Linux | macOS |
| ----- | ------- | ----- | ----- |
| Capture | DXGI Desktop Duplication | PipeWire via `xdg-desktop-portal` | ScreenCaptureKit |
| Encode | NVENC / AMF / QSV, auto-probed, with an NVENC to QSV fallback | VAAPI, or native NVENC on NVIDIA | VideoToolbox |
| Audio | WASAPI loopback | PulseAudio monitor source | CoreAudio tap |
| Input injection | `SendInput` | `/dev/uinput` | `CGEventPost` |

Platform specifics sit behind `src/app/host_platform.h`; `host_loop.cpp` is
platform-agnostic and drives capture to encode to send, plus the control
channel.

**Lazy encoder.** With zero connected clients the host keeps capture and
encoder torn down, so an idle tray process costs nothing. The first client
starts them; the last one leaving stops them.

**Static-screen heartbeat.** Capture backends only deliver frames when the
screen changes. Without a heartbeat, a client joining a motionless desktop sees
nothing at all. The host re-emits the last frame at a floor cadence with
`FLAG_HEARTBEAT` set, which the client excludes from its loss and framerate
statistics.

---

## 3. Client pipeline

```
UDP ──► decrypt ──► FEC recover ──► reassemble ──► decode ──► present
                                                    (GPU)      (GPU)
```

| Stage | Windows | Linux | macOS |
| ----- | ------- | ----- | ----- |
| Decode | MediaFoundation D3D11VA | FFmpeg with VAAPI, silent software fallback | VideoToolbox |
| Present | D3D11 swapchain | Qt OpenGL | `AVSampleBufferDisplayLayer` |
| Input capture | Raw Input | Qt events | NSEvent and IOKit |

Decoding runs on its own thread and hands finished surfaces to the render
thread through a lock-free queue (`src/common/utils/spsc_ring.h`), so a slow
frame stalls neither the socket nor the compositor.

**No half-decoded frames, ever.** Any decode error drops the frame and asks the
host for an IDR rather than presenting a partially reconstructed picture.
Visible corruption is treated as worse than a brief freeze.

---

## 4. Wire protocol

Custom UDP. Not TCP, which head-of-line blocks exactly when a stream can least
afford it. Not WebRTC, which is a dependency larger than the rest of the
project and whose congestion control is tuned for conversational video rather
than for a desktop that has to stay sharp.

### Packet header, 10 bytes

```
Type(1) | SeqNo(2) | Timestamp(4) | Flags(1) | PayloadLen(2) | Payload...
```

`src/common/protocol/packet.h` holds the type enum. Beyond the four media types
(`Video`, `Audio`, `Input`, `Control`) it carries the control traffic that makes
adaptation work: `IdrRequest`, `NackRequest`, `FecReport`, `BwProbe` and
`BwProbeAck`, `PerfReport`, `HostStats`, cursor shape and position, monitor
enumeration, clipboard, `Disconnect` with a reason, and `CodecRenegotiate`.

### Channels

- **Video** — unreliable, forward error correction, no full reliability. A
  frame that cannot be recovered is skipped and the client requests an IDR.
- **Audio** — semi-reliable, Opus, with an adaptive jitter buffer (30 to 200 ms)
  that grows when packet loss concealment kicks in and shrinks again when the
  line is quiet.
- **Input** — its own datagrams, so a video burst never delays a keystroke.
- **Control** — handshake, approval, clipboard, monitor switching.

### Loss recovery

Reed-Solomon FEC over groups of shards, with the parity ratio adapted from the
client's `FecReport` and held above a sticky floor so it does not oscillate.
Parity shards are interleaved across the group, so a burst loss spreads over
several groups instead of destroying one outright. Keyframe groups get extra
parity, because losing one costs a full IDR round-trip.

When FEC cannot recover a group the client NACKs the specific missing shards if
the group is nearly complete; only a group that stays unrecoverable escalates
to an IDR request.

### Congestion control

Bitrate follows a damage-driven controller: round-trip time, observed loss and
the client's reported sustainable framerate feed a target that the encoder is
reconfigured to. A bandwidth probe at connect time gives it a starting point.

FEC overhead is carved *out* of the wire budget, never added on top — raising
the parity ratio lowers the encoder's target by the same amount.

---

## 5. Security

`Noise_IK_25519_ChaChaPoly_BLAKE2b`, implemented on Monocypher
(`src/common/crypto/`). Not TLS: the handshake is two messages, the code is
auditable in an afternoon, and it keeps OpenSSL out of the media path.

- A host's Curve25519 public key **is** its identity. The peer code
  (`swift-tiger-4271`) is a memorable handle that the rendezvous server maps
  back to that key.
- IK rather than NK, so the host learns the client's static key during the
  handshake. That is what lets a host recognise a device it already trusts.
- Trust on first use: keys are pinned in `known_peers.txt`, and a mismatch on a
  later connection raises a prompt instead of silently continuing.
- Every channel is sealed, audio included, with a per-channel replay window.

---

## 6. Getting two machines connected

1. **Direct** — if the user typed an address, use it.
2. **Rendezvous and hole punch** — both peers register with the rendezvous
   server, exchange candidates (including LAN addresses, which short-circuit
   the whole procedure when both machines are on the same network) and punch
   through using STUN-discovered mappings.
3. **Relay** — if no HELLO_ACK arrives within a few seconds, both peers BIND to
   a relay with a shared session id and the relay forwards between them. The
   Noise session is re-established over the relay path, so a relay operator
   sees ciphertext only.

The Vivora-operated relay requires a Pro license token. A self-hosted relay does
not, and pointing the client at your own is a settings change.

---

## 7. Codec negotiation

The client advertises what it can decode in its HELLO. The host intersects that
with what it can encode and picks the best common codec. If the client's decoder
then fails at runtime, it sends `CodecRenegotiate` carrying its reduced
capabilities and the host rebuilds its encoder live, without dropping the
session.

The host keeps its *configured* codec preference separate from the *live* one,
so one H.264-only client connecting does not permanently pin the host to H.264
for everyone who connects afterwards.

---

## 8. Source layout

```
src/
├── common/          protocol, crypto, FEC, audio codec, sockets, utils
│   ├── protocol/    packet and input-event wire formats
│   ├── crypto/      Noise IK, packet sealing, peer pinning, license tokens
│   ├── net/         sockets, fragmenter/assembler, FEC, STUN, rendezvous, relay
│   └── audio/       Opus, resampler, jitter buffer, per-platform in/out
├── host/            capture/, encode/, input/, session/
├── client/          decode/, render/, net/
├── app/             the single binary: CLI entry, host and view loops,
│   └── gui/         per-platform glue, and the Qt controller, settings,
│                    tray and cloud client
└── relay/           rendezvous and relay servers
qml/                 the GUI, registered through a plain .qrc
tests/               CTest suites; the majority run on every platform
```

---

## 9. Rules that keep the latency budget

These are the conventions a change is judged against:

- **Zero-copy.** A video frame must not visit system memory between capture and
  encode, or between decode and present.
- **Never present a damaged frame.** Drop it and request an IDR instead.
- **FEC comes out of the budget.** More parity means a lower encoder target.
- **Platform code lives behind an interface** from the start, not after the
  second platform arrives.
- **Every stage reports its timing** — capture, encode, network RTT, decode,
  render — so a regression can be attributed rather than guessed at.
- **Protocol changes get a test first.** `tests/` is the only place the wire
  format is actually pinned down.
