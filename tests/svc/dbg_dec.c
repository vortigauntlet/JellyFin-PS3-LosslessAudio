// Debug helper (not part of the suite): prints what the decode chain produces for the first frames of a fixture.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fmp4.h"
#include "sc_aac.h"

static uint8_t *slurp(const char *dir, const char *name, size_t *len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    *len = (size_t)n;
    return b;
}

int main(int argc, char **argv) {
    size_t il, sl;
    uint8_t *ib = slurp(argv[1], "init.mp4", &il);
    uint8_t *sb = slurp(argv[1], "data001.m4s", &sl);
    fmp4_init in;
    printf("init rc=%d\n", fmp4_parse_init(ib, il, &in));
    printf("ts=%u ch=%d rate=%d aot=%d asc=%d: %02x %02x\n", in.timescale, in.channels, in.rate, in.aot, in.asc_len, in.asc[0], in.asc[1]);
    fmp4_seg sg;
    printf("seg n=%d\n", fmp4_parse_segment(sb, sl, &in, &sg));
    ScAac *a = scaac_open(in.asc, in.asc_len);
    printf("aac=%p\n", (void *)a);
    float out[4096];
    for (int f = 0; f < 6; f++) {
        const int n = scaac_decode(a, sb + sg.s[f].off, (int)sg.s[f].size, out, 2048);
        float mx = 0;
        for (int i = 0; i < (n > 0 ? n * 2 : 0); i++) if (fabsf(out[i]) > mx) mx = fabsf(out[i]);
        printf("frame %d size %u -> n=%d rate=%d max=%g first=%g %g %g\n", f, sg.s[f].size, n, scaac_rate(a), mx, n > 0 ? out[0] : 0, n > 0 ? out[2] : 0, n > 0 ? out[100] : 0);
    }
    return 0;
}
