# Contributing to Vivora

Thanks for looking. This is a small project with an unusual constraint, so a
few things are worth saying before you spend an evening on a patch.

## Read this first

**Latency is the primary metric.** Every architectural decision is judged
through it. A change that makes the code cleaner and the pipeline slower will
be turned down, and a change that adds an allocation or a copy to the per-frame
path needs a reason. [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) explains
why the code looks the way it does — it is the fastest way to understand what
will and will not be accepted.

[`STATUS.md`](STATUS.md) is the honest per-platform state of everything,
including what is deliberately not implemented.

## Getting set up

```sh
git clone --recursive https://github.com/vivoradesk/vivora.git
cd vivora
scripts/install-git-hooks.sh          # refuses to commit signing keys
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Platform dependencies are in the [README](README.md#build-from-source). The
Windows build currently needs a statically built Qt 6.9 and links against Qt's
build tree rather than an install prefix; that is a known wart with a follow-up
issue open.

Run the tests in **both** configurations before sending a patch:

```sh
ctest --test-dir build -C Release
ctest --test-dir build -C Debug
```

Debug catches out-of-bounds access in the `[size, capacity)` window that a
Release build and ASan both miss.

## What makes a good change

- **Protocol changes get a test first.** `tests/` is the only place the wire
  format is actually pinned down. If you change a packet layout, a FEC
  parameter or a handshake step, the test comes with it.
- **Platform code lives behind an interface.** New OS-specific work goes behind
  the existing abstractions (`src/app/host_platform.h`, `view_platform.h`), not
  in an `#ifdef` in shared code.
- **Every pipeline stage reports its timing.** Capture, encode, network RTT,
  decode, render. A regression should be attributable, not guessed at.
- **Never present a damaged frame.** Any decode error drops the frame and
  requests an IDR. Visible corruption is worse than a brief freeze, and this
  rule is not negotiable.
- **FEC overhead comes out of the wire budget.** Raising the parity ratio must
  lower the encoder target by the same amount, never add on top of it.

If a change touches Windows, Linux and macOS behaviour, say in the pull request
which platforms you actually ran it on. "Compiles on the other two" is a
perfectly acceptable answer — silently implying you tested them is not.

## Style

- C++17, C++20 where it clearly helps. Match the file you are editing: this
  codebase uses long explanatory comments where the *why* is not obvious from
  the code, and no comments where it is.
- Comments and identifiers in English.
- One class per pair of files. Do not add a class body to `main.cpp`.
- Commit messages in [conventional commits](https://www.conventionalcommits.org/)
  form: `fix(host): ...`, `feat(protocol): ...`, `perf(client): ...`. The body
  should say what was wrong and why this is the fix, not restate the diff.

## Reporting bugs

Include the platform and version, and attach the log — it is at:

| | |
| --- | --- |
| Windows | `%LOCALAPPDATA%\Vivora\Vivora\vivora.log` |
| Linux | `~/.local/share/Vivora/Vivora/vivora.log` |
| macOS | `~/Library/Application Support/Vivora/vivora.log` |

The previous session is kept alongside it as `vivora.log.1`, which is usually
the one you want after a crash. For a verbose run, start the app with
`VIVORA_LOG_LEVEL=debug`.

For stream-quality reports, the F9 diagnostics HUD in the stream window is
worth a screenshot: it shows the negotiated codec, framerate, bitrate, FEC
ratio and RTT.

Security issues do **not** go in the issue tracker — see
[`SECURITY.md`](SECURITY.md).

## Licensing of contributions

Vivora is AGPL-3.0-or-later. By opening a pull request you agree your
contribution ships under that license. There is no CLA.
