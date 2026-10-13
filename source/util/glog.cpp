// Diagnostic GUI / thumbnail-cache log.  Lines go through plog's ring (the
// player log's own writer thread does the file I/O), prefixed "gui:".
//
// It used to fprintf+fflush to GUIlog.txt itself, under a spin lock, on the
// CALLING thread -- and thumb_request() calls it from the render thread for
// every poster it queues.  When posters do not arrive the slots churn and are
// queued again and again, so the menu was doing HDD writes every frame (and
// spinning behind the low-priority fetch thread whenever that one held the
// lock mid-write).  Everything is a no-op unless logging is on (plog_enabled()).

#include <stdio.h>
#include <stdarg.h>

#include "glog.h"
#include "plog.h"

void glog(const char *msg) {
    if (!plog_enabled()) return;
    char buf[200];
    snprintf(buf, sizeof(buf), "gui: %s", msg);
    plog(buf);
}

void glogf(const char *fmt, ...) {
    if (!plog_enabled()) return;   // skip the vsnprintf entirely when off
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    glog(buf);
}
