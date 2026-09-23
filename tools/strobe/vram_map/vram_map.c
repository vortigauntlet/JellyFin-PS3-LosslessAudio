/* vram_map.c -- replay rsxMemalign()/rsxMalloc()/rsxFree() through PSL1GHT's
 * OWN allocator (ppu/librt/heap.c, compiled unmodified from your $PSL1GHT) and
 * print where every RSX-local buffer lands.
 *
 * WHY
 *   The strobe investigation needs "GOOD: X = ..., BAD: X = ..." for the
 *   JellyWave vertex buffers' offsets and neighbours.  rsxMemalign is not a
 *   bump allocator, and two properties of it are not what one would assume:
 *
 *   1. An ALIGNED request (rsxMemalign, i.e. every allocation this app makes)
 *      is carved from the TOP of the first free block that fits
 *      (__heap_check_block computes alloc_begin from block_end), so buffers
 *      are laid out downwards from the end of local memory in call order.
 *      Growing one buffer moves ITS OWN start and every LATER allocation;
 *      nothing allocated before it moves.
 *   2. Block headers (prev_size, size, next, prev -- 32 bytes) live IN local
 *      memory directly below each allocation, so every rsxMemalign/rsxFree
 *      is a PPU read-modify-write of VRAM next to live buffers.
 *
 *   Both are measured by this program, not asserted: it prints them.
 *
 * INPUT (stdin or file), one request per line, '#' comments:
 *     <tag> <alignment> <size>      rsxMemalign(alignment, size)
 *     <tag> 0 <size>                rsxMalloc(size)
 *     free <tag>                    rsxFree(<tag>)
 *   sizes accept 0x.., decimal, and a trailing K/M.
 *
 * usage: vram_map [--local-size 0x0F900000] [script]
 *   --local-size is gcmConfiguration.localSize (heap = localSize - 4096,
 *   mm.c's SAFE_AREA).  Offsets are what rsxAddressToOffset() returns.
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <heap.h>          /* $PSL1GHT/ppu/include/sys/heap.h, via -I */

#define SAFE_AREA 4096
#define MAXA 512

typedef struct { char tag[64]; uintptr_t addr; u64 size, align; int live; } alloc_t;

static alloc_t A[MAXA];
static int NA;

static u64 num(const char *s)
{
    char *e;
    u64 v = strtoull(s, &e, 0);
    if (*e == 'K' || *e == 'k') v <<= 10;
    else if (*e == 'M' || *e == 'm') v <<= 20;
    return v;
}

static int by_addr(const void *a, const void *b)
{
    const alloc_t *x = a, *y = b;
    return (x->addr > y->addr) - (x->addr < y->addr);
}

int main(int argc, char **argv)
{
    u64 local_size = 0x0F900000ull;     /* typical PSL1GHT gcmConfiguration.localSize */
    const char *script = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--local-size") && i + 1 < argc) local_size = num(argv[++i]);
        else script = argv[i];
    }
    FILE *in = script ? fopen(script, "r") : stdin;
    if (!in) { perror(script); return 2; }

    /* 1 MB-aligned stand-in for localAddress, so every alignment the app asks
     * for (<= 256) resolves exactly as it does at 0xC0000000 on the console. */
    size_t span = (size_t)local_size + (1u << 20);
    u8 *raw = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) { perror("mmap"); return 2; }
    u8 *base = (u8 *)(((uintptr_t)raw + 0xFFFFF) & ~(uintptr_t)0xFFFFF);

    static heap_cntrl h;
    heapInit(&h, base, (uintptr_t)(local_size - SAFE_AREA));

    printf("localSize=0x%08llx  heap=[0x%08x .. 0x%08llx)  (offsets as rsxAddressToOffset)\n\n",
           (unsigned long long)local_size, 0, (unsigned long long)(local_size - SAFE_AREA));
    printf("%-4s %-22s %6s %10s  %-10s %-10s  %s\n", "#", "tag", "align", "size", "offset", "end", "note");

    char line[256];
    int seq = 0;
    while (fgets(line, sizeof line, in)) {
        char *h0 = strchr(line, '#'); if (h0) *h0 = 0;
        char t[64], b[64], c[64];
        int n = sscanf(line, "%63s %63s %63s", t, b, c);
        if (n <= 0) continue;
        if (n == 2 && !strcmp(t, "free")) {
            for (int i = NA - 1; i >= 0; i--)
                if (A[i].live && !strcmp(A[i].tag, b)) {
                    heapFree(&h, (void *)A[i].addr);
                    A[i].live = 0;
                    printf("%-4d %-22s %6s %10s  %-10s %-10s  rsxFree\n", ++seq, b, "", "", "", "");
                    break;
                }
            continue;
        }
        if (n != 3 || NA >= MAXA) continue;
        u64 al = num(b), sz = num(c);
        void *p = al ? heapAllocateAligned(&h, sz, al) : heapAllocate(&h, sz);
        if (!p) { printf("%-4d %-22s %6llu %10llu  ALLOC FAILED\n", ++seq, t,
                         (unsigned long long)al, (unsigned long long)sz); continue; }
        alloc_t *x = &A[NA++];
        snprintf(x->tag, sizeof x->tag, "%s", t);
        x->addr = (uintptr_t)p; x->size = sz; x->align = al; x->live = 1;
        u64 off = (u64)((u8 *)p - base);
        printf("%-4d %-22s %6llu %10llu  0x%08llx 0x%08llx  %s\n", ++seq, t,
               (unsigned long long)al, (unsigned long long)sz,
               (unsigned long long)off, (unsigned long long)(off + sz),
               al ? "" : "(rsxMalloc: from the BOTTOM of the first free block)");
    }

    /* Address-ordered map with the gap to each neighbour: what sits directly
     * above and below every buffer, header included. */
    alloc_t S[MAXA]; int ns = 0;
    for (int i = 0; i < NA; i++) if (A[i].live) S[ns++] = A[i];
    qsort(S, ns, sizeof S[0], by_addr);
    printf("\nADDRESS ORDER (low -> high).  gap = bytes between the end of one buffer and the\n"
           "start of the next; the next buffer's %d-byte block header sits at the top of that gap.\n\n",
           (int)HEAP_BLOCK_HEADER_SIZE);
    for (int i = 0; i < ns; i++) {
        u64 off = (u64)((u8 *)S[i].addr - base);
        printf("  0x%08llx..0x%08llx  %-22s size=%-9llu",
               (unsigned long long)off, (unsigned long long)(off + S[i].size), S[i].tag,
               (unsigned long long)S[i].size);
        if (i + 1 < ns) {
            u64 noff = (u64)((u8 *)S[i + 1].addr - base);
            printf("  gap-to-next=%lld (next: %s)", (long long)(noff - (off + S[i].size)), S[i + 1].tag);
        }
        printf("\n");
    }
    return 0;
}
