# Vivora — Open-Source Low-Latency Remote Desktop

## Project Vision

Vivora — open-source remote desktop решение нового поколения, фокус на минимальную задержку и максимальное качество видеопотока. Цель — создать продукт, который по качеству стрима конкурирует с Parsec (4-8ms pipeline latency на LAN), но при этом open-source и self-hosted как RustDesk.

**Ключевое отличие от конкурентов:** zero-copy GPU pipeline, аппаратное кодирование, кастомный UDP-протокол — всё то, чего критически не хватает RustDesk (110K+ GitHub stars, но стриминг-пайплайн слабый: лимит битрейта ~600KB/s, реальный FPS часто вдвое ниже заявленного, задержки 100-1500ms даже на LAN).

---

## Технологический стек

- **Язык:** C++17 (core), с возможностью C++20 где уместно
- **UI Framework:** Qt 6 / QML (кросс-платформенность, опыт разработчика)
- **Сборка:** CMake
- **Платформы (MVP):** Windows (host + client), Linux (client)
- **Платформы (v2):** Linux (host), macOS (client)
- **Лицензия:** AGPL-3.0 (как RustDesk) или MIT — решить до публикации

---

## Архитектура (High-Level)

```
┌──────────────────────────────────────────────────┐
│                   HOST (сервер)                   │
│                                                    │
│  ┌─────────┐   ┌─────────┐   ┌────────────────┐  │
│  │ Screen  │──▶│ GPU     │──▶│  Network       │  │
│  │ Capture │   │ Encoder │   │  Protocol      │──────▶ UDP
│  └─────────┘   └─────────┘   └────────────────┘  │
│                                                    │
│  ┌─────────────────────────────────────────────┐  │
│  │          Input Injection                     │  │
│  │     (keyboard, mouse, gamepad)               │◀──── UDP
│  └─────────────────────────────────────────────┘  │
│                                                    │
│  ┌─────────────────────────────────────────────┐  │
│  │    Auxiliary Channels                        │  │
│  │  (clipboard, file transfer, audio)           │◀──▶ TCP/UDP
│  └─────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────┐
│                  CLIENT (клиент)                   │
│                                                    │
│  ┌────────────────┐   ┌─────────┐   ┌─────────┐  │
│  │   Network      │──▶│ GPU     │──▶│ Render  │  │
│  │   Protocol     │   │ Decoder │   │ Display │  │
│  └────────────────┘   └─────────┘   └─────────┘  │
│                                                    │
│  ┌─────────────────────────────────────────────┐  │
│  │          Input Capture                       │  │
│  │     (keyboard, mouse, gamepad)               │──────▶ UDP
│  └─────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────┘
```

---

## Модуль 1: Screen Capture (Host)

### Windows
- **Primary:** DXGI Desktop Duplication API
  - Zero-copy: кадр остаётся в GPU memory (ID3D11Texture2D)
  - Поддержка dirty rects — кодировать только изменившиеся области
  - Поддержка cursor shape и position отдельно от кадра
  - Работает с Windows 8+
- **Fallback:** Windows Graphics Capture API (Windows 10 1903+)
  - Для UWP/protected content если DXGI не работает

### Linux (v2)
- **Primary:** DRM/KMS + DMA-BUF
  - Прямой доступ к GPU framebuffer
  - Работает на Wayland и headless серверах
- **Fallback:** PipeWire screen capture
  - Для десктопных окружений с Wayland

### Ключевые требования
- Кадр НЕ должен копироваться в системную память (RAM)
- Поддержка multi-monitor: каждый монитор — отдельный поток захвата
- Target: < 1ms на операцию захвата
- Поддержка HDR passthrough (в будущем)

---

## Модуль 2: Video Encoding (Host)

### Hardware Encoding (приоритет)
- **NVIDIA NVENC** — через NVIDIA Video Codec SDK (nvEncodeAPI)
  - Preset: P1 (lowest latency) или Tuning: ultra-low-latency
  - Rate control: CBR для стабильного битрейта
  - Важно: использовать async encode для pipeline overlap
- **AMD AMF** — через Advanced Media Framework SDK
  - Аналогичные low-latency presets
- **Intel QSV** — через Intel Media SDK / oneVPL
  - Для систем без дискретной GPU

