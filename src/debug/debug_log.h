#pragma once

namespace timmy::debug {

void init_console();
void shutdown_console();
void log(const char* fmt, ...);

} // namespace timmy::debug
