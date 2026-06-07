# macOS code signing for stable TCC grants (dev)

The app needs Screen Recording (capture) and Accessibility (input injection)
TCC grants. With **ad-hoc** signing (`codesign --sign -`) the designated
requirement is the binary's cdhash, which changes on every build — so macOS
forgets the grant after each rebuild and (on macOS 15+/26) can re-prompt in a
loop even across plain relaunches.

Fix for development: sign with a **self-signed code-signing certificate**. Its
designated requirement is `identifier + certificate`, which is stable across
rebuilds and relaunches, so a granted permission sticks. No paid Apple
Developer account is needed (that's only for distribution / notarization — see
VIV-14).

## One-time: create the cert ("Vivora Dev") in the login keychain

Run on the Mac (no keychain password needed; `-A` lets any tool use the key,
which is fine for a dev cert):

```bash
cd /tmp
cat > vivora_cs.cnf <<'EOF'
[req]
distinguished_name = dn
x509_extensions    = v3
prompt             = no
[dn]
CN = Vivora Dev
[v3]
basicConstraints   = critical,CA:FALSE
keyUsage           = critical,digitalSignature
extendedKeyUsage   = critical,codeSigning
EOF
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout vivora_cs.key -out vivora_cs.crt -config vivora_cs.cnf
openssl pkcs12 -export -inkey vivora_cs.key -in vivora_cs.crt \
  -name "Vivora Dev" -out vivora_cs.p12 -passout pass:
security import vivora_cs.p12 -k ~/Library/Keychains/login.keychain-db -P "" -A
rm -f vivora_cs.key vivora_cs.crt vivora_cs.p12 vivora_cs.cnf
security find-identity -v -p codesigning   # should list "Vivora Dev"
```

## Build with it

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" \
  -DVIVORA_MAC_CODESIGN_IDENTITY="Vivora Dev"
cmake --build build
```

The post-build step signs `Vivora.app` with the cert and **skips** the
`tccutil reset` (only ad-hoc builds reset). Grant Screen Recording +
Accessibility once in System Settings → Privacy & Security; the grants now
survive subsequent rebuilds and relaunches.

If you ever switch back to ad-hoc, configure with
`-DVIVORA_MAC_CODESIGN_IDENTITY=-` (the default).