### Кодеки (в порядке приоритета)
1. **H.264** — максимальная совместимость, хардварная поддержка везде
2. **H.265/HEVC** — лучшее качество на тот же битрейт, но не все GPU декодируют
3. **AV1** — будущее, но пока ограниченная поддержка HW encode

### Software Encoding (fallback)
- **x264** — ultrafast preset, zero-latency tune
- Использовать ТОЛЬКО если нет HW encoder
- Предупреждать пользователя о degraded performance

### Zero-Copy Pipeline
```
DXGI Texture (GPU) ──▶ NVENC Input (GPU) ──▶ Encoded Bitstream (GPU→CPU)
                   НЕТ копирования в RAM!
```
- Ключевая оптимизация: передать ID3D11Texture2D напрямую в encoder
- Для NVENC: использовать DirectX interop (NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX)
- Для AMF: AMFSurface из D3D11 texture

### Адаптивный битрейт
- Стартовый битрейт: 10-20 Mbps (1080p60)
- Динамическая подстройка на основе:
  - Network RTT и packet loss (от протокола)
  - Encoder buffer fullness
  - Client decoder feedback
- Изменение битрейта через encoder API без пересоздания сессии
- IDR-frame по запросу клиента (при потере ключевого кадра)

### Целевые метрики
- Encoding latency: < 5ms для NVENC, < 10ms для AMF/QSV
- Framerate: 60fps стабильно, 120fps опционально
- Разрешение: до 4K (3840x2160)

---

## Модуль 3: Network Protocol

### Архитектура — кастомный UDP-протокол
TCP не подходит для real-time стриминга: head-of-line blocking убивает латенси.
WebRTC слишком тяжёлый и несёт ненужные зависимости.

### Структура каналов
- **Video Channel** (UDP, unreliable + FEC)
  - Основной поток видеоданных
  - Forward Error Correction (FEC) вместо ретрансмиссии
  - Потерянный кадр лучше пропустить, чем ждать
- **Audio Channel** (UDP, semi-reliable)
  - Opus codec, отдельный от видео
  - Маленький jitter buffer (20-40ms)
- **Input Channel** (UDP, reliable)
  - Минимальный overhead: timestamp + input event
  - Отдельный от видео — ввод не должен ждать видео-пакеты
  - Reliable delivery через lightweight ACK mechanism
- **Control Channel** (TCP)
  - Handshake, authentication, session management
  - Clipboard, file transfer negotiation
  - Не критично к латенси

### Шифрование
- **Noise Protocol (pattern NK)** для всех каналов (UDP и control)
  - Реализация через libsodium (~100KB vs OpenSSL ~1-2MB)
  - Curve25519 для key exchange + ChaCha20-Poly1305 для symmetric encryption
  - Single-flight handshake (1-2 пакетов) — совместим с UDP hole punching
  - 16 байт auth tag на пакет (vs 29 байт у DTLS)
  - Публичный ключ хоста = его ID (показывается как QR-код, TOFU при первом подключении)
  - Эквивалентно DTLS по уровню защиты, используется в WireGuard/Tailscale

### Congestion Control
- Кастомный алгоритм, адаптированный для видеостриминга:
  - Приоритет: минимальный jitter > максимальный throughput
  - Мониторинг one-way delay variation (не только RTT)
  - Быстрое снижение битрейта при обнаружении congestion
  - Медленное восстановление (не агрессивное, как TCP)
- Тесная интеграция с video encoder:
  - При congestion → снизить битрейт encoder
  - При packet loss > threshold → запросить IDR frame

### NAT Traversal
- **STUN** — определение типа NAT и внешнего адреса
- **UDP Hole Punching** — прямое P2P соединение
- **TURN relay** — fallback когда P2P невозможен
  - Self-hosted relay server (простой, написать самим)
  - Опционально: облачные relay серверы для удобства

### Framing
```
┌─────────────────────────────────────────────┐
│ Packet Header (8 bytes)                      │
│ ┌──────┬──────┬───────┬──────┬─────────────┐│
│ │Type  │SeqNo │Timestamp│Flags│PayloadLen   ││
│ │1 byte│2 byte│4 bytes │1 byte│2 bytes     ││
│ └──────┴──────┴───────┴──────┴─────────────┘│
│ Payload (variable)                           │
│ FEC Data (if applicable)                     │
└─────────────────────────────────────────────┘
```

