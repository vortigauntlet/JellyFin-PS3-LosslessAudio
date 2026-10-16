// The two small files the SoundCloud tab keeps on the console's disk, as pure parse / format functions
// (the file I/O is sc_svc.cpp's).
//
//   jellyfin_sc.txt            the public web client id, found by scraping soundcloud.com, with the time it was found:
//                                cid <32 alphanumerics>
//                                at <seconds since 1970>
//   jellyfin_sc_settings.txt   what the user chose:
//                                profile <SoundCloud profile name>
//                                quality <160 or 96>
//
// The client id is a public identifier of SoundCloud's own web player, not a secret of the user's, but it is kept
// out of logs and out of the repository all the same (rule 4 of HANDOFF-music-channels.md).
//
// Pure C with no platform includes: the host suite runs this file as it is.
#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char cid[33];          // "" when absent or malformed
    long at;
} ScCidFile;

typedef struct {
    char profile[96];      // "" = not set
    int  quality;          // 160 or 96
} ScSettings;

void sc_cidfile_parse(const char *text, ScCidFile *out);
int  sc_cidfile_format(const ScCidFile *in, char *out, size_t cap);     // bytes written (excluding NUL), or -1

void sc_settings_defaults(ScSettings *out);
void sc_settings_parse(const char *text, ScSettings *out);               // starts from the defaults
int  sc_settings_format(const ScSettings *in, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
