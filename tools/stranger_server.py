#!/usr/bin/env python3
"""A PBF/1 file server written from SPEC.md, for testing the C++ client.   python3 tools/stranger_server.py ROOT PORT"""
import os, socket, sys, threading
from pbf1 import *

root, port = os.path.realpath(sys.argv[1]), int(sys.argv[2])


def respond(status, body=b"", extra=()):
    hdrs = [("Content-Type", "text/plain"), ("Content-Length", str(len(body))), ("Server", "stranger/1")] + list(extra)
    return response_payload(status, hdrs, body)


def handle_request(payload):
    try:
        method, path, _ = parse_request(payload)
    except Exception:
        return respond(400, b"malformed\n")
    if method != 1:
        return respond(405, b"method\n")
    if ".." in path.split("/") or "\\" in path:
        return respond(400, b"forbidden\n")
    real = os.path.realpath(os.path.join(root, path.lstrip("/")))
    if not os.path.exists(real):
        return respond(404, b"not found\n")
    if not (real == root or real.startswith(root + os.sep)):
        return respond(400, b"outside the root\n")
    if not os.path.isfile(real):
        return respond(404, b"not found\n")
    return respond(200, open(real, "rb").read())


def serve(conn):
    with conn:
        try:
            while True:
                ftype, seq, payload = read_frame(conn, 4096)
                if ftype != REQUEST:
                    continue                      # unknown frame type: skip silently
                conn.sendall(frame(RESPONSE, seq, handle_request(payload)))
        except (EOFError, ProtocolError, ConnectionError):
            pass


srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen()
print("stranger_server listening on", port, flush=True)
while True:
    c, _ = srv.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