---

## Модуль 4: Video Decoding & Rendering (Client)

### Hardware Decoding
- **DXVA2 / D3D11VA** (Windows)
- **VAAPI** (Linux)
- **VideoToolbox** (macOS, v2)

### Zero-Copy Decode → Render
```
Network ──▶ HW Decoder (GPU) ──▶ Texture ──▶ Render (GPU)
                           НЕТ копирования в RAM!
```
- Decoded frame остаётся как GPU texture
- Рендер через Qt Quick Scene Graph (OpenGL/Vulkan backend)
- Custom QQuickItem для отображения decoded texture

### Frame Timing
- Нет буферизации! Показывать кадр сразу после декодирования
- Vsync-aware: синхронизация с монитором клиента
- Отображение overlay с метриками (FPS, latency, bitrate, codec)

---

## Модуль 5: Input Handling

### Client → Host
- Перехват raw input events (не processed Qt events)
- Windows: Raw Input API для мыши, клавиатуры
- Отправка немедленно, без батчинга
- Формат: {timestamp, event_type, data} — минимальный размер пакета
- Поддержка:
  - Клавиатура (scancode-based, не keycode — для layout independence)
  - Мышь (relative movement, absolute position, scroll)
  - Gamepad (XInput на Windows) — в будущем

### Host: Input Injection
- Windows: SendInput API для клавиатуры и мыши
- Корректная обработка modifier keys (Ctrl, Alt, Shift, Win)
- Caps Lock / Num Lock синхронизация между клиент и хост

### Целевая метрика
- Input-to-display latency: < 20ms на LAN (включая capture + encode + network + decode + render)

---

## Модуль 6: Audio Streaming

- **Capture (Host):** WASAPI Loopback (Windows) / PulseAudio monitor (Linux)
- **Codec:** Opus (48kHz, stereo, ~128kbps)
- **Transport:** отдельный UDP поток
- **Playback (Client):** WASAPI (Windows) / PulseAudio (Linux)
- **Sync:** audio/video синхронизация через timestamps
- **Jitter buffer:** адаптивный, 20-60ms

---

## Модуль 7: Auxiliary Features

### Clipboard Sync
- Двусторонняя синхронизация текстового буфера обмена
- Поддержка форматов: plain text, Rich Text, images (PNG)
- Через TCP control channel

### File Transfer
- Drag & drop или explicit file send
- Через TCP с progress reporting
- Chunked transfer для больших файлов

### Session Management
- ID-based подключение (как RustDesk/TeamViewer)
- Password / PIN authentication
- Unattended access mode
- Session recording (опционально, v2)

---

## Фазы разработки (MVP → Product)

### Фаза 1: Proof of Concept (1-2 месяца)
**Цель:** видеопоток 1080p60 по LAN с минимальной задержкой

Scope:
- [ ] Screen capture через DXGI Desktop Duplication
- [ ] H.264 encoding через NVENC (zero-copy от DXGI texture)
- [ ] Простой UDP transport (без encryption, без FEC)
- [ ] H.264 HW decoding на клиенте (D3D11VA)
- [ ] Рендер decoded frame в Qt Quick window
- [ ] Базовый input: мышь + клавиатура (SendInput)
- [ ] Прямое подключение по IP:port (без NAT traversal)
- [ ] CLI для запуска host/client
- [ ] Overlay с метриками: FPS, encode time, decode time, RTT

**Критерий успеха:** < 15ms end-to-end latency на LAN, стабильные 60fps, визуально чёткая картинка

### Фаза 2: Usable Product (2-3 месяца)
- [ ] Audio streaming (WASAPI + Opus)
- [ ] AMF и QSV поддержка (AMD, Intel)
- [ ] Software encoding fallback (x264)
- [ ] FEC для video channel
- [ ] Шифрование (Noise_NK + libsodium)
- [ ] Adaptive bitrate
- [ ] Clipboard sync
- [ ] Qt/QML GUI для host и client
- [ ] Конфигурация: resolution, fps, bitrate, codec
- [ ] Multi-monitor support (выбор монитора)
- [ ] Linux client
- [ ] Базовый installler (Windows)

