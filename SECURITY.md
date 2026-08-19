# Security policy

Vivora hands one machine control of another's keyboard, mouse and screen. A
vulnerability here is not an inconvenience, so please report one privately and
give us a chance to ship a fix before it is public.

## Reporting

Use GitHub's [private vulnerability
reporting](https://github.com/vivoradesk/vivora/security/advisories/new) on this
repository, or email **security@vivora.dev**.

Please include enough to reproduce: version, platform, and whether the issue
needs an already-approved session or can be reached by an unauthenticated peer.
A proof of concept helps and will not be shared beyond the people fixing it.

We aim to acknowledge within 72 hours and to have either a fix or a concrete
plan within 14 days. You will be credited in the release notes unless you would
rather not be.

## Supported versions

Only the latest release. Before 1.0 there is no backport branch — fixes ship in
the next version.

## Scope

In scope, and taken seriously:

- Anything that lets a peer connect, view, or inject input without the host
  accepting the connection.
- Breaking or downgrading the Noise transport, defeating peer-key pinning, or
  bypassing the replay window.
- Remote crashes or memory-safety bugs reachable from the network. Parsers are
  the obvious surface: packet headers, FEC group reconstruction, fragment
  reassembly, clipboard messages, the rendezvous and relay protocols.
- Attacks by a rendezvous or relay operator against session confidentiality or
  integrity. Both are designed to see ciphertext only; anything that breaks
  that assumption is a real bug.
- Local privilege issues: what the installer writes, what the app reads, how
  the host identity key and the trusted-peer list are stored.

Out of scope:

- The screen being visible to whoever the host explicitly approved. That is the
  product.
- Bypassing the Pro license check in a local build. The client is AGPL and the
  check is offline by design; entitlements that matter are enforced
  server-side.
- Findings that require an attacker to already have code execution or physical
  access to one of the machines.
- Reports from automated scanners with no demonstrated impact.

## Known design decisions

Not bugs, but worth stating so nobody has to rediscover them:

- **Trust on first use.** The first connection to a host pins its public key.
  If someone can intercept that very first exchange, they can substitute their
  own key. Compare the fingerprint out of band when that matters.
- **`vivora --host` from the CLI has no approval prompt.** It is the
  server-style flow: anyone holding the peer code and passing the handshake
  gets in. Use the GUI if you want the prompt.
- **`VIVORA_AUTO_ACCEPT=1` disables the approval prompt** in the GUI too. It is
  a development hook and it is documented here so nobody is surprised to find
  it.
- **STUN goes to Google by default** (`stun.l.google.com:19302`). That server
  learns your IP, which it would learn from any connection anyway. Change or
  disable it in Settings or with `--no-stun`.
- **The relay sees traffic patterns**, though not plaintext: packet sizes and
  timing are not padded.
