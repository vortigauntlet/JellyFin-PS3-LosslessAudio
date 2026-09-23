// The redesigned player HUD -- design-import-v3 "07 · Player HUD", drawn when
// the spine gate is on.  #included by hud_draw.cpp (it uses that file's
// staging-buffer statics); not a translation unit of its own.
//
// Same mechanism as the old HUD: composed into the main-memory staging buffer
// only when its content changes (about once a second), uploaded, and drawn as
// one blended quad over the video.  Nothing here reads video memory.
//
// Geometry is the canvas's, measured at 1280x720 (MEASURED-detail-hud.md) and
// scaled with UIS_*, which the old HUD's raw pixels never were.
//
// KEPT FROM THE OLD HUD, because the canvas shows no controls for them and
// dropping them would remove features: the focusable slots.  Left/Right still
// walk REW / PLAY / FF / AUDIO / VOLUME / CC, and X still activates the focused
// one.  They are drawn in the canvas's own vocabulary -- AUDIO, VOLUME and CC
// are the chips on the right, REW / FF show their step above the bar -- and
// the X hint names whatever X will do.
//
// NOT SHOWN, because the client has no such data yet: the buffered-to mark
// and the "2014 · Chapter 14" subline.

static const int V3_INCR[3] = { 10, 30, 300 };   // mirrors hud_core.cpp

static int v3x(int x) { return overscan_x() + UIS_W(x); }
static int v3y(int y) { return overscan_y() + UIS_H(y); }

// Straight-alpha fill into the staging buffer.
static void v3_fill(int x, int y, int w, int h, u32 rgb, u8 a) {
    if (w <= 0 || h <= 0) return;
    int x2 = x + w, y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > (int)s_ovl_w) x2 = (int)s_ovl_w;
    if (y2 > (int)s_ovl_h) y2 = (int)s_ovl_h;
    const u32 px = ((u32)a << 24) | (rgb & 0x00FFFFFFu);
    for (int yy = y; yy < y2; yy++) {
        u32 *row = s_ovl_stage + (u32)yy * s_ovl_w;
        for (int xx = x; xx < x2; xx++) row[xx] = px;
    }
    ovl_span(y, y2);
}

// Coverage of pixel (px, py) by a rounded rect, 0..1, with a one-pixel soft
// edge.  Only the corner squares need the distance test.
static float v3_rrect_cov(float px, float py, float x, float y, float w, float h,
                          float r) {
    const float cx = px < x + r ? x + r : (px > x + w - r ? x + w - r : px);
    const float cy = py < y + r ? y + r : (py > y + h - r ? y + h - r : py);
    const float dx = px - cx, dy = py - cy;
    float d;
    if (dx == 0.0f && dy == 0.0f) {
        // Inside the straight part: distance to the nearest edge.
        float e = px - x;
        if (x + w - px < e) e = x + w - px;
        if (py - y < e) e = py - y;
        if (y + h - py < e) e = y + h - py;
        return e >= 0.5f ? 1.0f : (e + 0.5f > 0.0f ? e + 0.5f : 0.0f);
    }
    d = sqrtf(dx * dx + dy * dy);
    const float c = r + 0.5f - d;
    return c <= 0.0f ? 0.0f : (c >= 1.0f ? 1.0f : c);
}

// Rounded rect in the staging buffer: a fill at fill_a, then a t-pixel ring
// at line_a over its edge (either alpha may be 0 to skip that part).
static void v3_rrect(int x, int y, int w, int h, int r, int t,
                     u32 fill_rgb, u8 fill_a, u32 line_rgb, u8 line_a) {
    if (w <= 0 || h <= 0) return;
    const float fx = (float)x, fy = (float)y, fw = (float)w, fh = (float)h;
    const float fr = (float)r, ft = (float)t;
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= (int)s_ovl_h) continue;
        u32 *row = s_ovl_stage + (u32)yy * s_ovl_w;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= (int)s_ovl_w) continue;
            const float pcx = (float)xx + 0.5f, pcy = (float)yy + 0.5f;
            const float outer = v3_rrect_cov(pcx, pcy, fx, fy, fw, fh, fr);
            if (outer <= 0.0f) continue;
            const float inner = t > 0
                ? v3_rrect_cov(pcx, pcy, fx + ft, fy + ft, fw - 2 * ft, fh - 2 * ft,
                               fr > ft ? fr - ft : 0.0f)
                : outer;
            const float ring = outer - inner;
            u32 rgb; float a;
            if (line_a && ring > 0.001f && (!fill_a || ring * line_a >= inner * fill_a)) {
                rgb = line_rgb; a = ring * (float)line_a + inner * (float)fill_a * (1.0f - ring);
            } else {
                rgb = fill_rgb; a = inner * (float)fill_a;
            }
            if (a < 0.5f) continue;
            row[xx] = ((u32)(a + 0.5f) << 24) | (rgb & 0x00FFFFFFu);
        }
    }
    ovl_span(y, y + h);
}

