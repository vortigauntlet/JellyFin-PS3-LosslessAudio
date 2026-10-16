#include "sc_api.h"
#include "sx_json.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- small helpers -------------------------------------------------------------------------------

static void cpy(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static bool contains(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }

static bool put(char *out, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static bool put(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n >= 0 && (size_t)n < cap;
}

bool sc_urlenc(const char *in, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        const unsigned char c = *p;
        const bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) { if (o + 1 >= cap) return false; out[o++] = (char)c; }
        else       { if (o + 3 >= cap) return false; out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    if (o >= cap) return false;
    out[o] = 0;
    return true;
}

// ---- page container ------------------------------------------------------------------------------

void sc_page_init(sc_page *p) { memset(p, 0, sizeof(*p)); }

void sc_page_free(sc_page *p) {
    free(p->tr); free(p->pl);
    memset(p, 0, sizeof(*p));
}

static sc_track *page_add_track(sc_page *p) {
    if (p->ntr == p->cap_tr) {
        const int nc = p->cap_tr ? p->cap_tr * 2 : 32;
        sc_track *g = (sc_track *)realloc(p->tr, sizeof(*g) * (size_t)nc);
        if (!g) return NULL;
        p->tr = g; p->cap_tr = nc;
    }
    return &p->tr[p->ntr++];
}

static sc_playlist *page_add_playlist(sc_page *p) {
    if (p->npl == p->cap_pl) {
        const int nc = p->cap_pl ? p->cap_pl * 2 : 16;
        sc_playlist *g = (sc_playlist *)realloc(p->pl, sizeof(*g) * (size_t)nc);
        if (!g) return NULL;
        p->pl = g; p->cap_pl = nc;
    }
    return &p->pl[p->npl++];
}

// ---- track ---------------------------------------------------------------------------------------

static int preset_kbps(const char *preset) {                 // "aac_160k" -> 160
    const char *p = strchr(preset, '_');
    return p ? atoi(p + 1) : 0;
}

// Scores one transcoding; 0 = unusable.  Higher is better.
static int score_tc(const char *preset, const char *proto, const char *mime, bool snipped,
                    const char *url, int pref, int *kind, int *kbps, bool *enc_seen, bool *snip_seen) {
    if (strcmp(proto, "cbc-encrypted-hls") == 0 || strcmp(proto, "ctr-encrypted-hls") == 0 ||
        contains(proto, "encrypted")) {
        *enc_seen = true;
        return 0;
    }
    if (snipped || contains(url, "/preview/")) { *snip_seen = true; return 0; }
    if (strcmp(proto, "hls") == 0 && contains(mime, "mp4a.40.2") && strncmp(preset, "aac_", 4) == 0) {
        const int k = preset_kbps(preset);
        *kind = SC_TC_AAC_HLS; *kbps = k;
        if (pref <= 96) return k <= 96 ? 50 : 40;               // the lower preset first, the other as a fallback
        return k >= 160 ? 50 : 40;
    }
    if (strcmp(proto, "progressive") == 0 && contains(mime, "mpeg")) {
        *kind = SC_TC_MP3_PROGRESSIVE; *kbps = 128;
        return 10;
    }
    return 0;
}

static bool track_from(sx_jv t, int pref, sc_track *o) {
    memset(o, 0, sizeof(*o));
    if (sx_j_type(t) != SXJ_OBJ) return false;
    o->id = sx_j_get_int(t, "id", 0);
    if (o->id <= 0) return false;
    sx_j_get_str(t, "title", o->title, sizeof(o->title));
    o->dur_ms = (uint32_t)sx_j_get_int(t, "duration", 0);

    sx_jv user = {NULL, NULL};
    if (sx_j_get(t, "user", &user) && sx_j_type(user) == SXJ_OBJ) {
        sx_j_get_str(user, "username", o->artist, sizeof(o->artist));
    }
    sx_jv v;
    if (sx_j_get(t, "artwork_url", &v) && sx_j_type(v) == SXJ_STR) sx_j_str(v, o->art, sizeof(o->art));
    if (!o->art[0] && sx_j_type(user) == SXJ_OBJ) sx_j_get_str(user, "avatar_url", o->art, sizeof(o->art));
    sx_j_get_str(t, "permalink_url", o->permalink, sizeof(o->permalink));

    sx_jv media;
    if (!sx_j_get(t, "media", &media) || sx_j_type(media) != SXJ_OBJ) {
        o->hidden = SC_HID_STUB;                                 // a playlist stub carries no media
        return true;
    }
    if (!sx_j_get_bool(t, "streamable", true)) { o->hidden = SC_HID_NOT_STREAMABLE; return true; }

    sx_jv tcs;
    bool enc = false, snip = false;
    int best = 0;
    if (sx_j_get(media, "transcodings", &tcs) && sx_j_type(tcs) == SXJ_ARR) {
        sx_jit it = sx_j_iter(tcs);
        sx_jv ev;
        while (sx_j_next(&it, NULL, &ev)) {
            if (sx_j_type(ev) != SXJ_OBJ) continue;
            char preset[24] = "", url[sizeof(o->tc_url)] = "", proto[40] = "", mime[64] = "";
            sx_j_get_str(ev, "preset", preset, sizeof(preset));
            sx_jv f;
            if (sx_j_get(ev, "format", &f) && sx_j_type(f) == SXJ_OBJ) {
                sx_j_get_str(f, "protocol", proto, sizeof(proto));
                sx_j_get_str(f, "mime_type", mime, sizeof(mime));
            }
            if (!sx_j_get_str(ev, "url", url, sizeof(url))) continue;
            const bool snipped = sx_j_get_bool(ev, "snipped", false);
            int kind = 0, kbps = 0;
            const int s = score_tc(preset, proto, mime, snipped, url, pref, &kind, &kbps, &enc, &snip);
            if (s > best) {
                best = s;
                o->tc_kind = kind; o->tc_kbps = kbps;
                cpy(o->tc_url, sizeof(o->tc_url), url);
            }
        }
    }
    if (best == 0) o->hidden = enc ? SC_HID_ENCRYPTED : snip ? SC_HID_PREVIEW : SC_HID_UNSUPPORTED;
    return true;
}

bool sc_parse_track(const char *json, size_t len, int pref, sc_track *out) {
    return track_from(sx_j_root(json, len), pref, out);
}

static void add_track(sc_page *p, sx_jv t, int pref) {
    sc_track tmp;
    if (!track_from(t, pref, &tmp)) return;
    sc_track *d = page_add_track(p);
    if (!d) return;
    *d = tmp;
    p->hidden[tmp.hidden]++;
}

// ---- playlist ------------------------------------------------------------------------------------

static bool playlist_from(sx_jv o, sc_playlist *pl) {
    memset(pl, 0, sizeof(*pl));
    if (sx_j_type(o) != SXJ_OBJ) return false;
    pl->id = sx_j_get_int(o, "id", 0);
    if (pl->id <= 0) return false;
    sx_j_get_str(o, "title", pl->title, sizeof(pl->title));
    pl->track_count = (int)sx_j_get_int(o, "track_count", 0);
    sx_jv u;
    if (sx_j_get(o, "user", &u) && sx_j_type(u) == SXJ_OBJ) sx_j_get_str(u, "username", pl->owner, sizeof(pl->owner));
    sx_jv v;
    if (sx_j_get(o, "artwork_url", &v) && sx_j_type(v) == SXJ_STR) sx_j_str(v, pl->art, sizeof(pl->art));
    sx_j_get_str(o, "permalink_url", pl->permalink, sizeof(pl->permalink));
    return true;
}

static void add_playlist(sc_page *p, sx_jv o) {
    sc_playlist tmp;
    if (!playlist_from(o, &tmp)) return;
    sc_playlist *d = page_add_playlist(p);
    if (d) *d = tmp;
}

// The kind of an item, looking through the wrapper that likes and reposts add.
static int take_item(sc_page *p, sx_jv item, int pref) {
    if (sx_j_type(item) != SXJ_OBJ) return 0;
    sx_jv inner;
    if (sx_j_get(item, "track", &inner) && sx_j_type(inner) == SXJ_OBJ)    { add_track(p, inner, pref); return 1; }
    if (sx_j_get(item, "playlist", &inner) && sx_j_type(inner) == SXJ_OBJ) { add_playlist(p, inner); return 1; }
    char kind[24] = "";
    sx_j_get_str(item, "kind", kind, sizeof(kind));
    if (strcmp(kind, "track") == 0)    { add_track(p, item, pref); return 1; }
    if (strcmp(kind, "playlist") == 0 || strcmp(kind, "system-playlist") == 0) { add_playlist(p, item); return 1; }
    return 0;
}

int sc_parse_page(const char *json, size_t len, int pref, sc_page *out) {
    const sx_jv root = sx_j_root(json, len);
    sx_jv coll;
    if (sx_j_type(root) != SXJ_OBJ || !sx_j_get(root, "collection", &coll) || sx_j_type(coll) != SXJ_ARR) return -1;
    sx_j_get_str(root, "next_href", out->next_href, sizeof(out->next_href));
    int n = 0;
    sx_jit it = sx_j_iter(coll);
    sx_jv ev;
    while (sx_j_next(&it, NULL, &ev)) n += take_item(out, ev, pref);
    return n;
}

int sc_parse_tracks_array(const char *json, size_t len, int pref, sc_page *out) {
    const sx_jv root = sx_j_root(json, len);
    if (sx_j_type(root) != SXJ_ARR) return -1;
    int n = 0;
    sx_jit it = sx_j_iter(root);
    sx_jv ev;
    while (sx_j_next(&it, NULL, &ev)) { add_track(out, ev, pref); n++; }
    return n;
}

bool sc_parse_playlist(const char *json, size_t len, int pref, sc_playlist *meta, sc_page *out) {
    const sx_jv root = sx_j_root(json, len);
    if (!playlist_from(root, meta)) return false;
    sx_jv tracks;
    if (sx_j_get(root, "tracks", &tracks) && sx_j_type(tracks) == SXJ_ARR) {
        sx_jit it = sx_j_iter(tracks);
        sx_jv ev;
        while (sx_j_next(&it, NULL, &ev)) add_track(out, ev, pref);
    }
    return true;
}

int sc_fill_stubs(sc_page *p, const sc_page *full) {
    int filled = 0;
    for (int i = 0; i < p->ntr; i++) {
        if (p->tr[i].hidden != SC_HID_STUB) continue;
        for (int k = 0; k < full->ntr; k++) {
            if (full->tr[k].id != p->tr[i].id) continue;
            p->hidden[SC_HID_STUB]--;
            p->tr[i] = full->tr[k];
            p->hidden[p->tr[i].hidden]++;
            filled++;
            break;
        }
    }
    return filled;
}

// ---- user and stream URL ---------------------------------------------------------------------------

bool sc_parse_user(const char *json, size_t len, sc_user *out) {
    memset(out, 0, sizeof(*out));
    const sx_jv root = sx_j_root(json, len);
    if (sx_j_type(root) != SXJ_OBJ) return false;
    char kind[16] = "";
    sx_j_get_str(root, "kind", kind, sizeof(kind));
    if (strcmp(kind, "user") != 0) return false;
    out->id = sx_j_get_int(root, "id", 0);
    if (out->id <= 0) return false;
    sx_j_get_str(root, "username", out->name, sizeof(out->name));
    sx_j_get_str(root, "permalink", out->permalink, sizeof(out->permalink));
    sx_jv v;
    if (sx_j_get(root, "avatar_url", &v) && sx_j_type(v) == SXJ_STR) sx_j_str(v, out->avatar, sizeof(out->avatar));
    return true;
}

bool sc_parse_stream_url(const char *json, size_t len, char *out, size_t cap) {
    const sx_jv root = sx_j_root(json, len);
    if (sx_j_type(root) != SXJ_OBJ) return false;
    char tmp[1536];
    if (!sx_j_get_str(root, "url", tmp, sizeof(tmp)) || strlen(tmp) >= cap) return false;     // never a truncated URL
    cpy(out, cap, tmp);
    return strncmp(out, "https://", 8) == 0;                 // never follow a plain-http or odd-scheme URL
}

// ---- URLs ----------------------------------------------------------------------------------------

#define API "https://" SC_API_HOST "/"

bool sc_profile_url(const char *in, char *out, size_t cap) {
    while (*in == ' ' || *in == '\t') in++;
    if (strncmp(in, "https://", 8) == 0) in += 8;
    else if (strncmp(in, "http://", 7) == 0) in += 7;
    if (strncmp(in, "www.", 4) == 0) in += 4;
    if (strncmp(in, "m.", 2) == 0 && strstr(in, "soundcloud.com")) in += 2;
    if (strncmp(in, "soundcloud.com/", 15) == 0) in += 15;
    while (*in == '@' || *in == '/') in++;
    char name[96];
    size_t n = 0;
    while (in[n] && in[n] != '/' && in[n] != '?' && in[n] != '#' && in[n] != ' ' && n < sizeof(name) - 1) {
        const unsigned char c = (unsigned char)in[n];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.';
        if (!ok) return false;
        name[n] = (char)c; n++;
    }
    if (n < 3 || n > 80) return false;                          // SoundCloud profile names are 3 to 25 characters; be generous
    name[n] = 0;
    return put(out, cap, "https://soundcloud.com/%s", name);
}

static bool list_url(const char *path, int limit, const char *cid, char *out, size_t cap) {
    return put(out, cap, API "%s?limit=%d&linked_partitioning=1&client_id=%s", path, limit, cid);
}

bool sc_url_resolve(const char *page_url, const char *cid, char *out, size_t cap) {
    char enc[256];
    if (!sc_urlenc(page_url, enc, sizeof(enc))) return false;
    return put(out, cap, API "resolve?url=%s&client_id=%s", enc, cid);
}

bool sc_url_user_likes(int64_t uid, int limit, const char *cid, char *out, size_t cap) {
    char p[64]; snprintf(p, sizeof(p), "users/%lld/likes", (long long)uid);
    return list_url(p, limit, cid, out, cap);
}
bool sc_url_user_playlists(int64_t uid, int limit, const char *cid, char *out, size_t cap) {
    char p[64]; snprintf(p, sizeof(p), "users/%lld/playlists", (long long)uid);
    return list_url(p, limit, cid, out, cap);
}
bool sc_url_user_tracks(int64_t uid, int limit, const char *cid, char *out, size_t cap) {
    char p[64]; snprintf(p, sizeof(p), "users/%lld/tracks", (long long)uid);
    return list_url(p, limit, cid, out, cap);
}
bool sc_url_user_reposts(int64_t uid, int limit, const char *cid, char *out, size_t cap) {
    char p[64]; snprintf(p, sizeof(p), "stream/users/%lld/reposts", (long long)uid);
    return list_url(p, limit, cid, out, cap);
}
bool sc_url_playlist(int64_t pid, const char *cid, char *out, size_t cap) {
    return put(out, cap, API "playlists/%lld?client_id=%s", (long long)pid, cid);
}
bool sc_url_search_tracks(const char *q, int limit, const char *cid, char *out, size_t cap) {
    char enc[192];
    if (!sc_urlenc(q, enc, sizeof(enc))) return false;
    return put(out, cap, API "search/tracks?q=%s&limit=%d&linked_partitioning=1&client_id=%s", enc, limit, cid);
}
bool sc_url_related(int64_t tid, int limit, const char *cid, char *out, size_t cap) {
    char p[64]; snprintf(p, sizeof(p), "tracks/%lld/related", (long long)tid);
    return list_url(p, limit, cid, out, cap);
}
bool sc_url_tracks_ids(const int64_t *ids, int n, const char *cid, char *out, size_t cap) {
    if (n <= 0) return false;
    size_t o = (size_t)snprintf(out, cap, API "tracks?ids=");
    if (o >= cap) return false;
    for (int i = 0; i < n; i++) {
        const int w = snprintf(out + o, cap - o, "%s%lld", i ? "," : "", (long long)ids[i]);
        if (w < 0 || (size_t)w >= cap - o) return false;
        o += (size_t)w;
    }
    const int w = snprintf(out + o, cap - o, "&client_id=%s", cid);
    return w >= 0 && (size_t)w < cap - o;
}

bool sc_url_with_cid(const char *url, const char *cid, char *out, size_t cap) {
    static const char pre[] = "https://" SC_API_HOST "/";
    if (strncmp(url, pre, sizeof(pre) - 1) != 0) return false;
    if (strstr(url, "client_id=")) return put(out, cap, "%s", url);       // already carries one
    return put(out, cap, "%s%sclient_id=%s", url, strchr(url, '?') ? "&" : "?", cid);
}

bool sc_art_url(const char *art, int px, char *out, size_t cap) {
    const char *dot = strrchr(art, '.');
    const char *dash = strrchr(art, '-');
    const char *slash = strrchr(art, '/');
    if (!dot || !dash || dash > dot || (slash && dash < slash) || dot - dash > 12 || dot - dash < 3) return put(out, cap, "%s", art);
    return put(out, cap, "%.*s-t%dx%d%s", (int)(dash - art), art, px, px, dot);
}

// ---- artwork keys --------------------------------------------------------------------------------

#define ART_PREFIX "sca:"

bool sc_is_art_key(const char *key) { return strncmp(key, ART_PREFIX, sizeof(ART_PREFIX) - 1) == 0; }

static bool word_ok(const char *s, size_t n, size_t max) {
    if (n == 0 || n > max) return false;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    }
    return true;
}

static bool path_ok(const char *s, size_t n) {
    if (n == 0 || n > 120) return false;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '/')) return false;
    }
    return s[0] != '/' && strstr(s, "//") == NULL;
}

