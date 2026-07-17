#pragma once
// Minimal leveled logger. Deliberately printf-style and header-only: the
// server's hot path must never log, so there is nothing to optimize here.

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace hermit::logging {

enum class Level : int { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

inline std::atomic<int>& level_ref() {
  static std::atomic<int> level{static_cast<int>(Level::kInfo)};
  return level;
}

inline void set_level(Level l) { level_ref().store(static_cast<int>(l)); }

inline void vlog(Level l, const char* tag, const char* fmt, va_list ap) {
  if (static_cast<int>(l) < level_ref().load(std::memory_order_relaxed)) return;
  char ts[32];
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&now, &tm);
  std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
  std::fprintf(stderr, "%s %-5s ", ts, tag);
  std::vfprintf(stderr, fmt, ap);
  std::fputc('\n', stderr);
}

#define HERMIT_LOG_FN(name, level, tag)                            \
  inline void name(const char* fmt, ...)                           \
      __attribute__((format(printf, 1, 2)));                       \
  inline void name(const char* fmt, ...) {                         \
    va_list ap;                                                    \
    va_start(ap, fmt);                                             \
    vlog(level, tag, fmt, ap);                                     \
    va_end(ap);                                                    \
  }

HERMIT_LOG_FN(debug, Level::kDebug, "DEBUG")
HERMIT_LOG_FN(info, Level::kInfo, "INFO")
HERMIT_LOG_FN(warn, Level::kWarn, "WARN")
HERMIT_LOG_FN(error, Level::kError, "ERROR")

#undef HERMIT_LOG_FN

}  // namespace hermit::logging
