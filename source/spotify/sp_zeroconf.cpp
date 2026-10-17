// Spotify Connect pairing ("zeroconf") for the console: it advertises itself over mDNS as a _spotify-connect._tcp device and
// answers the Spotify app's two HTTP requests (getInfo, addUser) on a small server, which hands the app's encrypted login
// blob to cspot's LoginBlob.  Replaces the CLI target's civetweb server and bell's Linux mDNS responder, neither of which
// this platform can host.
//
// What the phone sends is the account's login credential for this device: nothing here logs it, the HTTP bodies, or the blob.
#include "sp_zeroconf.h"

#include <net/net.h>
#include <net/netctl.h>
#include <net/poll.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/thread.h>
#include <unistd.h>
#include <atomic>
#include <map>
#include <string>
#include <vector>

#include "LoginBlob.h"
#include "sx_plat.h"

namespace {

std::atomic<bool> s_run{false};
std::atomic<bool> s_started{false};
cspot::LoginBlob* s_blob = nullptr;
std::function<void()> s_on_auth;
int s_port = 7864;
std::string s_name = "PS3";
std::string s_host = "ps3";      // the .local name of this console
uint32_t s_ip_be = 0;            // our address, network order
char s_ip_txt[24] = "";

void log(const char* line) { sx_log(line); }

// ---- own address ----------------------------------------------------------------------------------------------

bool read_own_ip() {
  netCtlInit();
  union net_ctl_info info;
  memset(&info, 0, sizeof info);
  if (netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &info) != 0 || !info.ip_address[0]) return false;
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (sscanf(info.ip_address, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
  s_ip_be = htonl((a << 24) | (b << 16) | (c << 8) | d);
  snprintf(s_ip_txt, sizeof s_ip_txt, "%u.%u.%u.%u", a, b, c, d);
  return true;
}

// ---- tiny HTTP server -------------------------------------------------------------------------------------------

std::string url_decode(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); i++) {
    const char c = in[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < in.size() + 0 && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
      out += (char)strtol(in.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

std::map<std::string, std::string> parse_form(const std::string& body) {
  std::map<std::string, std::string> m;
  size_t pos = 0;
  while (pos <= body.size()) {
    size_t amp = body.find('&', pos);
    if (amp == std::string::npos) amp = body.size();
    const std::string kv = body.substr(pos, amp - pos);
    const size_t eq = kv.find('=');
    if (eq != std::string::npos) m[url_decode(kv.substr(0, eq))] = url_decode(kv.substr(eq + 1));
    pos = amp + 1;
  }
  return m;
}

bool send_all(int s, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    pollfd p;
    p.fd = s; p.events = POLLOUT; p.revents = 0;
    if (netPoll(&p, 1, 3000) <= 0) return false;
    const int r = netSend(s, data.data() + sent, data.size() - sent, 0);
    if (r <= 0) return false;
    sent += (size_t)r;
  }
  return true;
}

void respond(int s, int code, const char* text, const std::string& body) {
  char head[160];
  snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n", code, text,
           (unsigned)body.size());
  send_all(s, std::string(head) + body);
}

// Reads one request: headers, then Content-Length bytes of body.  False on a timeout or an oversized request.
bool read_request(int s, std::string& method, std::string& path, std::string& body) {
  std::string buf;
  char tmp[1024];
  size_t head_end = std::string::npos;
  const uint64_t deadline = sx_now_ms() + 5000;
  while (sx_now_ms() < deadline && buf.size() < 32768) {
    if (head_end == std::string::npos || buf.size() < head_end + 4) {
      // still reading the head, or the body has not started
    }
    pollfd p;
    p.fd = s; p.events = POLLIN; p.revents = 0;
    if (netPoll(&p, 1, 500) <= 0) continue;
    const int r = netRecv(s, tmp, sizeof tmp, 0);
    if (r <= 0) break;
    buf.append(tmp, (size_t)r);
    if (head_end == std::string::npos) head_end = buf.find("\r\n\r\n");
    if (head_end != std::string::npos) {
      size_t clen = 0;
      const size_t cl = buf.find("Content-Length:");
      const size_t cl2 = buf.find("content-length:");
      const size_t at = cl != std::string::npos ? cl : cl2;
      if (at != std::string::npos && at < head_end) clen = (size_t)atoi(buf.c_str() + at + 15);
      if (buf.size() >= head_end + 4 + clen) {
        const size_t sp1 = buf.find(' ');
        const size_t sp2 = buf.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
        method = buf.substr(0, sp1);
        path = buf.substr(sp1 + 1, sp2 - sp1 - 1);
        body = buf.substr(head_end + 4, clen);
        return true;
      }
    }
  }
  return false;
}

void handle(int s) {
  std::string method, path, body;
  if (!read_request(s, method, path, body)) return;
  const std::string route = path.substr(0, path.find('?'));
  if (route != "/spotify_info") {
    respond(s, 404, "Not Found", "{}");
    return;
  }
  if (method == "GET") {
    respond(s, 200, "OK", s_blob->buildZeroconfInfo());      // action=getInfo
    return;
  }
  if (method == "POST") {
    auto form = parse_form(body);
    if (form.find("action") != form.end() && form["action"] != "addUser") {
      respond(s, 200, "OK", "{\"status\":101,\"spotifyError\":0,\"statusString\":\"ERROR-OK\"}");
      return;
    }
    log("spotify: pairing request received");
    s_blob->loadZeroconfQuery(form);
    respond(s, 200, "OK", "{\"status\":101,\"spotifyError\":0,\"statusString\":\"ERROR-OK\"}");
    if (s_on_auth) s_on_auth();
    return;
  }
  respond(s, 405, "Method Not Allowed", "{}");
}

void http_thread(void*) {
  const int ls = netSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ls < 0) { log("spotify: http socket failed"); sysThreadExit(0); }
  int one = 1;
  netSetSockOpt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_len = sizeof a;
  a.sin_family = AF_INET;
  a.sin_port = htons((u16)s_port);
  a.sin_addr.s_addr = 0;
  if (netBind(ls, (sockaddr*)&a, sizeof a) < 0 || netListen(ls, 4) < 0) {
    log("spotify: http bind/listen failed");
    netClose(ls);
    sysThreadExit(0);
  }
  log("spotify: pairing server listening");
  while (s_run) {
    pollfd p;
    p.fd = ls; p.events = POLLIN; p.revents = 0;
    if (netPoll(&p, 1, 500) <= 0) continue;
    sockaddr_in c;
    socklen_t cl = sizeof c;
    const int s = netAccept(ls, (sockaddr*)&c, &cl);
    if (s < 0) continue;
    handle(s);
    netClose(s);
  }
  netClose(ls);
  sysThreadExit(0);
}

// ---- mDNS responder ----------------------------------------------------------------------------------------------------

void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, (uint16_t)(x >> 16)); put16(v, (uint16_t)x); }

