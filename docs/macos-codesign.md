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

The post-build step (`packaging/macos/codesign-app.sh`) signs `Vivora.app`
with the cert as `dev.vivora.app`, then runs `tccutil reset` for Screen
Recording and Accessibility. The reset is deliberate even though the grants
would survive: each test run starts from the permission flow a new user sees.
Launch the app and grant both again in System Settings → Privacy & Security.

Sign from the Mac's own Terminal. Over SSH the login keychain is locked,
codesign cannot reach the key, and the script falls back to ad-hoc with a
warning rather than failing the build.

If you ever switch back to ad-hoc, configure with
`-DVIVORA_MAC_CODESIGN_IDENTITY=-` (the default).

## Developer ID (the real certificate)

A Developer ID Application certificate replaces "Vivora Dev" the same way;
nothing else changes. Use the exact name `security find-identity` prints:

```bash
security find-identity -v -p codesigning
# "Developer ID Application: Your Name (TEAMID1234)"
cmake -B build -DVIVORA_MAC_CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID1234)"
cmake --build build
```

The certificate lives in the login keychain of the Mac it was created on
(Xcode → Settings → Accounts → Manage Certificates → + → Developer ID
Application). Only the Account Holder can create one, and Apple allows a
handful per team, so export it with its private key (Keychain Access →
right-click → Export, `.p12`) and keep the file somewhere safe rather than
making new ones per machine.

The first build asks "codesign wants to sign using key…": enter the login
password and choose **Always Allow**, or every build will ask again.

Check the result:

```bash
codesign -dv --verbose=4 build/bin/Vivora.app 2>&1 | grep -E 'Authority|TeamIdentifier'
codesign --verify --strict --verbose=2 build/bin/Vivora.app
```

Expect three `Authority=` lines ending in `Apple Root CA` and
`TeamIdentifier=` your team ID. The designated requirement is the identifier plus
the team, so TCC treats every build signed this way, on any Mac, as the same
app.

This is a signed build, not a distributable one. Gatekeeper still refuses a
downloaded copy until it is notarised, which needs the hardened runtime
(`--options runtime`) and a secure timestamp (`--timestamp`); the script
passes neither yet.
