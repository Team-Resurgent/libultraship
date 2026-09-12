#include <spdlog/spdlog.h>
#include <string>
#include <stdarg.h>
#include "libultraship/log/luslog.h"

#if defined(LUS_XBOX)
#include <cstdio>
#endif

extern "C" {
void luslog(const char* file, int32_t line, int32_t logLevel, const char* msg) {
#if defined(LUS_XBOX)
    // spdlog is a no-op shim on Xbox; route game/engine logs to stdout, which RXDK's
    // line-buffered libc forwards to the debug serial port (visible in xemu's -serial log).
    (void)file;
    (void)line;
    (void)logLevel;
    std::printf("[soh] %s\n", msg);
    std::fflush(stdout);
#else
    std::string str(msg);
    spdlog::level::level_enum lvl = (spdlog::level::level_enum)logLevel;
    auto loc = spdlog::source_loc{ file, line, SPDLOG_FUNCTION };

    spdlog::default_logger_raw()->log(loc, lvl, str);
#endif
}

void lusprintf(const char* file, int32_t line, int32_t logLevel, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buffer[4096];

    vsnprintf(buffer, sizeof(buffer), fmt, args);
    luslog(file, line, logLevel, buffer);
}
}