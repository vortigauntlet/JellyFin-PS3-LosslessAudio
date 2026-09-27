// Screenshots of the app's own frames -- see shot.h.

#include <stdio.h>
#include <string.h>
#include <ppu-types.h>
#include <sys/file.h>     // sysLv2FsMkdir

#include "shot.h"
#include "plog.h"

#define SHOT_DIR  "/dev_hdd0/tmp/jfshot"
#define SHOT_REQ  "/dev_hdd0/tmp/jf_shot_req.txt"
#define SHOT_POLL 30      // flips between checks for the request file

static bool s_pending = false;
static char s_name[40];
static int  s_seq     = 0;
static u32  s_flips   = 0;

void shot_request(void) {
    if (s_pending) return;
    snprintf(s_name, sizeof s_name, "shot_%03d", s_seq++);
    s_pending = true;
}

// The request file's first word names the shot ("home" -> home.bmp); an empty
// file gets a numbered name.  Only [A-Za-z0-9_-] survive.
static void poll_request_file(void) {
    FILE *f = fopen(SHOT_REQ, "r");
    if (!f) return;
    char word[40] = "";
    if (fscanf(f, "%39s", word) != 1) word[0] = '\0';
    fclose(f);
    remove(SHOT_REQ);
    int n = 0;
    for (const char *p = word; *p && n < (int)sizeof s_name - 1; p++) {
        const char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')
            s_name[n++] = c;
    }
    s_name[n] = '\0';
    if (!n) snprintf(s_name, sizeof s_name, "shot_%03d", s_seq++);
    s_pending = true;
}

bool shot_due(void) {
    if (!s_pending && ++s_flips >= SHOT_POLL) {
        s_flips = 0;
        poll_request_file();
    }
    return s_pending;
}

static void put_le32(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

void shot_write(const u32 *fb, int w, int h) {
    s_pending = false;
    if (!fb || w <= 0 || h <= 0 || w > 1920) return;
    sysLv2FsMkdir(SHOT_DIR, 0777);
    char path[96];
    snprintf(path, sizeof path, SHOT_DIR "/%s.bmp", s_name);
    FILE *f = fopen(path, "wb");
    if (!f) { plog("shot: cannot open file"); return; }

    // 24-bit BMP, top-down (negative height).  Rows of w*3 bytes, padded to 4.
    const u32 row = ((u32)w * 3u + 3u) & ~3u;
    u8 hdr[54];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    put_le32(hdr + 2, 54 + row * (u32)h);
    put_le32(hdr + 10, 54);
    put_le32(hdr + 14, 40);
    put_le32(hdr + 18, (u32)w);
    put_le32(hdr + 22, (u32)(-h));
    hdr[26] = 1;
    hdr[28] = 24;
    put_le32(hdr + 34, row * (u32)h);
    fwrite(hdr, 1, sizeof hdr, f);

    // The framebuffer is VRAM: reading it is slow on the PPU (~8 MB/s), so a
    // 720p shot holds this frame for about half a second.  Words are
    // 0x00RRGGBB; the file wants B, G, R.
    static u32 src[1920];
    static u8  out[1920 * 3 + 4];
    for (int y = 0; y < h; y++) {
        memcpy(src, fb + (size_t)y * (size_t)w, (size_t)w * 4);
        for (int x = 0; x < w; x++) {
            const u32 p = src[x];
            out[x * 3 + 0] = (u8)p;
            out[x * 3 + 1] = (u8)(p >> 8);
            out[x * 3 + 2] = (u8)(p >> 16);
        }
        fwrite(out, 1, row, f);
    }
    fclose(f);
    char line[128];
    snprintf(line, sizeof line, "shot: wrote %s (%dx%d)", path, w, h);
    plog(line);
}
