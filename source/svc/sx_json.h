// Zero-copy JSON reading for the Stremio layer.  A value is a view into the
// caller's buffer; nothing is allocated.  Everything is bounds-checked against
// the view's end, nesting is capped, and malformed input yields "absent" rather
// than a crash, because the documents come from addon servers.
//
// Pure C with no platform includes: the host suite runs this file as it is.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { const char *p; const char *e; } sx_jv;     // p == NULL: no value

enum { SXJ_NONE = 0, SXJ_NULL, SXJ_BOOL, SXJ_NUM, SXJ_STR, SXJ_ARR, SXJ_OBJ };

#define SXJ_MAX_DEPTH 48

// The first value of a document (a UTF-8 BOM and whitespace are skipped).
sx_jv sx_j_root(const char *buf, size_t len);

int   sx_j_type(sx_jv v);
bool  sx_j_present(sx_jv v);

// One past the end of the value, or NULL when it is malformed or too deep.
const char *sx_j_end(sx_jv v);
// The whole document is one well-formed value (plus trailing whitespace).
bool  sx_j_valid(const char *buf, size_t len);

// Members of an object / elements of an array.  Both stop quietly at malformed
// input.
typedef struct { const char *p; const char *e; bool obj; bool done; } sx_jit;
sx_jit sx_j_iter(sx_jv container);
// key (may be NULL) is the member name's string value, val its value.
bool   sx_j_next(sx_jit *it, sx_jv *key, sx_jv *val);

bool  sx_j_get(sx_jv obj, const char *key, sx_jv *out);   // first member with that name
bool  sx_j_idx(sx_jv arr, int i, sx_jv *out);
int   sx_j_len(sx_jv container);                          // elements / members; 0 when not a container

// Decoding.  A string is decoded to UTF-8 (every escape, surrogate pairs
// included), truncated on a character boundary, always NUL-terminated.
// False when the value is not of that kind.
bool  sx_j_str(sx_jv v, char *out, size_t cap);
bool  sx_j_int(sx_jv v, int64_t *out);       // a number with no fraction, or a numeric string
bool  sx_j_dbl(sx_jv v, double *out);
bool  sx_j_bool(sx_jv v, bool *out);

// Member shortcuts.  The string/int variants leave `out`/return `def` when the
// member is absent or of another type.
bool    sx_j_get_str(sx_jv obj, const char *key, char *out, size_t cap);
int64_t sx_j_get_int(sx_jv obj, const char *key, int64_t def);
bool    sx_j_get_bool(sx_jv obj, const char *key, bool def);

// Escapes `in` as the inside of a JSON string (no surrounding quotes).
// Returns false when it does not fit.
bool  sx_json_escape(const char *in, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
