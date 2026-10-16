// Host (Linux) implementation of sx_plat.h, so the portable network code runs
// under the test suite.  Not part of the PS3 build.
#define _GNU_SOURCE
#include "sx_plat.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

uint64_t sx_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
uint64_t sx_now_ms(void) { return sx_now_us() / 1000ULL; }
void sx_sleep_ms(unsigned ms) { usleep(ms * 1000u); }

void sx_log(const char *line) {
    if (getenv("SX_VERBOSE")) fprintf(stderr, "[sx] %s\n", line);
}

struct sx_mutex { pthread_mutex_t m; };
sx_mutex *sx_mutex_create(void) {
    sx_mutex *m = (sx_mutex *)calloc(1, sizeof(*m));
    if (m) pthread_mutex_init(&m->m, NULL);
    return m;
}
void sx_mutex_lock(sx_mutex *m)   { if (m) pthread_mutex_lock(&m->m); }
void sx_mutex_unlock(sx_mutex *m) { if (m) pthread_mutex_unlock(&m->m); }

struct thr_start { void (*fn)(void *); void *arg; };
static void *thr_entry(void *p) {
    struct thr_start st = *(struct thr_start *)p;
    free(p);
    st.fn(st.arg);
    return NULL;
}

bool sx_thread_spawn(void (*fn)(void *), void *arg, int priority, int stack_kb, const char *name) {
    (void)priority; (void)stack_kb; (void)name;
    struct thr_start *st = (struct thr_start *)malloc(sizeof(*st));
    if (!st) return false;
    st->fn = fn; st->arg = arg;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    const int rc = pthread_create(&t, &at, thr_entry, st);
    pthread_attr_destroy(&at);
    if (rc != 0) { free(st); return false; }
    return true;
}

int sx_sock_connect(const char *host, int port, int rcvbuf, int connect_ms) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[8];
    snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return SX_E_DNS;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return SX_E_CONNECT; }
    if (rcvbuf > 0) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return SX_E_CONNECT; }
    if (rc < 0) {
        struct pollfd p = { fd, POLLOUT, 0 };
        int pr = poll(&p, 1, connect_ms);
        if (pr == 0) { close(fd); return SX_E_TIMEOUT; }
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        if (pr < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr != 0) {
            close(fd);
            return SX_E_CONNECT;
        }
    }
    return fd;
}

int sx_sock_recv(int fd, void *buf, int n, int timeout_ms) {
    struct pollfd p = { fd, POLLIN, 0 };
    int pr = poll(&p, 1, timeout_ms);
    if (pr == 0) return SX_E_TIMEOUT;
    if (pr < 0) return errno == EINTR ? SX_E_TIMEOUT : SX_E_IO;
    ssize_t r = recv(fd, buf, (size_t)n, MSG_DONTWAIT);
    if (r > 0) return (int)r;
    if (r == 0) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return SX_E_TIMEOUT;
    return SX_E_IO;
}

int sx_sock_send(int fd, const void *buf, int n, int timeout_ms) {
    const uint64_t deadline = sx_now_ms() + (uint64_t)timeout_ms;
    int sent = 0;
    while (sent < n) {
        const uint64_t now = sx_now_ms();
        if (now >= deadline) return SX_E_TIMEOUT;
        struct pollfd p = { fd, POLLOUT, 0 };
        int pr = poll(&p, 1, (int)(deadline - now));
        if (pr == 0) return SX_E_TIMEOUT;
        if (pr < 0) { if (errno == EINTR) continue; return SX_E_IO; }
        ssize_t r = send(fd, (const char *)buf + sent, (size_t)(n - sent), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (r > 0) { sent += (int)r; continue; }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        return SX_E_IO;
    }
    return n;
}

void sx_sock_close(int fd) { if (fd >= 0) close(fd); }

int sx_sock_idle_ok(int fd) {
    struct pollfd p = { fd, POLLIN, 0 };
    int pr = poll(&p, 1, 0);
    if (pr < 0) return 0;
    return pr == 0 ? 1 : 0;
}

size_t sx_plat_entropy(unsigned char *buf, size_t n) {
    ssize_t r = getrandom(buf, n, 0);
    return r > 0 ? (size_t)r : 0;
}
