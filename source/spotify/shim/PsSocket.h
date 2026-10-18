// bell::Socket over sx_conn (the music apps' TCP + mbedTLS layer, source/net).  bell's own TCPSocket and TLSSocket use
// getaddrinfo, mbedtls_net and netinet/tcp.h, none of which this platform has.
//
// read() and write() block like bell expects.  A read waits in short slices so close() from another thread (cspot closes the
// AP connection to stop a blocked reader) ends it; the connection itself is released only once no thread is inside it.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <atomic>
#include <stdexcept>
#include <string>

#include "BellSocket.h"
#include "sx_conn.h"

namespace bell {

class PsSocket : public Socket {
 public:
  explicit PsSocket(bool tls) : tls_(tls) {}
  ~PsSocket() { close(); }

  void open(const std::string& host, uint16_t port) {
    sx_neterr err;
    memset(&err, 0, sizeof err);
    c_ = sx_conn_open(host.c_str(), port, tls_, 10000, 15000, 0, &err);
    if (!c_) throw std::runtime_error("Resolve failed");
    closing_ = false;
  }

  // Bytes (>0), 0 once the peer closed or close() was called.  Waits as long as the connection stays open.
  size_t read(uint8_t* buf, size_t len) {
    if (len == 0) return 0;
    busy_++;
    if (closing_) { busy_--; return 0; }
    sx_conn* conn = c_;
    if (!conn) { busy_--; return 0; }
    int n = 0;
    while (!closing_) {
      n = sx_conn_read(conn, buf, (int)len, 500);
      if (n != SX_E_TIMEOUT) break;
    }
    busy_--;
    return n > 0 ? (size_t)n : 0;
  }

  size_t write(uint8_t* buf, size_t len) {
    if (!c_ || closing_) return 0;
    busy_++;
    const int n = sx_conn_write(c_, buf, (int)len, 30000);
    busy_--;
    return n > 0 ? (size_t)n : 0;
  }

  size_t poll() { return 0; }            // nothing in cspot or bell asks how much is waiting
  bool isOpen() { return c_ != nullptr && !closing_; }
  int getFd() { return -1; }

  void close() {
    if (!c_) return;
    closing_ = true;
    for (int i = 0; i < 400 && busy_ > 0; i++) usleep(10000);     // a reader leaves within one 500 ms slice
    sx_conn_close(c_);
    c_ = nullptr;
  }

 private:
  bool tls_;
  sx_conn* c_ = nullptr;
  std::atomic<bool> closing_{false};
  std::atomic<int> busy_{0};
};

class TCPSocket : public PsSocket {
 public:
  TCPSocket() : PsSocket(false) {}
};

class TLSSocket : public PsSocket {
 public:
  TLSSocket() : PsSocket(true) {}
};

}  // namespace bell
