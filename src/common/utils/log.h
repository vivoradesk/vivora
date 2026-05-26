#pragma once

#include <string>
#include <cstdio>
#include <cstdarg>

namespace vivora::log {

enum class Level { Debug, Info, Warn, Error };

void set_level(Level level);
Level get_level();

// Redirect log output to a path on disk.  POSIX default sink is
// stderr; Windows default is null (no sink — first log goes
// nowhere) because /SUBSYSTEM:WINDOWS binaries have an unusable
// stderr that fastfails on first fprintf.  Returns true if the
// file was opened.  Truncates the file on open.
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
