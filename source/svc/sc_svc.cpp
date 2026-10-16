// SoundCloud on the console -- see sc_svc.h.
#include "sc_svc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "jf_paths.h"
#include "local_music_fs.h"      // lm_art_bytes
#include "plog.h"
#include "sc_api.h"
#include "sc_store.h"
#include "sx_plat.h"

#define CID_FILE      "jellyfin_sc.txt"
#define SETTINGS_FILE "jellyfin_sc_settings.txt"

static sx_mutex  *s_mtx;
static ScSettings s_set;
static char       s_cid[SC_CID_LEN + 1];
static bool       s_inited;

static void lock(void)   { if (s_mtx) sx_mutex_lock(s_mtx); }
static void unlock(void) { if (s_mtx) sx_mutex_unlock(s_mtx); }

// The whole text of a small file, or NULL.
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = (char *)malloc(2048);
    if (!b) { fclose(f); return NULL; }
    const size_t n = fread(b, 1, 2047, f);
    fclose(f);
    b[n] = 0;
    return b;
}

static void write_file(const char *path, const char *text, int len) {
    FILE *f = fopen(path, "wb");
    if (!f) { plog("sc: could not write a settings file"); return; }
    fwrite(text, 1, (size_t)len, f);
    fclose(f);
}

void sc_svc_init(void) {
    if (s_inited) return;
    s_inited = true;
    s_mtx = sx_mutex_create();
    char *t = read_file(jf_data_path(SETTINGS_FILE));
    sc_settings_parse(t, &s_set);
    free(t);
    t = read_file(jf_data_path(CID_FILE));
    ScCidFile c;
    sc_cidfile_parse(t, &c);
    free(t);
    memcpy(s_cid, c.cid, sizeof(s_cid));
    memset(&c, 0, sizeof(c));
    char b[96];
    snprintf(b, sizeof b, "sc: settings loaded (profile %s, %d kbps, client id %s)", s_set.profile[0] ? "set" : "not set",
             s_set.quality, s_cid[0] ? "cached" : "not cached");
    plog(b);
}

static void save_settings(void) {
    char out[256];
    ScSettings copy = s_set;
    const int n = sc_settings_format(&copy, out, sizeof out);
    if (n > 0) write_file(jf_data_path(SETTINGS_FILE), out, n);
}

const char *sc_svc_profile(void) { return s_set.profile; }
int sc_svc_quality(void) { return s_set.quality == 96 ? 96 : 160; }

void sc_svc_set_profile(const char *name) {
    lock();
    snprintf(s_set.profile, sizeof s_set.profile, "%s", name ? name : "");
    save_settings();
    unlock();
}

void sc_svc_set_quality(int kbps) {
    lock();
    s_set.quality = kbps == 96 ? 96 : 160;
    save_settings();
    unlock();
}

void sc_svc_net_begin(sc_net *n) {
    sc_net_init(n);
    lock();
    if (s_cid[0]) sc_net_set_cid(n, s_cid);
    unlock();
}

void sc_svc_net_end(sc_net *n) {
    if (n->cid_dirty && n->cid[0]) {
        lock();
        if (strcmp(s_cid, n->cid) != 0) {
            memcpy(s_cid, n->cid, sizeof s_cid);
            ScCidFile c;
            memset(&c, 0, sizeof c);
            memcpy(c.cid, s_cid, sizeof c.cid);
            c.at = (long)time(NULL);
            char out[96];
            const int len = sc_cidfile_format(&c, out, sizeof out);
            if (len > 0) write_file(jf_data_path(CID_FILE), out, len);
            memset(out, 0, sizeof out);
            plog("sc: a new client id was stored");
        }
        n->cid_dirty = false;
        unlock();
    }
    memset(n->cid, 0, sizeof n->cid);
}

// The thumbnail thread has a 64 KB stack, which is little for a TLS handshake, so the fetch runs on a worker of its
// own (128 KB) and the provider waits for it.  One request at a time: only the thumbnail thread asks.
static struct {
    volatile int state;                  // 0 idle, 1 asked, 2 answered
    char         url[200];
    int          cap;
    int          rc;                     // sc_net_get's code
    char        *body;                   // the picture, to free()
    size_t       len;
} s_art;
static bool s_art_started;

static void art_worker(void *) {
    static sc_net net;
    for (;;) {
        if (s_art.state != 1) { usleep(15000); continue; }
        __sync_synchronize();
        sc_svc_net_begin(&net);
        net.connect_ms = 6000; net.handshake_ms = 8000; net.first_byte_ms = 10000; net.idle_ms = 6000;
        s_art.body = NULL;
        s_art.len = 0;
        s_art.rc = sc_net_get(&net, s_art.url, (size_t)s_art.cap, &s_art.body, &s_art.len);
        sc_svc_net_end(&net);
        memset(s_art.url, 0, sizeof s_art.url);
        __sync_synchronize();
        s_art.state = 2;
    }
}

int sc_art_bytes(const char *key, unsigned char *buf, int cap) {
    if (!sc_is_art_key(key)) return 0;
    char url[200];
    if (!sc_art_key_url(key, 500, url, sizeof url)) return -1;
    if (!s_art_started) {
        s_art_started = true;
        if (!sx_thread_spawn(art_worker, NULL, 1500, 128, "sc_art")) { s_art_started = false; return -1; }
    }
    // An earlier request may still be running, or have finished after its caller gave up: settle it first.
    for (int t = 0; t < 1500 && s_art.state != 0; t++) {
        if (s_art.state == 2) { free(s_art.body); s_art.body = NULL; s_art.state = 0; break; }
        usleep(10000);
    }
    if (s_art.state != 0) return -1;
    snprintf(s_art.url, sizeof s_art.url, "%s", url);
    s_art.cap = cap;
    __sync_synchronize();
    s_art.state = 1;
    for (int t = 0; t < 1300 && s_art.state != 2; t++) usleep(10000);
    if (s_art.state != 2) return -1;                     // still busy: the next call settles it
    __sync_synchronize();
    int r = -1;
    if (s_art.rc == SC_NET_OK && s_art.body && s_art.len > 0 && s_art.len <= (size_t)cap) {
        memcpy(buf, s_art.body, s_art.len);
        r = (int)s_art.len;
    }
    free(s_art.body);
    s_art.body = NULL;
    s_art.state = 0;
    return r;
}

int music_art_bytes(const char *key, unsigned char *buf, int cap) {
    const int r = lm_art_bytes(key, buf, cap);
    if (r != 0) return r;
    return sc_art_bytes(key, buf, cap);
}
