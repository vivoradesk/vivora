Vivora - portable build for Windows
===================================

Low-latency remote desktop.  https://vivora.dev

This is the no-installer build: unzip it anywhere you can write to and run
vivora.exe.  Nothing is written outside your user profile, and nothing is
registered with the system, so removing the folder removes the program.

If you would rather have a Start menu entry and an uninstaller, use the .msi
from the same release instead.  It installs per-user and never asks for
administrator rights either.


Running it
----------

  vivora.exe                     the app
  vivora.exe --host              share this screen, no window
  vivora.exe --view <address>    connect to a host, no window
  vivora.exe --help              every flag

Open it on both machines.  The one being shared shows a peer code like
swift-tiger-4271; type that on the other machine and press Connect.  The
sharing side has to accept the prompt.  No account, nothing to configure.


"Windows protected your PC"
---------------------------

These binaries are not code-signed yet, so SmartScreen shows that warning the
first time.  Choose "More info", then "Run anyway".

Before you do, check the download against SHA256SUMS.txt from the release
page - that is a stronger guarantee than a signature you cannot inspect:

  certutil -hashfile Vivora-<version>-windows-x64.zip SHA256


What is in this folder
----------------------

  vivora.exe                the whole application
  libcrypto-1_1-x64.dll     required by the Qt build we link against; no
  libssl-1_1-x64.dll        Vivora traffic goes through them (TLS uses
                            Schannel, the media path uses Monocypher)
  LICENSE                   GNU AGPL v3
  THIRD-PARTY-NOTICES.md    what else is in the binary and under what terms

The app will not start without the two DLLs - keep the folder together.


Where it keeps things
---------------------

  Settings          HKCU\Software\Vivora\Vivora
  Identity, peers   %APPDATA%\Vivora\
  Log               %LOCALAPPDATA%\Vivora\Vivora\vivora.log
                    (the previous session is kept as vivora.log.1)

To report a problem, attach the log.  For a verbose run, set
VIVORA_LOG_LEVEL=debug before starting.  In a stream window, F9 shows the
diagnostics overlay - include it for anything about stream quality.


Source
------

https://github.com/vivoradesk/vivora - AGPL-3.0-or-later.  The host, the
client, the rendezvous server and the relay are all there, and you can run
every piece of the infrastructure yourself.