static void hud_body_v3(u64 elapsed_us, bool paused, bool scrubbing) {
    const int W = (int)s_ovl_w;
    const u32 WHITE = 0x00FFFFFF;

    // ---- bottom scrim: y=459, 260 tall, transparent -> black 0.90 @ 62% ----
    {
        const int y0 = v3y(459), h = (int)s_ovl_h - y0;
        for (int i = 0; i < h; i++) {
            float t = (float)i / ((float)h * 0.62f);
            if (t > 1.0f) t = 1.0f;
            const u8 a = (u8)(230.0f * t + 0.5f);
            if (a) v3_fill(0, y0 + i, W, 1, 0, a);
        }
    }

    // ---- elapsed / remaining ----
    const u32 el = (u32)(elapsed_us / 1000000ULL);
    char el_s[16], rem_s[20] = "";
    fmt_time(el_s, sizeof el_s, el);
    if (g_hud.total_secs > 0) {
        const u32 rem = g_hud.total_secs > el ? g_hud.total_secs - el : 0;
        char t[16]; fmt_time(t, sizeof t, rem);
        snprintf(rem_s, sizeof rem_s, "-%s", t);
    }

    // ---- title: 30px/700 white, -0.01em, at (57,536) ----
    const int chips_right = v3x(1223);
    if (g_hud.title[0]) {
        const float tpx = UIS_TF(30.0f);
        char tb[128];
        snprintf(tb, sizeof tb, "%s", g_hud.title);
        const int maxw = v3x(960) - v3x(57);
        int len = (int)strlen(tb);
        while (len > 1 && ttf_text_width(tb, tpx, true) > maxw) tb[--len] = 0;
        drawTTF((u32)v3x(57), (u32)v3y(536), tb, tpx, WHITE, true);
    }

    // ---- chips, right-aligned to x=1223 at y=581: audio, volume, subs ----
    // Each is also a focus slot; the focused one takes the accent_alt border
    // and full-white text, the rest the canvas's white@14% / white@86%.
    {
        const float cpx = UIS_TF(11.0f);
        const int  ch = UIS_H(29), cy = v3y(581), gap = UIS_W(8), pad = UIS_W(12);
        char a_lab[80], v_lab[24], s_lab[24];
        snprintf(a_lab, sizeof a_lab, "audio \xC2\xB7 %s",
                 g_hud.audio_label[0] ? g_hud.audio_label : "default");
        snprintf(v_lab, sizeof v_lab, "vol \xC2\xB7 %d", audio_get_volume());
        snprintf(s_lab, sizeof s_lab, "subs \xC2\xB7 %s", g_hud.cc_active ? "on" : "off");
        const char *labs[3] = { a_lab, v_lab, s_lab };
        const int   slot[3] = { FOCUS_AUDIO, FOCUS_VOLUME, FOCUS_CC };
        int w[3], x = chips_right;
        for (int i = 2; i >= 0; i--) {
            w[i] = ttf_text_width(labs[i], cpx) + 2 * pad;
            x -= w[i];
            const bool foc = g_hud.focus == slot[i];
            v3_rrect(x, cy, w[i], ch, ch / 2, 1, WHITE, 20,
                     foc ? XMB_ACCENT_ALT : WHITE, foc ? 255 : 36);
            drawTTF((u32)(x + pad), (u32)(cy + (ch - (int)cpx) / 2 - 1), labs[i], cpx,
                    foc ? WHITE : 0x00DBDBDB);
            // The volume slider opens above its chip while adjusting.
            if (slot[i] == FOCUS_VOLUME && g_hud.vol_active) {
                const int vol = audio_get_volume();
                const int sx  = x + w[i] / 2, bot = cy - UIS_H(14);
                const int th  = UIS_H(90), tw = UIS_W(6);
                v3_rrect(sx - UIS_W(22), bot - th - UIS_H(34), UIS_W(44), th + UIS_H(44),
                         UIS_H(6), 1, 0, 140, WHITE, 26);
                v3_fill(sx - tw / 2, bot - th, tw, th, WHITE, 60);
                const int fh = th * vol / 100;
                v3_fill(sx - tw / 2, bot - fh, tw, fh, XMB_ACCENT, 255);
                char vb[8]; snprintf(vb, sizeof vb, "%d", vol);
                const int vw = ttf_text_width(vb, cpx);
                drawTTF((u32)(sx - vw / 2), (u32)(bot - th - UIS_H(26)), vb, cpx, WHITE);
            }
            x -= gap;
        }
    }

    // ---- progress: (57,628) 1166 x 6, r=3; played in accent ----
    const int bx = v3x(57), by = v3y(628), bw = v3x(1223) - v3x(57), bh = UIS_H(6);
    v3_rrect(bx, by, bw, bh, bh / 2, 0, WHITE, 41, 0, 0);
    int fill = 0;
    if (g_hud.total_secs > 0) {
        float p = (float)el / (float)g_hud.total_secs;
        if (p > 1.0f) p = 1.0f;
        fill = (int)(p * (float)bw);
    }
    if (fill > 0) v3_rrect(bx, by, fill, bh, bh / 2, 0, XMB_ACCENT, 255, 0, 0);
    if (scrubbing || g_hud.focus == FOCUS_REW || g_hud.focus == FOCUS_FF)
        v3_rrect(bx + fill - UIS_H(7), by + bh / 2 - UIS_H(7), UIS_H(14), UIS_H(14),
                 UIS_H(7), 0, WHITE, 255, 0, 0);

    // REW / FF focus: the step they will jump, above the bar at its ends.
    if (g_hud.focus == FOCUS_REW || g_hud.focus == FOCUS_FF) {
        char st[24];
        const int n = V3_INCR[g_hud.incr_idx < 0 ? 0 : g_hud.incr_idx % 3];
        const float spx = UIS_TF(12.5f);
        if (g_hud.focus == FOCUS_REW) {
            snprintf(st, sizeof st, "\xE2\x80\xB9 back %ds", n);
            drawTTF((u32)bx, (u32)(by - UIS_H(24)), st, spx, XMB_ACCENT_ALT);
        } else {
            snprintf(st, sizeof st, "ahead %ds \xE2\x80\xBA", n);
            const int sw = ttf_text_width(st, spx);
            drawTTF((u32)(bx + bw - sw), (u32)(by - UIS_H(24)), st, spx, XMB_ACCENT_ALT);
        }
    }

    // Trickplay preview while hold-scrubbing, centred on the play head.
    if (scrubbing) {
        const TrickplayTile *tp = trickplay_current();
        if (tp && tp->sheet_rgb && tp->tile_w > 0 && tp->tile_h > 0) {
            const int cw = tp->tile_w + CARD_PAD * 2, chh = tp->tile_h + CARD_PAD * 2;
            int cx = bx + fill - cw / 2;
            if (cx < bx) cx = bx;
            if (cx + cw > bx + bw) cx = bx + bw - cw;
            const int cyy = by - UIS_H(34) - chh;
            ovl_dim((u32)cx, (u32)cyy, (u32)cw, (u32)chh, 235);
            drawBitmapRect(tp->sheet_rgb, (u32)tp->sheet_w,
                           (u32)tp->tile_x, (u32)tp->tile_y,
                           (u32)tp->tile_w, (u32)tp->tile_h,
                           (u32)(cx + CARD_PAD), (u32)(cyy + CARD_PAD));
        }
    }

    // ---- times: 12.5px at y=644, white@76% ----
    {
        const float tpx = UIS_TF(12.5f);
        drawTTF((u32)bx, (u32)v3y(644), el_s, tpx, 0x00C2C2C2);
        if (rem_s[0]) {
            const int rw = ttf_text_width(rem_s, tpx);
            drawTTF((u32)(bx + bw - rw), (u32)v3y(644), rem_s, tpx, 0x00C2C2C2);
        }
    }

    // ---- audio path, top right: accent_alt pill on black@55% ----
    if (g_hud.audio_label[0]) {
        const float apx = UIS_TF(11.0f);
        const int aw = ttf_text_width(g_hud.audio_label, apx) + UIS_W(28);
        const int ah = UIS_H(31), ax = v3x(1239) - aw, ay = v3y(41);
        v3_rrect(ax, ay, aw, ah, ah / 2, 1, 0, 140, XMB_ACCENT_ALT, 255);
        drawTTF((u32)(ax + UIS_W(14)), (u32)(ay + (ah - (int)apx) / 2 - 1),
                g_hud.audio_label, apx, XMB_ACCENT_ALT);
    }

    // ---- hints: what X does, then the canvas's Tracks / Stop / Stats ----
    {
        Hint h[4]; int n = 0;
        h[n].glyph = 'X';
        h[n].label = g_hud.focus == FOCUS_REW    ? "Back"
                   : g_hud.focus == FOCUS_FF     ? "Ahead"
                   : g_hud.focus == FOCUS_AUDIO  ? "Audio"
                   : g_hud.focus == FOCUS_VOLUME ? "Volume"
                   : g_hud.focus == FOCUS_CC     ? "Subtitles"
                   : paused                      ? "Play" : "Pause";
        n++;
        h[n].glyph = 'T'; h[n].label = "Tracks"; n++;
        h[n].glyph = 'C'; h[n].label = "Stop";   n++;
#if ENABLE_PLAYER_STATS
        h[n].glyph = 'S'; h[n].label = "Stats";  n++;
#endif
        draw_hints_bar(h, n);
        ovl_span(v3y(660), (int)s_ovl_h);
    }

    // The track menu, lifted above the chips instead of sitting on the strip.
    if (g_hud.menu_visible && g_hud.menu_n > 0)
        draw_menu(W, v3y(560) + HUD_STRIP_H + 6);
}
