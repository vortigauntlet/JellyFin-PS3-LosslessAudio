// Streaming reader for the one JSON shape SoundCloud's listings have: a top-level object (or array) whose members
// are scalars, small objects, and arrays of objects.  Nothing is held but a read chunk and one element at a time,
// because the console has well under 200 KB of free memory while it plays and a page of 50 tracks is 150-250 KB.
//
//   {"collection":[ {...}, {...} ], "next_href":"...", "query_urn":"..."}        members + array elements
//   {"id":1,"title":"...","user":{...},"tracks":[ {...}, {...} ]}                  a playlist document
//   [ {...}, {...} ]                                                              a root array: elements have key ""
//
// For every element of an array that is a top-level member (or of a root array) elem() gets the element's JSON text.
// For every other top-level member, member() gets the value's JSON text (a string includes its quotes), when it fits.
// A value larger than the work buffer is skipped and counted, never truncated.  Both texts are valid only during the
// call and are not NUL-terminated.  Hand them to sx_json (sx_j_root).
//
// Pure C with no platform includes: the host suite runs this file as it is.
#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Returns false to stop reading.
    bool (*elem)(void *ud, const char *key, const char *json, size_t len);
    void (*member)(void *ud, const char *key, const char *json, size_t len);
    void *ud;
} sc_jcb;

// Reads up to n bytes: > 0 bytes, 0 at the end, < 0 on a failure.
typedef int (*sc_jread)(void *ctx, void *buf, int n);

enum {
    SC_J_OK       =  0,
    SC_J_E_READ   = -1,       // the reader failed
    SC_J_E_SYNTAX = -2,       // not the expected shape
    SC_J_E_MEM    = -3
};

typedef struct {
    int elements;             // elem() calls
    int skipped;              // values too large for the work buffer
    bool stopped;             // elem() asked to stop
} sc_jstats;

// work_cap bytes of the heap are used for the largest element or member kept (it is allocated here).
int sc_jstream_run(sc_jread rd, void *rctx, const sc_jcb *cb, size_t work_cap, sc_jstats *stats);

#ifdef __cplusplus
}
#endif
