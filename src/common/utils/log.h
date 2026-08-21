// SPDX-FileCopyrightText: 2026 Andrii Myronov
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <cstdio>
#include <cstdarg>

namespace vivora::log {

enum class Level { Debug, Info, Warn, Error };

// Default level is Debug in developer builds and Info in release builds;
// VIVORA_LOG_LEVEL=debug|info|warn|error overrides it at startup.
void set_level(Level level);
Level get_level();

// Redirect log output to a path on disk.  POSIX default sink is
// stderr; Windows default is null (no sink — first log goes
// nowhere) because /SUBSYSTEM:WINDOWS binaries have an unusable
// stderr that fastfails on first fprintf.  Returns true if the
// file was opened.
//
// The file is kept to one generation on each side of the current one:
// an existing log is moved to <path>.1 on open, and the live file is
// rolled over to <path>.1 again once it passes the size cap (8 MiB,
// override with VIVORA_LOG_MAX_MB).  Worst case on disk is 2x the cap.
bool set_file(const char* path);

// Point the log sink at stderr.  Used by the Windows CLI path
// after AttachConsole + freopen(stderr) — stderr is now valid and
// we want logs to land in the calling console rather than a file.
void use_stderr();

void debug(const char* tag, const char* fmt, ...);
void info(const char* tag, const char* fmt, ...);
void warn(const char* tag, const char* fmt, ...);
void error(const char* tag, const char* fmt, ...);

} // namespace vivora::log
