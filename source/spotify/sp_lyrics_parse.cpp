// See sp_lyrics_parse.h.
#include "sp_lyrics_parse.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "nlohmann/json.hpp"

namespace {

using Json = nlohmann::json;

// Trims, folds whitespace runs and caps the length on a UTF-8 boundary.  The music-note glyph Spotify puts in instrumental gaps
// is not in the console fonts, so it becomes three dots.
std::string clean(const char *s, size_t n) {
  std::string out;
  out.reserve(n < LYRICS_MAX_TEXT ? n : LYRICS_MAX_TEXT);
  bool space = false;
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)s[i];
    if (c == '\r' || c == '\n' || c == '\t' || c == ' ') { space = !out.empty(); continue; }
    if (c == 0xE2 && i + 2 < n && (unsigned char)s[i + 1] == 0x99 &&
        ((unsigned char)s[i + 2] == 0xAA || (unsigned char)s[i + 2] == 0xAB || (unsigned char)s[i + 2] == 0xAC)) {
      if (space) out += ' ';
      out += "...";
      space = false;
      i += 2;
      continue;
    }
    if (space) { out += ' '; space = false; }
    out += (char)c;
  }
  if (out.size() > LYRICS_MAX_TEXT) {
    size_t cut = LYRICS_MAX_TEXT;
    while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) cut--;
    out.resize(cut);
  }
  return out;
}

std::string clean(const std::string &s) { return clean(s.data(), s.size()); }

// "mm:ss", "mm:ss.x", "mm:ss.xx", "mm:ss.xxx" inside the brackets.  False for a tag such as "ar:Name".
bool stamp_ms(const char *s, size_t n, uint32_t *ms) {
  size_t i = 0;
  uint32_t mm = 0, ss = 0, frac = 0, fdig = 0;
  if (i >= n || s[i] < '0' || s[i] > '9') return false;
  while (i < n && s[i] >= '0' && s[i] <= '9') { mm = mm * 10 + (uint32_t)(s[i] - '0'); i++; if (mm > 999) return false; }
  if (i >= n || s[i] != ':') return false;
  i++;
  int sd = 0;
  while (i < n && s[i] >= '0' && s[i] <= '9') { ss = ss * 10 + (uint32_t)(s[i] - '0'); i++; sd++; }
  if (sd == 0 || sd > 2 || ss > 59) return false;
  if (i < n && (s[i] == '.' || s[i] == ':')) {
    i++;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
      if (fdig < 3) { frac = frac * 10 + (uint32_t)(s[i] - '0'); fdig++; }
      i++;
    }
  }
  if (i != n) return false;
  while (fdig < 3) { frac *= 10; fdig++; }
  *ms = (mm * 60 + ss) * 1000 + frac;
  return true;
}

void finish(std::vector<LyricLine> &out) {
  std::stable_sort(out.begin(), out.end(), [](const LyricLine &a, const LyricLine &b) { return a.ms < b.ms; });
  if (out.size() > LYRICS_MAX_LINES) out.resize(LYRICS_MAX_LINES);
}

uint32_t json_ms(const Json &v) {
  if (v.is_number_unsigned()) return v.get<uint32_t>();
  if (v.is_number_integer()) { const long long x = v.get<long long>(); return x < 0 ? 0 : (uint32_t)x; }
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    unsigned long x = 0;
    for (char c : s) { if (c < '0' || c > '9') break; x = x * 10 + (unsigned long)(c - '0'); if (x > 100000000UL) break; }
    return (uint32_t)x;
  }
  return 0;
}

bool lrclib_object(const Json &o, std::vector<LyricLine> &out, bool *synced) {
  if (!o.is_object()) return false;
  auto s = o.find("syncedLyrics");
  if (s != o.end() && s->is_string()) {
    const std::string t = s->get<std::string>();
    if (lrc_parse(t.data(), t.size(), out)) { *synced = true; return true; }
  }
  auto p = o.find("plainLyrics");
  if (p != o.end() && p->is_string()) {
    const std::string t = p->get<std::string>();
    plain_parse(t.data(), t.size(), out);
    if (!out.empty()) { *synced = false; return true; }
  }
  return false;
}

}  // namespace

