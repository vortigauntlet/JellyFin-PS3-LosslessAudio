// Search tab rendering — the search field and the results list below it.
// The term itself is typed on the system keyboard (ui_search.cpp).

#include "i18n.h"
#include <stdio.h>
#include <string.h>

#include "ui_render_internal.h"
#include "thumbnail_cache.h"
#include "timing.h"

#define OSK_FIELD_H UIS_H(40)

// -------------------------------------------------------
// Search layout
// -------------------------------------------------------

int xmb_search_results_y(void) {
    return XMB_CONTENT_Y + 8 + OSK_FIELD_H + 16;
}

// Rows that actually fit.  The single source of truth for both the renderer
// and the scroll arithmetic.
int xmb_search_vis_rows(void) {
    int r = ((int)display_height - XMB_BOTTOM_PAD - xmb_search_results_y())
            / XMB_ROW_STRIDE;
    return r < 0 ? 0 : r;
}

static int search_field_w(void) { return 10 * OSK_STEP_X - OSK_GAP; }

void xmb_cpu_draw_search(void) {
    int total_w = search_field_w();
    int x0      = ((int)display_width - total_w) / 2;

    // Search field: dark well with an accent underline.
    drawRect((u32)x0, (u32)(XMB_CONTENT_Y + UIS_H(8)),
             (u32)total_w, OSK_FIELD_H, XMB_TRACK);
    drawRect((u32)x0, (u32)(XMB_CONTENT_Y + UIS_H(8) + OSK_FIELD_H - UIS_H(2)),
             (u32)total_w, UIS_H(2), XMB_ACCENT);
}

void xmb_rsx_draw_search(void) {
    int total_w = search_field_w();
    int x0      = ((int)display_width - total_w) / 2;

    // Search field content: magnifier glyph, then the term or a ghost prompt.
    {
        int fx = x0 + UIS_W(14);
        int fy = XMB_CONTENT_Y + UIS_H(8);
        drawIcon((u32)fx, (u32)(fy + (OSK_FIELD_H - UIS_H(20)) / 2), ICON_SEARCH, UIS_TF(20.0f),
                 XMB_TEXT_FAINT);
        int tx = fx + UIS_W(30);
        int ty = fy + (OSK_FIELD_H - UIS_H(18)) / 2 - UIS_H(2);
        if (g_search_buf[0])
            drawTTF((u32)tx, (u32)ty, g_search_buf, UIS_TF(18), XMB_TEXT);
        else
            drawTTF((u32)tx, (u32)ty, TR("Search your library"), UIS_TF(17),
                    XMB_TEXT_FAINT);
    }

    int results_y = xmb_search_results_y();
    int count     = g_search_results_count;
    int vis_r     = xmb_search_vis_rows();
    {
        int sr_list_x = ((int)display_width - XMB_LIST_W) / 2;
        int sr_tx     = sr_list_x + UIS_W(16) + XMB_THUMB_W + UIS_W(16);
        for (int i = 0; i < vis_r; i++) {
            int idx = g_search_scroll + i;
            if (idx >= count) break;
            const XMBItem *it = &g_search_results[idx];
            int iy = results_y + i * XMB_ROW_STRIDE;
            bool sel = g_search_focus_results && idx == g_search_sel;
            drawTTF((u32)sr_tx, (u32)(iy + UIS_H(18)), it->name, UIS_TF(19),
                    sel ? XMB_WHITE : XMB_TEXT, sel);
            xmb_draw_meta((u32)sr_tx, (u32)(iy + UIS_H(46)), it, UIS_TF(14));
        }
    }
    if (count == 0 && g_search_buf[0] && xmb_search_in_flight()) {
        // The query is out on its worker thread.  This server takes 2.5-8.4 s
        // to answer a search (HANDOFF.md), and saying "No results" for that
        // whole window is indistinguishable from the search being broken.
        char msg[96];
        snprintf(msg, sizeof(msg), TR("Searching for \"%s\"..."), g_search_buf);
        int mw = ttf_text_width(msg, UIS_TF(16));
        drawTTF((u32)(((int)display_width - mw) / 2), (u32)(results_y + UIS_H(10)),
                msg, UIS_TF(16), XMB_TEXT_DIM);
    } else if (count == 0 && g_search_buf[0]) {
        char msg[96];
        snprintf(msg, sizeof(msg), TR("No results for \"%s\""), g_search_buf);
        int mw = ttf_text_width(msg, UIS_TF(16));
        drawTTF((u32)(((int)display_width - mw) / 2), (u32)(results_y + UIS_H(10)),
                msg, UIS_TF(16), XMB_TEXT_FAINT);
    }
}

// CPU draws for search results list (selection highlight + scaled thumbs).
void xmb_cpu_draw_search_results(void) {
    int results_y = xmb_search_results_y();
    int count     = g_search_results_count;
    int vis_r     = xmb_search_vis_rows();
    int list_x = ((int)display_width - XMB_LIST_W) / 2;
    for (int i = 0; i < vis_r; i++) {
        int idx = g_search_scroll + i;
        if (idx >= count) break;
        int iy = results_y + i * XMB_ROW_STRIDE;
        if (g_search_focus_results && idx == g_search_sel) {
            drawRect((u32)list_x, (u32)iy,
                     (u32)XMB_LIST_W, (u32)XMB_ROW_H, XMB_PANEL_HI);
            drawRect((u32)(list_x - UIS_W(4)), (u32)iy,
                     UIS_W(3), (u32)XMB_ROW_H, XMB_ACCENT);
        }
        xmb_cpu_blit_thumb_scaled(g_search_results[idx].id,
                                  list_x + UIS_W(16),
                                  iy + (XMB_ROW_H - XMB_THUMB_H) / 2,
                                  XMB_THUMB_W, XMB_THUMB_H);
    }
}
