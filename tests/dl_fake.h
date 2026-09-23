#pragma once
// Host-side stand-in for source/offline/dl_platform.h -- see dl_fake.cpp.
//
// Filesystem calls hit the REAL filesystem (a temp directory per test), so
// atomic writes, renames and restores are exercised for real.  The network
// is a scripted fake server: each connection pops the next FakeResp from a
// queue (or uses the default), reads the request the download core sends,
// and answers with a generated body whose every byte is a known function of
// its offset -- so a finished file can be verified without keeping a copy.

#include <stdint.h>
#include <string>
#include <vector>

struct FakeResp {
    bool        refuse         = false;  // connect() fails
    int         status         = 0;      // 0 = 200, or 206 for an honoured range
    bool        honor_range    = true;   // answer Range with 206
    bool        chunked        = false;
    bool        accept_ranges  = true;
    bool        no_length      = false;  // 200 without Content-Length (close-delimited)
    std::string content_type   = "video/mp2t";
    int64_t     total          = -1;     // -1 = g_fake_total
    int64_t     range_start_lie = -1;    // claim this start in Content-Range
    int64_t     total_lie      = -1;     // claim this total in Content-Range/length
    int64_t     drop_after     = -1;     // close after this many body bytes
    int64_t     reset_after    = -1;     // hard error after this many body bytes
    int64_t     stall_after    = -1;     // timeouts forever after this many
    bool        head_stall     = false;  // never send the head
    std::string raw;                     // send exactly this, then close
    bool        close_now      = false;  // close before sending anything
    int         recv_chunk     = 7919;   // bytes per recv (prime: odd splits)
    int         timeout_every  = 0;      // >0: a timeout before every Nth data read
    // Called with the body offset reached, before each recv returns data.
    void      (*on_body)(int64_t body_sent) = nullptr;
};

// Controls
extern std::vector<FakeResp>    g_fake_queue;     // consumed front-first
extern FakeResp                 g_fake_default;
extern std::vector<std::string> g_fake_requests;  // every request received
extern int64_t                  g_fake_total;     // default content size
extern uint64_t                 g_fake_free;      // UINT64_MAX = unknown
extern int64_t                  g_fake_write_budget; // -1 = unlimited
extern uint64_t                 g_fake_now_ms;
extern int                      g_fake_connects;
extern bool                     g_fake_verbose;
extern int                      g_fake_data_recvs;  // recvs that returned bytes
extern std::vector<int>         g_fake_write_sizes; // every media write, in order
extern void                   (*g_fake_on_sleep)(void); // called on every dl_plat_sleep_ms

void    fake_reset(void);
uint8_t fake_byte(uint64_t offset);
// True if the file at path is exactly `size` bytes of the fake pattern.
bool    fake_file_matches(const char *path, int64_t size);
// Fresh temporary directory (removed by fake_rmtree).
std::string fake_mkdtemp(void);
void        fake_rmtree(const std::string &dir);
