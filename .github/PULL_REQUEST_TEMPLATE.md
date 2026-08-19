## What this changes

<!-- What was wrong and why this is the fix. The diff already says what you
     did; this section is for why. -->

## Platforms actually run on

<!-- Tick what you ran, not what you expect to work.  "Compiles on the other
     two" is a fine answer -- silently implying you tested them is not. -->

- [ ] Windows
- [ ] Linux
- [ ] macOS
- [ ] Compiles only on the untested ones

## Checks

- [ ] `ctest --test-dir build -C Release` passes
- [ ] `ctest --test-dir build -C Debug` passes (catches OOB that Release misses)
- [ ] Protocol/FEC/handshake change? A test in `tests/` comes with it
- [ ] Per-frame path unchanged, or the latency impact is measured and stated