### Фаза 3: Public Release (2-3 месяца)
- [ ] NAT traversal (STUN + hole punching)
- [ ] Self-hosted relay server
- [ ] ID-based connections (rendezvous server)
- [ ] Authentication (password, PIN)
- [ ] Unattended access
- [ ] File transfer
- [ ] Auto-update mechanism
- [ ] Linux host support
- [ ] GitHub README с скринкастами, бенчмарками
- [ ] CI/CD: автоматические билды для Windows/Linux
- [ ] Документация: setup guide, build guide, architecture docs

### Фаза 4: Monetization (0.1 launch + ongoing)

#### Open / Closed разделение
- **Open source (AGPL-3.0):**
  - `vivoradesk/vivora` — клиент + хост (всё ядро функциональности)
  - `vivoradesk/vivora-relay` — relay daemon (self-host)
  - `vivoradesk/vivora-rendezvous` — rendezvous server (self-host)
- **Closed source (proprietary):**
  - `vivora-cloud` — account system, license server, address book sync, managed relay auth
  - `vivora-console` — web admin console для команд (будущее)

#### Free tier (open source, AGPL-3.0)
- Полное качество стрима, все фичи клиента
- Direct IP подключения без ограничений
- Self-hosted relay/rendezvous (любой может поднять полную инфраструктуру)
- Локальный address book (без cloud sync)
- Personal use only (AGPL обязывает open-source при коммерческом использовании)
- Multi-monitor, all codecs, all features

#### Pro tier — $9.90/мес или $99/год (для 0.1 launch)
- Commercial use license (dual licensing, без AGPL обязательств)
- Доступ к public managed relay (на инфраструктуре Vivora)
- Cloud sync address book между устройствами
- Priority support
- Чекаут через Paddle (Merchant of Record для UA)

**Ключевой принцип:** все Pro-фичи завязаны на серверную инфраструктуру (account auth, managed relay, cloud sync). Технически невозможно "обойти" Pro патчингом клиента — клиент сам ничего не блокирует, но без валидного Pro-аккаунта закрытые endpoints на сервере не отвечают.

#### Технические компоненты для 0.1 launch monetization
- [ ] License key system (JWT с подписью, проверка на клиенте offline)
- [ ] Account backend (email + password, привязка лицензии)
- [ ] Paddle integration + webhook для генерации license keys
- [ ] Address book cloud sync API
- [ ] Relay auth (только Pro-аккаунты используют public relay)

#### Будущие Pro-фичи (отложено до 0.3+)
- [ ] Web-based admin console (для команд)
- [ ] Device management dashboard
- [ ] Audit logs
- [ ] LDAP / SAML SSO
- [ ] Custom branding / white-label client
- [ ] API для интеграций
- [ ] Enterprise deployment tools

#### Юридическая защита через AGPL
- AGPL заставляет коммерческих пользователей либо открывать свой код под AGPL, либо покупать commercial license
- Это стандартная dual-licensing модель (как у MongoDB, MySQL, Grafana)

---

## Структура проекта

```
vivora/
├── CMakeLists.txt
├── README.md
├── LICENSE
├── docs/
│   ├── architecture.md
│   ├── protocol.md
│   └── building.md
├── src/
│   ├── common/           # Общий код host/client
│   │   ├── protocol/     # Сетевой протокол, framing, encryption
│   │   ├── codec/        # Абстракция кодеков (encode/decode)
│   │   └── utils/        # Логирование, конфиг, метрики
│   ├── host/             # Серверная часть
│   │   ├── capture/      # Screen capture (DXGI, DRM)
│   │   ├── encode/       # Video encoding (NVENC, AMF, QSV, x264)
│   │   ├── audio/        # Audio capture (WASAPI, PulseAudio)
│   │   ├── input/        # Input injection (SendInput, uinput)
│   │   └── session/      # Session management
│   ├── client/           # Клиентская часть
│   │   ├── decode/       # Video decoding (D3D11VA, VAAPI)
│   │   ├── render/       # Qt Quick rendering
│   │   ├── audio/        # Audio playback
│   │   ├── input/        # Input capture
│   │   └── ui/           # QML UI
│   └── relay/            # Relay server (standalone)
├── qml/                  # QML UI файлы
├── third_party/          # Внешние зависимости
│   ├── nvenc/            # NVIDIA Video Codec SDK headers
│   ├── amf/              # AMD AMF SDK headers
│   └── opus/             # Opus codec
├── tests/
│   ├── capture_test.cpp
│   ├── encode_test.cpp
│   ├── protocol_test.cpp
│   └── latency_bench.cpp
└── scripts/
    ├── build.sh
    └── package.sh
```

