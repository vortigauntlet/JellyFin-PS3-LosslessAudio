#!/usr/bin/env python3
"""Test server for test_sx_net.c: plain HTTP and several TLS listeners.

usage: sx_net_server.py <certdir> <portfile>
Writes "<name> <port>" lines to portfile once every listener is up.
"""
import http.server
import socketserver
import ssl
import sys
import threading
import time
import os
import json

CERTS = sys.argv[1]
PORTFILE = sys.argv[2]

PATTERN_LEN = 1 << 20


def pattern(a, b):
    """Bytes a..b inclusive of the deterministic 1 MiB test file."""
    return bytes(((i * 7) + (i >> 8)) & 255 for i in range(a, b + 1))


class Stats:
    lock = threading.Lock()
    conns = 0


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def setup(self):
        super().setup()
        with Stats.lock:
            Stats.conns += 1

    def reply(self, code, body=b"", headers=None, chunked=False):
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            return
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n)
        self.reply(200, json.dumps({"len": len(body), "ct": self.headers.get("Content-Type"),
                                    "body": body.decode("latin1")}).encode(),
                   {"Content-Type": "application/json"})

    def do_GET(self):
        p = self.path
        if p == "/ok":
            self.reply(200, b"hello world", {"Content-Type": "text/plain"})
        elif p == "/json":
            self.reply(200, (b'{"k":"' + b"v" * 5000 + b'"}'), {"Content-Type": "application/json"})
        elif p == "/chunked":
            self.reply(200, headers={"Content-Type": "text/plain"}, chunked=True)
            for part in (b"alpha", b"", b"beta-" * 100, b"gamma"):
                if part:
                    self.wfile.write(b"%x;e=1\r\n" % len(part) + part + b"\r\n")
                    self.wfile.flush()
                    time.sleep(0.01)
            self.wfile.write(b"0\r\nX-Trailer: yes\r\n\r\n")
        elif p.startswith("/redir/"):
            n = int(p.split("/")[2])
            target = "ok" if n <= 0 else "%d" % (n - 1)
            if n == 2:
                target = "/redir/1"          # path-absolute
            elif n == 1:
                target = "../ok"             # relative with dot-segment
            else:
                target = "%d" % (n - 1)      # relative sibling
            self.reply(302, headers={"Location": target})
        elif p == "/loop":
            self.reply(302, headers={"Location": "/loop"})
        elif p.startswith("/cross?to="):
            self.reply(302, headers={"Location": p[len("/cross?to="):]})
        elif p == "/range" or p == "/norange":
            rng = self.headers.get("Range")
            if rng and p == "/range":
                spec = rng.split("=", 1)[1]
                a, b = spec.split("-")
                a = int(a)
                b = int(b) if b else PATTERN_LEN - 1
                b = min(b, PATTERN_LEN - 1)
                if a >= PATTERN_LEN:
                    self.reply(416, headers={"Content-Range": "bytes */%d" % PATTERN_LEN})
                    return
                self.reply(206, pattern(a, b), {"Content-Range": "bytes %d-%d/%d" % (a, b, PATTERN_LEN),
                                                "Accept-Ranges": "bytes"})
            else:
                hdr = {"Accept-Ranges": "bytes"} if p == "/range" else {}
                self.reply(200, pattern(0, PATTERN_LEN - 1), hdr)
        elif p == "/badrange":
            rng = self.headers.get("Range", "bytes=0-")
            a = int(rng.split("=")[1].split("-")[0])
            self.reply(206, pattern(a + 1, a + 10), {"Content-Range": "bytes %d-%d/%d" % (a + 1, a + 10, PATTERN_LEN)})
        elif p.startswith("/slow"):
            ms = int(p.split("=")[1]) if "=" in p else 3000
            time.sleep(ms / 1000.0)
            self.reply(200, b"slow ok")
        elif p == "/big":
            body = pattern(0, 3 * PATTERN_LEN - 1) if False else (pattern(0, PATTERN_LEN - 1) * 3)
            self.reply(200, body)
        elif p == "/close":
            self.send_response(200)
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"until-close-body")
            self.wfile.flush()
            self.close_connection = True
        elif p.startswith("/status/"):
            code = int(p.split("/")[2])
            h = {"Retry-After": "7"} if code in (429, 503) else {}
            self.reply(code, b"status %d" % code, h)
        elif p == "/conn":
            with Stats.lock:
                self.reply(200, str(Stats.conns).encode())
        elif p == "/echo-hdr":
            text = "".join("%s: %s\n" % (k, v) for k, v in self.headers.items())
            self.reply(200, ("%s\n%s" % (self.command, text)).encode())
        elif p == "/cut":
            self.send_response(200)
            self.send_header("Content-Length", "1000")
            self.end_headers()
            self.wfile.write(b"x" * 100)
            self.wfile.flush()
            self.close_connection = True
        elif p == "/stale":
            # Keep-alive is announced, then the connection is dropped under the client.
            self.reply(200, b"stale")
            self.wfile.flush()
            try:
                self.connection.shutdown(2)
            except Exception:
                pass
            self.close_connection = True
        elif p == "/space%20here":
            self.reply(200, b"escaped ok")
        else:
            self.reply(404, b"nope")


class Srv(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        pass


class TlsH(H):
    def setup(self):
        try:
            self.request.do_handshake()
        except Exception:
            self.request.close()
            raise
        super().setup()


class TlsSrv(Srv):
    def get_request(self):
        sock, addr = self.socket.accept()
        return self.ctx.wrap_socket(sock, server_side=True, do_handshake_on_connect=False), addr

    def handle_error(self, request, client_address):
        pass


def make_tls(name, cert, key, ciphers=None, client_pref=False):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(os.path.join(CERTS, cert), os.path.join(CERTS, key))
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    if ciphers:
        ctx.set_ciphers(ciphers)
    if client_pref:
        ctx.options &= ~ssl.OP_CIPHER_SERVER_PREFERENCE
    srv = TlsSrv(("127.0.0.1", 0), TlsH)
    srv.ctx = ctx
    return name, srv


def main():
    servers = [("plain", Srv(("127.0.0.1", 0), H))]
    servers += [
        make_tls("tls_good", "ec.pem", "ec.key"),
        make_tls("tls_expired", "expired.pem", "expired.key"),
        make_tls("tls_wronghost", "wronghost.pem", "wronghost.key"),
        make_tls("tls_self", "self.pem", "self.key"),
        make_tls("tls_chacha_rsa", "rsa.pem", "rsa.key", "ECDHE-RSA-CHACHA20-POLY1305"),
        make_tls("tls_aes_ec", "ec.pem", "ec.key", "ECDHE-ECDSA-AES128-GCM-SHA256"),
        make_tls("tls_clientpref", "ec.pem", "ec.key",
                 "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-ECDSA-CHACHA20-POLY1305", client_pref=True),
    ]
    with open(PORTFILE + ".tmp", "w") as f:
        for name, s in servers:
            f.write("%s %d\n" % (name, s.server_address[1]))
    os.rename(PORTFILE + ".tmp", PORTFILE)
    for name, s in servers[1:]:
        threading.Thread(target=s.serve_forever, daemon=True).start()
    servers[0][1].serve_forever()


main()
