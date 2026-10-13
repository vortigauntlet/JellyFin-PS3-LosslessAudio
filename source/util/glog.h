#pragma once

// Diagnostic GUI / thumbnail-cache log: "gui:" lines in player_log.txt,
// written by plog's thread, so a call never blocks on the disk.
// Gated by the SAME user logging toggle as plog (plog_enabled()): writes
// nothing at all while logging is off, so it costs nothing in normal use.
void glog(const char *msg);
void glogf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