---

## Внешние зависимости

### Обязательные
- **Qt 6.5+** — UI, networking utilities, platform abstraction
- **NVIDIA Video Codec SDK** — NVENC encode/NVDEC decode (header-only)
- **FFmpeg** (libavcodec, libavutil) — fallback codecs, pixel format conversion
- **Opus** — audio codec
- **libsodium** — Noise Protocol (Curve25519, ChaCha20-Poly1305)

### Опциональные
- **AMD AMF SDK** — AMD HW encoding
- **Intel oneVPL** — Intel QSV encoding
- **x264** — software encoding fallback

---

## Метрики и бенчмарки

Каждый модуль должен логировать latency для профилирования:

```
[CAPTURE]  frame_time=0.8ms  
[ENCODE]   encode_time=4.2ms  codec=h264_nvenc  bitrate=15Mbps
[NETWORK]  rtt=0.5ms  loss=0.0%  bandwidth=18Mbps
[DECODE]   decode_time=2.1ms  codec=h264_d3d11va
[RENDER]   render_time=0.3ms  vsync=on
[TOTAL]    pipeline=7.9ms  fps=60  resolution=1920x1080
```

Overlay в клиенте (toggle по горячей клавише) должен показывать эти метрики в реальном времени.

---

## Конкурентный контекст

| Feature | Vivora (цель) | RustDesk | Parsec | Sunshine+Moonlight |
|---------|-------------------|----------|--------|--------------------|
| Pipeline latency (LAN) | < 10ms | 50-200ms | 4-8ms | 10-15ms |
| Max FPS | 120 | 60 (реально 30) | 240 | 120 |
| GPU encode | NVENC/AMF/QSV | Есть, но buggy | NVENC/AMF/QSV | NVENC/AMF/QSV |
| Zero-copy pipeline | Да | Нет | Да | Частично |
| Transport | Custom UDP | TCP/KCP | Custom UDP (BUD) | NVIDIA GameStream |
| NAT traversal | STUN + relay | TCP punch + relay | Custom (97% success) | Нет (нужен VPN) |
| Open source | Да | Да | Нет | Да |
| Self-hosted | Да | Да | Нет | Да |
| Multi-platform | Win/Linux/Mac | All | Win/Mac (no Linux host) | Win/Linux/Mac |
| Remote desktop features | Full | Full | Limited | Limited |

---

## Принципы разработки

1. **Latency is king** — каждое архитектурное решение оценивается через призму задержки
2. **Zero-copy everywhere** — данные не покидают GPU без необходимости
3. **Measure everything** — каждый модуль имеет встроенное профилирование
4. **Modular design** — каждый компонент заменяем (capture, encoder, protocol, decoder, renderer)
5. **Platform abstraction** — платформо-зависимый код изолирован за интерфейсами
6. **No unnecessary dependencies** — минимальный набор библиотек, никаких фреймворков-монстров
7. **Security by default** — шифрование не опционально (кроме debug mode)

---

## Naming

- **Vivora** — финальное название проекта
- Мелодичное, уникальное в tech-нише, легко произносится на любом языке
- GitHub: github.com/vivoradesk ✅
- Домен: vivora.dev ✅
- Email: hello@vivora.dev (через Cloudflare Email Routing)

---

## Заметки для Claude Code

- Начинай с Фазы 1 — proof of concept
- Первый milestone: получить хоть какую-то картинку с хоста на клиент по UDP
- Не оптимизируй преждевременно, но закладывай правильную архитектуру сразу
- Используй абстракции для platform-specific кода с первого дня
- Пиши тесты для protocol layer — это самая критичная часть
- Комментарии в коде на английском
- Git commits на английском, с conventional commits format
