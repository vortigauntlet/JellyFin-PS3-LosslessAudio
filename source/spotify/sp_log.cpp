// The logger cspot and bell write through.  Their stock one prints to stdout, which on a console goes nowhere useful, and cspot
// logs things that must never reach player_log.txt: the signed CDN address of the audio, track names, account names.  So this
// one is an allow-list: only a fixed set of progress and error messages is passed to plog, truncated; everything else is dropped.
#include "sp_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "BellLogger.h"
#include "plog.h"

namespace {

// Messages that carry no address, name, key or token.  Matched as substrings of the formatted line.
const char* const kAllow[] = {
    "Got audio key",           "Failed to get audio key", "Received APHello",      "Authorization successful", "Authorization declined",
    "Unknown auth fail code",  "Connected to spotify",    "Closing socket",        "Received access key",      "Received CDN URL",
    "Cannot fetch CDN URL",    "Failed to fetch access token", "Access token sucessfully", "Header and footer bytes",
    "Track failed to load",    "File not available",      "Playing",               "EOF",                      "Resetting state",
    "Seeking",                 "Reconnection",            "Cannot reconnect",      "Connection lost",          "Disconnecting mercury",
    "Time synced",             "Mac doesn't match",       "No tracks in frame",    "Another player took",      "Error while receiving",
    "An error has occured",    "Failed to queue",         "AP connect failed",     "getaddrinfo",
};

// Lines that match the allow-list but carry a value after the fixed text that must not be logged are cut at that point.
const char* const kCutAfter[] = {"Received CDN URL", "Error while receiving"};

void emit(const char* tag, const char* line) {
  for (const char* a : kAllow) {
    if (strstr(line, a)) {
      char out[120];
      snprintf(out, sizeof out, "spotify: %s%s", tag, line);
      for (const char* c : kCutAfter) {
        const char* at = strstr(out, c);
        if (at) out[(at - out) + strlen(c)] = '\0';
      }
      plog(out);
      return;
    }
  }
}

class SpLogger : public bell::AbstractLogger {
 public:
  void debug(std::string, int, std::string, const char*, ...) override {}
  void error(std::string, int, std::string, const char* format, ...) override {
    char line[160];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    emit("E ", line);
  }
  void info(std::string, int, std::string, const char* format, ...) override {
    char line[160];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    emit("", line);
  }
};

SpLogger s_logger;

}  // namespace

void sp_log_install() { bell::bellGlobalLogger = &s_logger; }
