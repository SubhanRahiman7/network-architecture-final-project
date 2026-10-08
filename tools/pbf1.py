"""A second, independent PBF/1 implementation, written from SPEC.md alone (no C++ code was consulted).

It exists to prove that PBF/1 is a protocol and not the behaviour of one program: the interop test connects this
Python code to the C++ server and client in both directions.
"""
import struct

MAGIC = b"\x7fPBF"
VERSION = 1
REQUEST, RESPONSE = 1, 2
TABLE = ["Host", "User-Agent", "Accept", "Content-Type", "Content-Length",
         "Server", "Last-Modified", "ETag", "Cache-Control", "Accept-Ranges"]


class ProtocolError(Exception):
    pass


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError(f"connection closed after {len(buf)} of {n} bytes")
        buf += chunk
    return buf


def frame(ftype, seq, payload, version=VERSION):
    return MAGIC + bytes([version, ftype]) + struct.pack(">HI", seq, len(payload)) + payload


def read_frame(sock, limit):
    """Returns (type, seq, payload). Unknown types are NOT filtered here; the caller decides."""
    h = recv_exact(sock, 12)
    if h[:4] != MAGIC or h[4] != VERSION:
        raise ProtocolError("bad magic or version")
    seq, length = struct.unpack(">HI", h[6:12])
    if length > limit:
        recv_exact_discard(sock, length)
        raise ProtocolError(f"length {length} over limit {limit}")
    return h[5], seq, recv_exact(sock, length)


def recv_exact_discard(sock, n):
    while n:
        n -= len(sock.recv(min(n, 65536)) or (_ for _ in ()).throw(EOFError("closed while draining")))


def encode_headers(headers):
    out = bytes([len(headers)])
    for name, value in headers:
        value = value.encode() if isinstance(value, str) else value
        idx = next((i + 1 for i, t in enumerate(TABLE) if t.lower() == name.lower()), 0)
        if idx:
            out += bytes([idx])
        else:
            out += b"\x00" + bytes([len(name)]) + name.encode()
        out += struct.pack(">H", len(value)) + value
    return out


def decode_headers(buf, pos):
    count = buf[pos]
    pos += 1
    headers = []
    for _ in range(count):
        tag = buf[pos]
        pos += 1
        if tag == 0:
            nl = buf[pos]
            name = buf[pos + 1:pos + 1 + nl].decode()
            pos += 1 + nl
        elif 1 <= tag <= 10:
            name = TABLE[tag - 1]
        else:
            raise ProtocolError(f"reserved tag {tag}")
        (vl,) = struct.unpack(">H", buf[pos:pos + 2])
        headers.append((name, buf[pos + 2:pos + 2 + vl]))
        pos += 2 + vl
    return headers, pos


def request_payload(path, headers, method=1):
    p = path if isinstance(path, bytes) else path.encode()
    return bytes([method]) + struct.pack(">H", len(p)) + p + encode_headers(headers)


def parse_request(payload):
    method = payload[0]
    (pl,) = struct.unpack(">H", payload[1:3])
    path = payload[3:3 + pl]
    headers, pos = decode_headers(payload, 3 + pl)
    if pos != len(payload) or not path.startswith(b"/") or b"\x00" in path or not 1 <= pl <= 1024:
        raise ProtocolError("malformed request")
    return method, path.decode(), headers


def response_payload(status, headers, body):
    return struct.pack(">H", status) + encode_headers(headers) + body


def parse_response(payload):
    (status,) = struct.unpack(">H", payload[:2])
    headers, pos = decode_headers(payload, 2)
    return status, headers, payload[pos:]
