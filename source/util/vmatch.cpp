// Version-name vs title matching -- see vmatch.h.

#include "vmatch.h"
#include <ctype.h>
#include <string.h>

#define VW_MAX 40
#define VW_LEN 24

// ASCII words, lower-cased; anything else (UTF-8, punctuation) separates.
static int words(const char *s, char w[][VW_LEN], int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s && !isalnum((unsigned char)*s)) s++;
        int k = 0;
        while (*s && isalnum((unsigned char)*s)) {
            if (k < VW_LEN - 1) w[n][k++] = (char)tolower((unsigned char)*s);
            s++;
        }
        w[n][k] = '\0';
        if (k) n++;
    }
    return n;
}

static bool is_year(const char *w)
{
    return strlen(w) == 4 && (w[0] == '1' || w[0] == '2') &&
           isdigit((unsigned char)w[1]) && isdigit((unsigned char)w[2]) &&
           isdigit((unsigned char)w[3]);
}

static bool stop_word(const char *w)
{
    static const char *const k[] = { "the", "a", "an", "of", "and", "in", "on" };
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
        if (strcmp(w, k[i]) == 0) return true;
    return false;
}

int version_match_score(const char *label, const char *title)
{
    if (!label || !title) return 0;
    // The release filename: after the folder emoji (U+1F4C1) when present.
    const char *fn = strstr(label, "\xF0\x9F\x93\x81");
    fn = fn ? fn + 4 : label;

    static char tw[VW_MAX][VW_LEN], fw[VW_MAX][VW_LEN], t_sig[VW_MAX][VW_LEN];
    int nt = 0;
    {
        const int n = words(title, tw, VW_MAX);
        for (int i = 0; i < n; i++)
            if (!stop_word(tw[i])) strcpy(t_sig[nt++], tw[i]);
    }
    const int nf = words(fn, fw, VW_MAX);
    int score = 0;

    // Every significant title word somewhere.
    if (nt) {
        int hit = 0;
        for (int a = 0; a < nt; a++)
            for (int b = 0; b < nf; b++)
                if (strcmp(t_sig[a], fw[b]) == 0) { hit++; break; }
        if (hit == nt) score += 1;
    }
    // The title's significant words in order (stop words skipped in the
    // filename), then straight away a year.
    if (nt) {
        for (int s = 0; s < nf; s++) {
            int a = 0, b = s;
            while (a < nt && b < nf) {
                if (stop_word(fw[b])) { b++; continue; }
                if (strcmp(t_sig[a], fw[b]) != 0) break;
                a++; b++;
            }
            if (a == nt && b < nf && is_year(fw[b])) { score += 3; break; }
        }
    }
    for (int b = 0; b < nf; b++) {
        const char *w = fw[b];
        if (!strcmp(w, "complete") || !strcmp(w, "season") || !strcmp(w, "seasons") ||
            !strcmp(w, "episode") || !strcmp(w, "episodes") || !strcmp(w, "collection") ||
            (w[0] == 's' && isdigit((unsigned char)w[1]) && isdigit((unsigned char)w[2]) &&
             (w[3] == '\0' || w[3] == 'e'))) { score -= 4; break; }
    }
    for (int b = 0; b < nf; b++) {
        const char *w = fw[b];
        if (!strcmp(w, "3d") || !strcmp(w, "sbs") || !strcmp(w, "hsbs") ||
            !strcmp(w, "overunder") || !strcmp(w, "ou")) { score -= 2; break; }
    }
    if (strchr(label, '|')) score -= 1;
    return score;
}
