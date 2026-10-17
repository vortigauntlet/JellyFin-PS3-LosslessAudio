// Entropy for cspot's DH keys and nonces.  bell's Crypto.cpp seeds mbedTLS from mbedtls_entropy_func, which on this platform has
// no source of its own (the same reason the TLS layer seeds itself from sx_plat_entropy), so its random bytes would be zeros.
// Its seed callback is replaced by this one: the console's secure random number generator mixed with timing jitter.  A
// failure aborts the key generation rather than returning predictable bytes.
#include <stddef.h>
#include <stdint.h>

#include "sx_plat.h"

extern "C" int sp_entropy_cb(void*, unsigned char* out, size_t n) {
  size_t got = 0;
  while (got < n) {
    unsigned char hw[32];
    const size_t hn = sx_plat_entropy(hw, sizeof hw);
    if (hn == 0) return -1;                       // no hardware randomness: do not make up keys
    uint64_t acc = sx_now_us();
    for (int i = 0; i < 16; i++) {
      volatile unsigned spin = 0;
      for (unsigned k = 0; k < 40 + (unsigned)(acc & 31); k++) spin = spin + k;
      acc = acc * 6364136223846793005ULL + sx_now_us() + spin;
    }
    for (size_t i = 0; i < hn && got < n; i++, got++) out[got] = (unsigned char)(hw[i] ^ (unsigned char)(acc >> ((i & 7) * 8)));
  }
  return 0;
}
