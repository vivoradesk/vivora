#pragma once

#include <string>
#include <cstdio>
#include <cstdarg>

namespace vivora::log {

enum class Level { Debug, Info, Warn, Error };

void set_level(Level level);
Level get_level();

void debug(const char* tag, const char* fmt, ...);
void info(const char* tag, const char* fmt, ...);
void warn(const char* tag, const char* fmt, ...);
void error(const char* tag, const char* fmt, ...);

} // namespace vivora::log
