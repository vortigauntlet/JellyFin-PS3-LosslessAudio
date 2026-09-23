// Per-playback 24p decision record.  See display_diag.h and display_mode.h.
//
// Every line starts "24p:" so one grep over player_log.txt gives the whole
// story of a hardware test: what the app believed, what it would have wanted,
// and what it did -- which today is always nothing, by design.

#include <stdio.h>
#include <string.h>

#include <sysutil/video.h>

#include "display_diag.h"
#include "plog.h"

static const char *port_name(u8 p)
{
	switch (p) {
	case VIDEO_PORT_HDMI:      return "HDMI";
	case VIDEO_PORT_NETWORK:   return "network";
	case VIDEO_PORT_COMPOSITE: return "composite";
	case VIDEO_PORT_D:         return "D-terminal";
	case VIDEO_PORT_COMPONENT: return "component";
	case VIDEO_PORT_RGB:       return "RGB";
	case VIDEO_PORT_SCART:     return "SCART";
	case VIDEO_PORT_DSUB:      return "D-sub";
	case VIDEO_PORT_NONE:      return "none";
	default:                   return "?";
	}
}

void display_diag_session(u32 fps_num, u32 fps_den, dm_fps_source src,
                          u32 width, u32 height)
{
	char b[160];
	char rates[64];

	// ---- DISPLAY ----
	videoState st;
	memset(&st, 0, sizeof(st));
	const bool st_ok = videoGetState(0, 0, &st) == 0;

	videoDeviceInfo di;
	memset(&di, 0, sizeof(di));
	dm_mode modes[32];
	int nmodes = -1;
	if (videoGetDeviceInfo(0, 0, &di) == 0) {
		nmodes = di.availableModeCount > 32 ? 32 : di.availableModeCount;
		for (int i = 0; i < nmodes; i++) {
			modes[i].res   = di.availableModes[i].resolution;
			modes[i].rates = di.availableModes[i].refreshRates;
		}
	}

	videoResolution vr;
	memset(&vr, 0, sizeof(vr));
	const u8  cur_res   = st_ok ? st.displayMode.resolution : 0;
	const u16 cur_rates = st_ok ? st.displayMode.refreshRates : 0;
	if (st_ok) videoGetResolution(cur_res, &vr);

	u32 dnum, dden;
	const int rate_known = dm_display_rate(cur_rates, &dnum, &dden);
	dm_rates_str(cur_rates, rates, sizeof(rates));
	snprintf(b, sizeof(b),
	         "24p: DISPLAY connector=%s res=%u %ux%u reported_refresh=%u/%u%s"
	         " (bits %s)",
	         nmodes >= 0 ? port_name(di.portType) : "unreadable",
	         (unsigned)cur_res, (unsigned)vr.width, (unsigned)vr.height,
	         (unsigned)dnum, (unsigned)dden,
	         rate_known ? "" : " ASSUMED", rates);
	plog(b);

	// Every rate the TV advertises for 1920x1080, OR'd across entries: this is
	// the firmware's own EDID-derived answer, the only capability source a
	// game process has.
	u16 adv1080 = 0;
	for (int i = 0; i < nmodes; i++)
		if (modes[i].res == DM_RES_1080) adv1080 |= modes[i].rates;
	const dm_support sup = dm_display_24p_support(modes, nmodes, DM_RES_1080);
	dm_rates_str(adv1080, rates, sizeof(rates));
	snprintf(b, sizeof(b),
	         "24p: DISPLAY capabilities 1080p rates=%s -> 24Hz-family %s"
	         " (bit meaning inferred, 23.976 vs 24 unproven)",
	         nmodes >= 0 ? rates : "unreadable", dm_support_name(sup));
	plog(b);

	// ---- CONTENT ----
	const dm_film film = dm_classify_film(fps_num, fps_den);
	const int confident = dm_fps_confident(src);
	snprintf(b, sizeof(b),
	         "24p: CONTENT %ux%u fps=%u/%u film=%s source=%s confidence=%s",
	         (unsigned)width, (unsigned)height,
	         (unsigned)fps_num, (unsigned)fps_den,
	         film == DM_FILM_23976 ? "23.976" : film == DM_FILM_24 ? "24.000" : "no",
	         dm_fps_source_name(src), confident ? "detected" : "guessed");
	plog(b);

	// ---- DECISION ----
	const dm_decision d = dm_decide(film, confident, width, height,
	                                cur_res, cur_rates, sup);
	snprintf(b, sizeof(b),
	         "24p: DECISION candidate=%s (%s) display_support=%s path=%s attempt=%s",
	         d.candidate ? "yes" : "no", d.candidate_why,
	         dm_support_name(d.support), d.path, d.attempt ? "yes" : "no");
	plog(b);

	// ---- RESULT ----
	char cad[32];
	dm_cadence_str(dnum, dden, fps_num, fps_den, cad, sizeof(cad));
	snprintf(b, sizeof(b), "24p: RESULT mode_switch=%s (%s)", d.result, d.result_why);
	plog(b);
	snprintf(b, sizeof(b),
	         "24p: RESULT output=%ux%u@%u/%u presentation=%s",
	         (unsigned)vr.width, (unsigned)vr.height,
	         (unsigned)dnum, (unsigned)dden, cad);
	plog(b);
}
