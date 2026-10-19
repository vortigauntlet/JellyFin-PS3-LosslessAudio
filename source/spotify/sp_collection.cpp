// See sp_collection.h.  A minimal protobuf wire reader/writer: varints and length-delimited fields are all these messages use.
#include "sp_collection.h"

#include <string.h>

namespace {

void put_varint(std::string &out, uint64_t v) {
  while (v >= 0x80) { out += (char)((v & 0x7F) | 0x80); v >>= 7; }
  out += (char)v;
}

void put_string(std::string &out, int field, const std::string &s) {
  put_varint(out, ((uint64_t)field << 3) | 2);
  put_varint(out, s.size());
  out += s;
}

struct Reader {
  const uint8_t *p, *end;
  bool ok = true;

  bool more() const { return ok && p < end; }
  uint64_t varint() {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (p >= end) { ok = false; return 0; }
      const uint8_t b = *p++;
      v |= (uint64_t)(b & 0x7F) << shift;
      if (!(b & 0x80)) return v;
    }
    ok = false;
    return 0;
  }
  // Reads a field's key; *wire is its wire type.  Length-delimited payloads come back in (*sub, *sub_len).
  bool field(int *num, int *wire, const uint8_t **sub, size_t *sub_len, uint64_t *value) {
    const uint64_t key = varint();
    if (!ok) return false;
    *num = (int)(key >> 3);
    *wire = (int)(key & 7);
    *sub = nullptr;
    *sub_len = 0;
    *value = 0;
    switch (*wire) {
      case 0: *value = varint(); break;
      case 1: if (end - p < 8) ok = false; else p += 8; break;
      case 2: {
        const uint64_t n = varint();
        if (!ok || n > (uint64_t)(end - p)) { ok = false; break; }
        *sub = p;
        *sub_len = (size_t)n;
        p += n;
        break;
      }
      case 5: if (end - p < 4) ok = false; else p += 4; break;
      default: ok = false;
    }
    return ok;
  }
};

bool parse_item(const uint8_t *d, size_t n, std::string &uri, bool &removed) {
  Reader r{d, d + n};
  uri.clear();
  removed = false;
  while (r.more()) {
    int num, wire;
    const uint8_t *sub;
    size_t sl;
    uint64_t v;
    if (!r.field(&num, &wire, &sub, &sl, &v)) return false;
    if (num == 1 && wire == 2) uri.assign((const char *)sub, sl);
    else if (num == 3 && wire == 0) removed = v != 0;
  }
  return r.ok;
}

}  // namespace

std::string collection_page_request(const std::string &username, const char *set, const std::string &token, int limit) {
  std::string out;
  put_string(out, 1, username);
  put_string(out, 2, set);
  if (!token.empty()) put_string(out, 3, token);
  put_varint(out, (4 << 3) | 0);
  put_varint(out, (uint64_t)limit);
  return out;
}

bool collection_page_parse(const uint8_t *data, size_t len, std::vector<std::string> &uris, std::string &next_token) {
  uris.clear();
  next_token.clear();
  Reader r{data, data + len};
  while (r.more()) {
    int num, wire;
    const uint8_t *sub;
    size_t sl;
    uint64_t v;
    if (!r.field(&num, &wire, &sub, &sl, &v)) return false;
    if (num == 1 && wire == 2) {
      std::string uri;
      bool removed;
      if (!parse_item(sub, sl, uri, removed)) return false;
      if (!removed && uri.compare(0, 14, "spotify:track:") == 0) uris.push_back(uri);
    } else if (num == 2 && wire == 2) {
      next_token.assign((const char *)sub, sl);
    }
  }
  return r.ok;
}

bool sp_uri_to_hex(const char *uri, char out[33]) {
  static const char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  if (!uri || strncmp(uri, "spotify:track:", 14) != 0) return false;
  const char *id = uri + 14;
  if (strlen(id) != 22) return false;
  uint8_t n[16] = {0};
  for (int i = 0; i < 22; i++) {
    const char *at = strchr(kDigits, id[i]);
    if (!at || !id[i]) return false;
    unsigned carry = (unsigned)(at - kDigits);
    for (int k = 15; k >= 0; k--) {               // n = n * 62 + digit
      const unsigned cur = (unsigned)n[k] * 62 + carry;
      n[k] = (uint8_t)(cur & 0xFF);
      carry = cur >> 8;
    }
    if (carry) return false;                      // above 128 bits
  }
  static const char kHex[] = "0123456789abcdef";
  for (int i = 0; i < 16; i++) { out[i * 2] = kHex[n[i] >> 4]; out[i * 2 + 1] = kHex[n[i] & 15]; }
  out[32] = '\0';
  return true;
}