bool lrc_parse(const char *text, size_t len, std::vector<LyricLine> &out) {
  out.clear();
  size_t i = 0;
  while (i < len && out.size() < LYRICS_MAX_LINES) {
    size_t e = i;
    while (e < len && text[e] != '\n') e++;
    // one or more [stamp] prefixes, then the words
    std::vector<uint32_t> stamps;
    size_t p = i;
    while (p < e && (text[p] == ' ' || text[p] == '\t')) p++;
    while (p < e && text[p] == '[') {
      size_t q = p + 1;
      while (q < e && text[q] != ']') q++;
      if (q >= e) break;
      uint32_t ms;
      if (stamp_ms(text + p + 1, q - p - 1, &ms)) stamps.push_back(ms);
      p = q + 1;
    }
    if (!stamps.empty()) {
      const std::string words = clean(text + p, e - p);
      for (uint32_t ms : stamps) out.push_back({ms, words});
    }
    i = e + 1;
  }
  finish(out);
  // an LRC with only blank timed lines is no lyrics
  for (const auto &l : out) if (!l.text.empty()) return true;
  out.clear();
  return false;
}

void plain_parse(const char *text, size_t len, std::vector<LyricLine> &out) {
  out.clear();
  size_t i = 0;
  while (i < len && out.size() < LYRICS_MAX_LINES) {
    size_t e = i;
    while (e < len && text[e] != '\n') e++;
    std::string words = clean(text + i, e - i);
    if (!words.empty()) out.push_back({0, words});
    i = e + 1;
  }
}

bool spotify_lyrics_parse(const char *json, size_t len, std::vector<LyricLine> &out, bool *synced) {
  out.clear();
  Json root = Json::parse(json, json + len, nullptr, false);
  if (root.is_discarded() || !root.is_object()) return false;
  auto ly = root.find("lyrics");
  if (ly == root.end() || !ly->is_object()) return false;
  auto lines = ly->find("lines");
  if (lines == ly->end() || !lines->is_array()) return false;
  auto st = ly->find("syncType");
  const bool timed = st != ly->end() && st->is_string() && st->get<std::string>() == "LINE_SYNCED";
  for (const auto &l : *lines) {
    if (out.size() >= LYRICS_MAX_LINES) break;
    if (!l.is_object()) continue;
    auto w = l.find("words");
    if (w == l.end() || !w->is_string()) continue;
    std::string words = clean(w->get<std::string>());
    auto t = l.find("startTimeMs");
    const uint32_t ms = timed && t != l.end() ? json_ms(*t) : 0;
    if (!timed && words.empty()) continue;
    out.push_back({ms, words});
  }
  if (timed) finish(out);
  *synced = timed;
  bool any = false;
  for (const auto &l : out) if (!l.text.empty() && l.text != "...") any = true;
  if (!any) out.clear();
  return any;
}

bool lrclib_parse(const char *json, size_t len, std::vector<LyricLine> &out, bool *synced) {
  out.clear();
  Json root = Json::parse(json, json + len, nullptr, false);
  if (root.is_discarded()) return false;
  if (root.is_object()) return lrclib_object(root, out, synced);
  if (!root.is_array()) return false;
  // a search: the first entry with timed lyrics, else the first with any
  for (const auto &o : root) {
    auto s = o.is_object() ? o.find("syncedLyrics") : o.end();
    if (o.is_object() && s != o.end() && s->is_string() && lrclib_object(o, out, synced) && *synced) return true;
  }
  for (const auto &o : root)
    if (lrclib_object(o, out, synced)) return true;
  out.clear();
  return false;
}

int lyric_index_at(const std::vector<LyricLine> &lines, uint32_t ms) {
  int lo = 0, hi = (int)lines.size() - 1, best = -1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (lines[(size_t)mid].ms <= ms) { best = mid; lo = mid + 1; }
    else hi = mid - 1;
  }
  return best;
}

bool sp_hex_to_base62(const char *hex, char out[23]) {
  static const char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  if (!hex || strlen(hex) != 32) return false;
  uint8_t n[16];
  for (int i = 0; i < 16; i++) {
    int v = 0;
    for (int k = 0; k < 2; k++) {
      const char c = hex[i * 2 + k];
      int d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
      else return false;
      v = v * 16 + d;
    }
    n[i] = (uint8_t)v;
  }
  // repeated division of the 128-bit big-endian number by 62; 22 digits always (zero-padded)
  for (int pos = 21; pos >= 0; pos--) {
    unsigned rem = 0;
    for (int i = 0; i < 16; i++) {
      const unsigned cur = rem * 256 + n[i];
      n[i] = (uint8_t)(cur / 62);
      rem = cur % 62;
    }
    out[pos] = kDigits[rem];
  }
  out[22] = '\0';
  return true;
}

std::string url_encode(const char *s) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (; s && *s; s++) {
    const unsigned char c = (unsigned char)*s;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}
