// Stream request -- see stream_request.h.
//
// This is build_stream_url()'s decision logic, moved here unchanged so the
// offline downloader can share it.  The comments explaining each knob came
// with it; tests/test_offline.cpp checks the URLs against the pre-move
// output byte for byte.

#include "stream_request.h"
#include "track_codec.h"

#include <stdio.h>
#include <string.h>

void stream_select_initial(const JFTracks *tracks, bool have_tracks,
                           int *cur_audio, int *cur_sub) {
    *cur_audio = (have_tracks && tracks && tracks->n_audio > 0)
                     ? tracks->default_audio : -1;
    *cur_sub = -1;               // subtitles start off
}

void stream_request_resolve(const StreamPrefs *prefs,
                            const StreamSelection *sel, StreamRequest *rq) {
    memset(rq, 0, sizeof(*rq));

    // Frame size, profile/level and bitrate ceiling all come from the quality
    // ladder.  The display clamp only moves the sub-1080p steps' frame size;
    // profile/level/bitrate do not depend on it.
    vquality_params(prefs->quality, prefs->hd1080,
                    prefs->display_w, prefs->display_h,
                    &rq->max_w, &rq->max_h,
                    &rq->profile, &rq->level, &rq->vbitrate);

    const JFTracks *t = sel->tracks;
    const bool have_audio = t && sel->cur_audio >= 0 && sel->cur_audio < t->n_audio;
    const bool have_sub   = t && sel->cur_sub   >= 0 && sel->cur_sub   < t->n_subs;
    rq->audio_idx = have_audio ? t->audio[sel->cur_audio].index : -1;
    rq->sub_idx   = have_sub   ? t->subs[sel->cur_sub].index    : -1;

    // Surround 5.1 (Alpha): request an AC-3 5.1 transcode at the standard DVD
    // rate.  Gated — OFF reproduces the exact stereo MP3 query the ship path
    // sends.  This builder is reused by every seek (player_seek.cpp), so the
    // codec choice is stable across seeks by construction.
    rq->acodec   = prefs->surround ? "ac3"   : "mp3";
    rq->abitrate = prefs->surround ? 640000u : 192000u;
    rq->achans   = prefs->surround ? 6       : 2;
    // Surround "HD" mode: ask the server to STREAM-COPY the source's own HD
    // audio track instead of transcoding it.  Copy is the only way either of
    // these formats ever arrives — Jellyfin will not transcode TO them
    // (ffmpeg's dca encoder is experimental and its truehd encoder tops out
    // at 5.1) — and it is also the whole point:
    //   * a DTS-HD MA / DTS:X track copied intact still carries the 5.1 core
    //     this app decodes, at up to 1509 kbps;
    //   * a TrueHD / Atmos track copied intact decodes here LOSSLESSLY, to
    //     5.1 or 7.1.
    // Either way there is no audio transcode on the server at all.
    //
    // Only asked for when the SELECTED track really is one of those (its
    // label says so).  On any other track — including Dolby Digital Plus,
    // which nothing here decodes — the request is the plain AC-3 one, which
    // is why HD mode never plays worse than AC-3 mode.
    rq->hd_codec = NULL;
    if (prefs->surround && prefs->surround_hd && have_audio) {
        const char *label = t->audio[sel->cur_audio].label;
        if      (track_label_is_dts(label))    rq->hd_codec = "dts";
        else if (track_label_is_truehd(label)) rq->hd_codec = "truehd";
    }

    const JFMediaSource *src = (sel->source && sel->source->id[0]) ? sel->source : NULL;
    snprintf(rq->item_id, sizeof(rq->item_id), "%s", sel->item_id ? sel->item_id : "");
    snprintf(rq->source_id, sizeof(rq->source_id), "%s",
             src ? src->id : rq->item_id);
    snprintf(rq->live_stream_id, sizeof(rq->live_stream_id), "%s",
             src ? src->live_stream_id : "");
}

