// SoundCloud on the console -- see sc_svc.h.
#include "sc_svc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

int sc_art_bytes(const char *key, unsigned char *buf, int cap) {
    if (!sc_is_art_key(key)) return 0;
    char url[200];
    if (!sc_art_key_url(key, 500, url, sizeof url)) return -1;
    static sc_net net;                                   // the thumbnail thread's alone
    sc_svc_net_begin(&net);
    net.connect_ms = 6000; net.handshake_ms = 8000; net.first_byte_ms = 10000; net.idle_ms = 6000;
    char *body = NULL;
    size_t len = 0;
    const int rc = sc_net_get(&net, url, (size_t)cap, &body, &len);
    sc_svc_net_end(&net);
    if (rc != SC_NET_OK || !body) { free(body); return -1; }
    memcpy(buf, body, len);
    free(body);
    return (int)len;
}

int music_art_bytes(const char *key, unsigned char *buf, int cap) {
    const int r = lm_art_bytes(key, buf, cap);
    if (r != 0) return r;
    return sc_art_bytes(key, buf, cap);
}
