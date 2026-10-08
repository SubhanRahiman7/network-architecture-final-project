#!/usr/bin/env python3
"""Conformance checker for a PBF/1 *server*, written from SPEC.md: many exchanges on ONE connection.

    python3 tools/stranger_client.py HOST PORT          (the server's document root must hold hello.txt)
"""
import socket, sys
from pbf1 import *

host, port = sys.argv[1], int(sys.argv[2])
sock = socket.create_connection((host, port), timeout=5)
seq = 0
failures = []


def exchange(path, label, expect_status, method=1, headers=None, raw_payload=None, junk_before=None):
    global seq
    seq += 1
    if junk_before:                       # an unknown frame type: the server MUST skip it silently
        sock.sendall(frame(0x7E, 9999, junk_before))
    payload = raw_payload if raw_payload is not None else request_payload(
        path, headers or [("Host", f"{host}:{port}"), ("User-Agent", "stranger/1")], method)
    sock.sendall(frame(REQUEST, seq, payload))
    ftype, rseq, body = read_frame(sock, 16 << 20)
    status, hdrs, data = parse_response(body)
    ok = ftype == RESPONSE and rseq == seq and status == expect_status
    print(f"{'ok  ' if ok else 'FAIL'} {label:<44} -> {status} ({len(data)} body bytes)")
    if not ok:
        failures.append(label)
    return status, hdrs, data


s, h, d = exchange("/hello.txt", "GET existing file", 200)
assert d == b"Hello, packed binary world!\n", d
names = {n for n, _ in h}
assert "Content-Type" in names and dict(h)["Content-Length"] == str(len(d)).encode()
exchange("/hello.txt", "same connection, again", 200)
exchange("/nope.txt", "missing file", 404)
exchange("/", "directory", 404)
exchange("/../secret.txt", "path traversal with ..", 400)
exchange("/a\\b", "backslash", 400)
exchange("/hello.txt", "method 2 (not GET)", 405, method=2)
exchange(None, "malformed payload (3 stray bytes)", 400, raw_payload=b"\x01\xff\xff")
exchange("/hello.txt", "unknown frame type is skipped first", 200, junk_before=b"ignore me")
exchange("/hello.txt", "literal header name the server never heard of", 200,
         headers=[("Host", "x"), ("X-Never-Heard-Of-It", "1")])
literal_host = b"\x01\x00\x0a/hello.txt" + b"\x01" + b"\x00\x04Host" + b"\x00\x01x"      # Tag 0, NameLen 4, "Host"
exchange(None, "table name written as a literal (allowed)", 200, raw_payload=literal_host)
sock.sendall(frame(REQUEST, 77, b"\x01" + bytes(4096)))   # 4097-byte payload: over the server's limit
ftype, rseq, body = read_frame(sock, 16 << 20)
ok = ftype == RESPONSE and rseq == 77 and parse_response(body)[0] == 400
print(f"{'ok  ' if ok else 'FAIL'} {'oversized request frame':<44} -> {parse_response(body)[0]}")
failures += [] if ok else ["oversized"]
exchange("/hello.txt", "connection still usable after every error", 200)

sock.sendall(b"GET / HTTP/1.1\r\n\r\n")                      # not PBF/1 at all: the server must close
sock.settimeout(5)
try:
    closed = sock.recv(1) == b""
except ConnectionError:
    closed = True
print(f"{'ok  ' if closed else 'FAIL'} {'bad magic closes the connection':<44}")
failures += [] if closed else ["bad magic"]
sys.exit(1 if failures else 0)
