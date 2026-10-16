// PS3 (lv2 / libnet) implementation of sx_plat.h.
#include "sx_plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ppu-types.h>
#include <ppu-lv2.h>
#include <net/net.h>
#include <net/socket.h>
#include <net/poll.h>
#include <netinet/in.h>
#include <sys/mutex.h>
#include <sys/thread.h>
#include <unistd.h>

#include "plog.h"
#include "timing.h"

// ---- clock, log, sleep ---------------------------------------------------------

extern "C" uint64_t sx_now_us(void) { return timing_get_us(); }
extern "C" uint64_t sx_now_ms(void) { return timing_get_us() / 1000ULL; }
extern "C" void     sx_sleep_ms(unsigned ms) { usleep(ms * 1000u); }
extern "C" void     sx_log(const char *line) { plog(line); }

// ---- mutex --------------------------------------------------------------------------

struct sx_mutex { sys_mutex_t m; };

extern "C" sx_mutex *sx_mutex_create(void) {
    sx_mutex *x = (sx_mutex *)calloc(1, sizeof(*x));
    if (!x) return NULL;
    sys_mutex_attr_t attr;
    sysMutexAttrInitialize(attr);          // a zeroed attr is rejected by lv2
    attr.attr_protocol  = SYS_MUTEX_PROTOCOL_FIFO;
    attr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    if (sysMutexCreate(&x->m, &attr) != 0) { free(x); return NULL; }
    return x;
}
extern "C" void sx_mutex_lock(sx_mutex *x)   { if (x) sysMutexLock(x->m, 0); }
extern "C" void sx_mutex_unlock(sx_mutex *x) { if (x) sysMutexUnlock(x->m); }

// ---- threads ------------------------------------------------------------------------------

struct sx_thread_start { void (*fn)(void *); void *arg; };

static void sx_thread_entry(void *p) {
    sx_thread_start st = *(sx_thread_start *)p;
    free(p);
    st.fn(st.arg);
    sysThreadExit(0);
}

extern "C" bool sx_thread_spawn(void (*fn)(void *), void *arg, int priority, int stack_kb, const char *name) {
    sx_thread_start *st = (sx_thread_start *)malloc(sizeof(*st));
    if (!st) return false;
    st->fn = fn; st->arg = arg;
    static char fixed[24];                       // sysThreadCreate takes char *; the kernel copies the name
    snprintf(fixed, sizeof(fixed), "%.20s", name ? name : "sx");
    sys_ppu_thread_t t;
    // Flag 0 = detached: nobody joins these.
    if (sysThreadCreate(&t, sx_thread_entry, st, priority, (u64)stack_kb * 1024, 0, fixed) != 0) { free(st); return false; }
    return true;
}

// ---- sockets ---------------------------------------------------------------------------

// libnet's resolver and connect setup are not reentrant: serialise the part that
// touches shared state.  The wait for the connect to complete runs outside it.
static sx_mutex *s_net_mtx;

static sx_mutex *net_mutex(void) {
    if (!s_net_mtx) s_net_mtx = sx_mutex_create();     // first use is the start-up thread
    return s_net_mtx;
}

#define DNS_N   16
#define DNS_TTL 300000ULL
struct dns_ent { char host[128]; u32 ip; uint64_t stamp; };
static dns_ent s_dns[DNS_N];
static int     s_dns_next;

static bool host_is_ipv4(const char *h) {
    int dots = 0;
    for (const char *p = h; *p; p++) {
        if (*p == '.') dots++;
        else if (*p < '0' || *p > '9') return false;
    }
    return dots == 3;
}

// Network byte order, or 0.  Caller holds the net mutex.
static u32 resolve_locked(const char *host) {
    if (host_is_ipv4(host)) {
        unsigned a = 0, b = 0, c = 0, d = 0;
        sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d);
        return htonl((a << 24) | (b << 16) | (c << 8) | d);
    }
    const uint64_t now = sx_now_ms();
    for (int i = 0; i < DNS_N; i++)
        if (s_dns[i].ip && now - s_dns[i].stamp < DNS_TTL && strcmp(s_dns[i].host, host) == 0)
            return s_dns[i].ip;
    struct net_hostent *he = netGetHostByName(host);
    if (!he) return 0;
    u32 *addr_list = (u32 *)(u64)he->h_addr_list;
    if (!addr_list || !addr_list[0]) return 0;
    u32 *in = (u32 *)(u64)addr_list[0];
    const u32 ip = *in;                                   // already network byte order
    if (strlen(host) < sizeof(s_dns[0].host)) {
        dns_ent *e = &s_dns[s_dns_next++ % DNS_N];
        snprintf(e->host, sizeof(e->host), "%s", host);
        e->ip = ip;
        e->stamp = now;
    }
    return ip;
}