void put_name(std::vector<uint8_t>& v, const std::string& dotted) {
  size_t pos = 0;
  while (pos < dotted.size()) {
    size_t dot = dotted.find('.', pos);
    if (dot == std::string::npos) dot = dotted.size();
    const size_t len = dot - pos > 63 ? 63 : dot - pos;
    v.push_back((uint8_t)len);
    v.insert(v.end(), dotted.begin() + pos, dotted.begin() + pos + len);
    pos = dot + 1;
  }
  v.push_back(0);
}

void put_rr(std::vector<uint8_t>& v, const std::string& name, uint16_t type, bool flush, const std::vector<uint8_t>& rdata) {
  put_name(v, name);
  put16(v, type);
  put16(v, flush ? 0x8001 : 0x0001);
  put32(v, 120);                               // TTL, seconds
  put16(v, (uint16_t)rdata.size());
  v.insert(v.end(), rdata.begin(), rdata.end());
}

const char* const kType = "_spotify-connect._tcp.local";

std::string instance_name() { return s_name + "." + kType; }
std::string host_name() { return s_host + ".local"; }

// One packet carrying the whole record set: PTR (service -> instance), SRV, TXT, A.  Cheap enough to send for every match.
std::vector<uint8_t> build_response() {
  std::vector<uint8_t> v;
  put16(v, 0); put16(v, 0x8400); put16(v, 0); put16(v, 4); put16(v, 0); put16(v, 0);   // id, flags: response + authoritative; 4 answers

  std::vector<uint8_t> r;
  put_name(r, instance_name());
  put_rr(v, kType, 12, false, r);                                    // PTR

  r.clear();
  put16(r, 0); put16(r, 0); put16(r, (uint16_t)s_port);              // priority, weight, port
  put_name(r, host_name());
  put_rr(v, instance_name(), 33, true, r);                           // SRV

  r.clear();
  const char* txt[] = {"VERSION=1.0", "CPath=/spotify_info", "Stack=SP"};
  for (const char* t : txt) { r.push_back((uint8_t)strlen(t)); r.insert(r.end(), t, t + strlen(t)); }
  put_rr(v, instance_name(), 16, true, r);                           // TXT

  r.clear();
  const uint8_t* ip = (const uint8_t*)&s_ip_be;
  r.insert(r.end(), ip, ip + 4);
  put_rr(v, host_name(), 1, true, r);                                // A
  return v;
}