bool sc_art_key(const char *art, char *key, size_t cap) {
    if (strncmp(art, "https://", 8) != 0) return false;
    const char *host = art + 8;
    const char *slash = strchr(host, '/');
    if (!slash) return false;
    const size_t hl = (size_t)(slash - host);
    static const char dom[] = ".sndcdn.com";
    if (hl <= sizeof(dom) - 1 || memcmp(host + hl - (sizeof(dom) - 1), dom, sizeof(dom) - 1) != 0) return false;
    const size_t pl = hl - (sizeof(dom) - 1);                          // the "i1" of i1.sndcdn.com
    if (!word_ok(host, pl, 6)) return false;
    const char *path = slash + 1;
    size_t n = strlen(path);
    const char *q = strchr(path, '?');                                 // a cache-busting query is not part of the picture's identity
    if (q) n = (size_t)(q - path);
    const char *dot = NULL;
    for (size_t i = 0; i < n; i++) if (path[i] == '.') dot = path + i;
    if (!dot) return false;
    const size_t el = (size_t)(path + n - dot - 1);
    if (!word_ok(dot + 1, el, 4)) return false;
    size_t sl = (size_t)(dot - path);                                   // the stem, with its size suffix if it has one
    bool sized = false;
    if (strncmp(path, "artworks-", 9) == 0 || strncmp(path, "avatars-", 8) == 0 || strncmp(path, "visuals-", 8) == 0) {
        const char *dash = NULL;
        for (size_t i = 0; i < sl; i++) if (path[i] == '-') dash = path + i;
        if (dash && dash > path && word_ok(dash + 1, (size_t)(path + sl - dash - 1), 10)) { sized = true; sl = (size_t)(dash - path); }
    }
    if (!path_ok(path, sl)) return false;
    const int w = snprintf(key, cap, ART_PREFIX "%.*s:%c:%.*s:%.*s", (int)pl, host, sized ? 's' : 'u', (int)el, dot + 1, (int)sl, path);
    return w > 0 && (size_t)w < cap && (size_t)w <= 63;
}

bool sc_art_key_url(const char *key, int px, char *url, size_t cap) {
    if (!sc_is_art_key(key)) return false;
    const char *p = key + sizeof(ART_PREFIX) - 1;
    const char *c1 = strchr(p, ':');
    if (!c1) return false;
    const char *c2 = strchr(c1 + 1, ':');
    if (!c2 || c2 != c1 + 2) return false;
    const char sized = c1[1];
    const char *c3 = strchr(c2 + 1, ':');
    if (!c3 || (sized != 's' && sized != 'u')) return false;
    const size_t pl = (size_t)(c1 - p), el = (size_t)(c3 - c2 - 1);
    const char *stem = c3 + 1;
    if (!word_ok(p, pl, 6) || !word_ok(c2 + 1, el, 4) || !path_ok(stem, strlen(stem))) return false;
    if (sized == 's') {
        if (px != 300 && px != 500) px = 500;
        return put(url, cap, "https://%.*s.sndcdn.com/%s-t%dx%d.%.*s", (int)pl, p, stem, px, px, (int)el, c2 + 1);
    }
    return put(url, cap, "https://%.*s.sndcdn.com/%s.%.*s", (int)pl, p, stem, (int)el, c2 + 1);
}
