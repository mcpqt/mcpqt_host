// SPDX-License-Identifier: AGPL-3.0-only
#include "log.h"

#include <QByteArray>
#include <QString>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <QMutex>
#include <sstream>

namespace mcp
{

  namespace
  {

    // The sink itself is mutex-guarded; swaps are rare, calls are frequent.
    QMutex g_sink_mutex;
    LogSink g_sink; // empty until first call to ensure_default_sink().

    // Once a sink is claimed (typically by a logging plugin), it is exclusive
    // for the rest of the process: no other claim may replace it. Guarded by
    // g_sink_mutex for writes; read lock-free by log_message().
    std::atomic<bool> g_claimed{ false };

    const char* level_name(LogLevel l) noexcept
    {
      switch(l)
      {
      case LogLevel::trace: return "TRACE";
      case LogLevel::debug: return "DEBUG";
      case LogLevel::info:  return "INFO";
      case LogLevel::warn:  return "WARN";
      case LogLevel::error: return "ERROR";
      case LogLevel::off:   return "OFF";
      }
      return "?";
    }

    void default_sink(LogLevel level, const QString& msg)
    {
      // Compose the line under a single mutex so multi-thread interleaving
      // doesn't garble output. fwrite is line-buffered on stderr by default.
      static QMutex io_mutex;
      QMutexLocker lk(&io_mutex);

      using clock = std::chrono::system_clock;
      const auto now = clock::now();
      const auto secs
          = std::chrono::time_point_cast<std::chrono::seconds>(now);
      const auto micros
          = std::chrono::duration_cast<std::chrono::microseconds>(now - secs)
                .count();
      const std::time_t tt = clock::to_time_t(now);
      std::tm tm_buf{};
#if defined(_WIN32)
      localtime_s(&tm_buf, &tt);
#else
      localtime_r(&tt, &tm_buf);
#endif

      const char* level_text = level_name(level);
      const QByteArray text = msg.toUtf8();
      std::fprintf(stderr,
                   "[%04d-%02d-%02d %02d:%02d:%02d.%06lld %-5s] %.*s\n",
                   tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                   tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                   static_cast<long long>(micros), level_text,
                   static_cast<int>(text.size()), text.constData());
      std::fflush(stderr);
    }

  } // namespace

  bool claim_log_sink(LogSink sink)
  {
    if(!sink)
    {
      return false;
    }
    QMutexLocker lk(&g_sink_mutex);
    if(g_claimed.load(std::memory_order_relaxed))
    {
      return false; // someone else already owns logging
    }
    g_sink = std::move(sink);
    g_claimed.store(true, std::memory_order_release);
    return true;
  }

  bool log_sink_claimed(void) noexcept
  {
    return g_claimed.load(std::memory_order_acquire);
  }

  void log_message(LogLevel level, const QString& msg,
                   std::source_location /*loc*/)
  {
    LogSink sink;
    {
      QMutexLocker lk(&g_sink_mutex);
      if(!g_sink)
        g_sink = &default_sink;
      sink = g_sink;
    }
    sink(level, msg);
  }

} // namespace mcp