int stream_url_build(char *url, int url_sz, const StreamRequest *rq,
                     const char *server, const char *device_id,
                     const char *play_session_id,
                     unsigned long long start_ticks) {
    // Four knobs differ between the HD-copy request and the AC-3 one, and
    // all of them matter:
    //   * AllowAudioStreamCopy=true — without it the server transcodes and
    //     the "dts"/"truehd" preference lands on an encoder we do not want.
    //   * no AudioBitrate — Jellyfin checks the requested audio bitrate
    //     before allowing a copy, and a 1509 kbps DTS core (let alone a
    //     multi-Mbps TrueHD stream) fails a 640 kbps ceiling, silently
    //     demoting us to a transcode.
    //   * MaxAudioChannels=8, not 6 — DTS-HD MA, DTS:X and TrueHD tracks are
    //     routinely 7.1, and a 6-channel ceiling would refuse to copy them.
    //   * ac3 is listed FIRST even though the HD codec is the one being asked
    //     for.  Copy eligibility only asks whether the source codec appears
    //     in the list; the order decides what an actual transcode would
    //     encode to, and that must be ac3.
    char aparams[128];
    if (rq->hd_codec) {
        snprintf(aparams, sizeof(aparams),
                 "&AudioCodec=ac3,%s,mp3&AudioSampleRate=48000"
                 "&MaxAudioChannels=8", rq->hd_codec);
    } else {
        snprintf(aparams, sizeof(aparams),
                 "&AudioCodec=%s&AudioBitrate=%u&AudioSampleRate=48000"
                 "&MaxAudioChannels=%d", rq->acodec, rq->abitrate, rq->achans);
    }
    const char *copy_audio = rq->hd_codec ? "true" : "false";
    char encoded_source[288];
    url_encode_query(rq->source_id, encoded_source, sizeof(encoded_source));
    // Video request.  vbitrate == 0 is DIRECT PLAY (the "Original" quality
    // setting): ask the server to copy the source video through untouched.
    //
    // The ceiling has to be ABSENT, not just large.  Jellyfin checks the
    // requested bitrate before it will allow a copy, so a 10 Mbps ask against
    // a 30 Mbps remux does not clamp the copy -- it refuses it and re-encodes,
    // silently.
    //
    // MaxWidth/MaxHeight and MaxFramerate stay in both cases and act as the
    // safety gate: a 4K or 60 fps source exceeds them, so the server scales it
    // down rather than copying something this console cannot decode.
    // Copy is ALWAYS permitted, at every quality.  Jellyfin allows a copy
    // when the source bitrate is <= the requested one, so a ceiling is a
    // CEILING ON THE COPY, not an instruction to re-encode at that rate:
    //
    //   source <= ceiling -> copied untouched (full quality, no server CPU)
    //   source >  ceiling -> transcoded to the ceiling
    char vparams[96];
    if (rq->vbitrate == 0)
        snprintf(vparams, sizeof(vparams), "&AllowVideoStreamCopy=true");
    else
        snprintf(vparams, sizeof(vparams),
                 "&VideoBitrate=%u&AllowVideoStreamCopy=true", rq->vbitrate);

    int n = snprintf(url, url_sz,
        "%s/Videos/%s/stream.ts"
        "?VideoCodec=h264"
        "&Profile=%s"
        "&Level=%s"
        "&MaxWidth=%u&MaxHeight=%u"
        "%s"
        "%s"
        "&MaxFramerate=30"
        "&AllowAudioStreamCopy=%s"
        "&DeviceId=%s&Static=false"
        "&MediaSourceId=%s"
        "&StartTimeTicks=%llu",
        server, rq->item_id, rq->profile, rq->level, rq->max_w, rq->max_h,
        vparams, aparams, copy_audio,
        device_id, encoded_source, start_ticks);
    if (rq->live_stream_id[0] && n > 0 && n < url_sz) {
        char encoded_live[288];
        url_encode_query(rq->live_stream_id, encoded_live, sizeof(encoded_live));
        n += snprintf(url + n, url_sz - n, "&LiveStreamId=%s", encoded_live);
    }
    if (rq->audio_idx >= 0 && n > 0 && n < url_sz)
        n += snprintf(url + n, url_sz - n, "&AudioStreamIndex=%d", rq->audio_idx);
    // Subtitles are burned in server-side (SubtitleMethod=Encode): the PS3
    // client has no subtitle renderer.
    if (rq->sub_idx >= 0 && n > 0 && n < url_sz)
        n += snprintf(url + n, url_sz - n,
                      "&SubtitleStreamIndex=%d&SubtitleMethod=Encode", rq->sub_idx);
    if (play_session_id && play_session_id[0] && n > 0 && n < url_sz)
        n += snprintf(url + n, url_sz - n, "&PlaySessionId=%s", play_session_id);
    return n;
}