// Does the question section name something we answer for?
bool query_matches(const uint8_t* p, size_t n) {
  if (n < 12) return false;
  if (p[2] & 0x80) return false;                                     // a response, not a query
  const unsigned qd = (p[4] << 8) | p[5];
  size_t i = 12;
  for (unsigned q = 0; q < qd; q++) {
    std::string name;
    while (i < n && p[i] != 0) {
      const unsigned len = p[i++];
      if (len & 0xC0 || i + len > n) return false;                   // compressed names do not appear in the question we match
      name.append((const char*)p + i, len);
      name += '.';
      i += len;
    }
    i += 1 + 4;                                                      // the root label, type, class
    if (i > n) return false;
    if (!name.empty()) name.pop_back();
    for (auto& ch : name) ch = (char)tolower((unsigned char)ch);
    std::string svc = kType, inst = instance_name(), host = host_name();
    for (auto& ch : inst) ch = (char)tolower((unsigned char)ch);
    for (auto& ch : host) ch = (char)tolower((unsigned char)ch);
    if (name == svc || name == inst || name == host) return true;
  }
  return false;
}

void mdns_thread(void*) {
  const int s = netSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) { log("spotify: mdns socket failed"); sysThreadExit(0); }
  int one = 1;
  netSetSockOpt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  netSetSockOpt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
  sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_len = sizeof a;
  a.sin_family = AF_INET;
  a.sin_port = htons(5353);
  a.sin_addr.s_addr = 0;
  if (netBind(s, (sockaddr*)&a, sizeof a) < 0) log("spotify: mdns bind failed (is another mDNS service on this port?)");
  ip_mreq mr;
  mr.imr_multiaddr.s_addr = htonl(0xE00000FB);                       // 224.0.0.251
  mr.imr_interface.s_addr = s_ip_be;
  if (netSetSockOpt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) < 0) log("spotify: mdns could not join the multicast group");
  int ttl = 255;
  netSetSockOpt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
  int loop = 0;
  netSetSockOpt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);

  sockaddr_in dst;
  memset(&dst, 0, sizeof dst);
  dst.sin_len = sizeof dst;
  dst.sin_family = AF_INET;
  dst.sin_port = htons(5353);
  dst.sin_addr.s_addr = htonl(0xE00000FB);

  const std::vector<uint8_t> resp = build_response();
  uint64_t next_announce = 0;
  int announces = 0;
  uint8_t buf[1500];
  log("spotify: advertising on mDNS");
  while (s_run) {
    const uint64_t now = sx_now_ms();
    if (now >= next_announce) {                                      // unsolicited: at once, again a second later, then each minute
      netSendTo(s, resp.data(), resp.size(), 0, (sockaddr*)&dst, sizeof dst);
      next_announce = now + (announces++ < 2 ? 1000 : 60000);
    }
    pollfd p;
    p.fd = s; p.events = POLLIN; p.revents = 0;
    if (netPoll(&p, 1, 500) <= 0) continue;
    sockaddr_in from;
    socklen_t fl = sizeof from;
    const int n = netRecvFrom(s, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
    if (n > 12 && query_matches(buf, (size_t)n)) netSendTo(s, resp.data(), resp.size(), 0, (sockaddr*)&dst, sizeof dst);
  }
  netClose(s);
  sysThreadExit(0);
}

}  // namespace

bool sp_zc_start(cspot::LoginBlob* blob, const char* device_name, std::function<void()> on_auth) {
  if (s_started) return true;
  if (!read_own_ip()) {
    log("spotify: no network address, cannot pair");
    return false;
  }
  s_blob = blob;
  s_on_auth = on_auth;
  s_name = device_name && *device_name ? device_name : "PS3";
  char hostbuf[40];
  snprintf(hostbuf, sizeof hostbuf, "ps3-%02x%02x", ((const uint8_t*)&s_ip_be)[2], ((const uint8_t*)&s_ip_be)[3]);
  s_host = hostbuf;
  s_run = true;
  s_started = true;
  sys_ppu_thread_t t;
  const bool ok1 = sysThreadCreate(&t, http_thread, nullptr, 1001, 128 * 1024, 0, (char*)"sp_http") == 0;
  const bool ok2 = sysThreadCreate(&t, mdns_thread, nullptr, 1001, 128 * 1024, 0, (char*)"sp_mdns") == 0;
  char b[96];
  snprintf(b, sizeof b, "spotify: pairing on %s:%d as \"%s\" (%s.local)", s_ip_txt, s_port, s_name.c_str(), s_host.c_str());
  log(b);
  return ok1 && ok2;
}

void sp_zc_stop() {
  s_run = false;
  s_started = false;
}