extern "C" int sx_sock_connect(const char *host, int port, int rcvbuf, int connect_ms) {
    sx_mutex *m = net_mutex();
    sx_mutex_lock(m);
    const u32 ip = resolve_locked(host);
    if (ip == 0) { sx_mutex_unlock(m); return SX_E_DNS; }

    int sock = netSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) { sx_mutex_unlock(m); return SX_E_CONNECT; }
    // SO_RCVBUF has to be set before connect: the window scale is negotiated in the SYN.
    if (rcvbuf > 0) netSetSockOpt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((u16)port);
    addr.sin_addr.s_addr = ip;

    int nb = 1;
    netSetSockOpt(sock, SOL_SOCKET, SO_NBIO, &nb, sizeof(nb));
    const int cr = netConnect(sock, (struct sockaddr *)&addr, sizeof(addr));
    sx_mutex_unlock(m);

    if (cr < 0) {
        struct pollfd pfd;
        pfd.fd = sock; pfd.events = POLLOUT; pfd.revents = 0;
        const int pr = netPoll(&pfd, 1, connect_ms);
        if (pr == 0) { netClose(sock); return SX_E_TIMEOUT; }
        if (pr < 0 || !(pfd.revents & POLLOUT)) { netClose(sock); return SX_E_CONNECT; }
        int soerr = 0; socklen_t sl = sizeof(soerr);
        if (netGetSockOpt(sock, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr != 0) {
            netClose(sock);
            return SX_E_CONNECT;
        }
    }
    // Stay non-blocking: every read and write is bounded by netPoll, never by SO_RCVTIMEO.
    return sock;
}

extern "C" int sx_sock_recv(int fd, void *buf, int n, int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
    const int pr = netPoll(&pfd, 1, timeout_ms);
    if (pr == 0) return SX_E_TIMEOUT;
    if (pr < 0) return SX_E_IO;
    const int r = netRecv(fd, buf, n, MSG_DONTWAIT);
    if (r > 0) return r;
    if (r == 0) return 0;
    const int e = net_errno;
    if (e == NET_EAGAIN || e == NET_EINTR || e == NET_ETIMEDOUT) return SX_E_TIMEOUT;
    return SX_E_IO;
}

extern "C" int sx_sock_send(int fd, const void *buf, int n, int timeout_ms) {
    const uint64_t deadline = sx_now_ms() + (uint64_t)timeout_ms;
    int sent = 0;
    while (sent < n) {
        const uint64_t now = sx_now_ms();
        if (now >= deadline) return SX_E_TIMEOUT;
        struct pollfd pfd;
        pfd.fd = fd; pfd.events = POLLOUT; pfd.revents = 0;
        const int pr = netPoll(&pfd, 1, (int)(deadline - now));
        if (pr == 0) return SX_E_TIMEOUT;
        if (pr < 0) return SX_E_IO;
        const int r = netSend(fd, (const char *)buf + sent, n - sent, MSG_DONTWAIT);
        if (r > 0) { sent += r; continue; }
        const int e = net_errno;
        if (r < 0 && (e == NET_EAGAIN || e == NET_EINTR)) continue;
        return SX_E_IO;
    }
    return n;
}

extern "C" void sx_sock_close(int fd) { if (fd >= 0) netClose(fd); }

extern "C" int sx_sock_idle_ok(int fd) {
    struct pollfd pfd;
    pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
    const int pr = netPoll(&pfd, 1, 0);
    return pr == 0 ? 1 : 0;      // readable, hung up or errored: not reusable
}

// ---- entropy -------------------------------------------------------------------------------

// sys_ss_random_number_generator (lv2 syscall 865, package 2): the unprivileged
// random source.  A refusal returns an error code rather than faulting, so the
// result is checked and the caller always mixes in timing jitter as well.
LV2_SYSCALL sx_sys_ss_random(u64 pkg_id, void *buf, u64 size)
{
    lv2syscall3(865, pkg_id, (u64)(uintptr_t)buf, size);
    return_to_user_prog(s32);
}

extern "C" size_t sx_plat_entropy(unsigned char *buf, size_t n) {
    static int s_logged;
    size_t got = 0;
    while (got < n) {
        unsigned char tmp[16] __attribute__((aligned(16)));
        const s32 rc = sx_sys_ss_random(2, tmp, sizeof(tmp));
        if (rc != 0) {
            if (!s_logged) {
                s_logged = 1;
                char b[64];
                snprintf(b, sizeof(b), "tls: ss random refused rc=0x%08x", (unsigned)rc);
                plog(b);
            }
            break;
        }
        const size_t take = (n - got < sizeof(tmp)) ? n - got : sizeof(tmp);
        memcpy(buf + got, tmp, take);
        got += take;
    }
    return got;
}
