#!/usr/bin/env python3
"""Generate HEXDUMP.md: one complete PBF/1 request and response, every byte annotated.

The bytes are not typed in. This script starts the real server on port 9000 (document root = a temporary
directory holding hello.txt with a fixed modification time), runs the real client with -v, extracts the two
hexdumps from the client's stderr and annotates them with a small decoder written from SPEC.md only.

    python3 tools/annotate.py            # rewrites HEXDUMP.md
    python3 tools/annotate.py --check    # exits 1 if HEXDUMP.md is out of date
"""
import os, re, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 9000
MTIME = 1760000000          # 2025-10-09 08:53:20 UTC, fixed so that the example never changes
CONTENT = b"Hello, packed binary world!\n"
TABLE = ["Host", "User-Agent", "Accept", "Content-Type", "Content-Length",
         "Server", "Last-Modified", "ETag", "Cache-Control", "Accept-Ranges"]


def hexs(b):
    return " ".join(f"{x:02x}" for x in b)


def printable(b):
    s = b.decode("latin-1")
    return "".join(c if 0x20 <= ord(c) < 0x7f else "." for c in s)


class Walker:
    """Cuts a frame into (offset, bytes, meaning) rows."""

    def __init__(self, data):
        self.d, self.i, self.rows = data, 0, []

    def take(self, n, text):
        self.rows.append((self.i, self.d[self.i:self.i + n], text))
        self.i += n
        return self.d[self.i - n:self.i]

    def num(self, n, text):
        v = int.from_bytes(self.take(n, text), "big")
        self.rows[-1] = (self.rows[-1][0], self.rows[-1][1], text.replace("%", str(v)))
        return v

    def frame_header(self):
        self.take(4, 'Magic: 0x7F "PBF"')
        self.num(1, "Version = %")
        t = self.num(1, "Type = % (%s)")
        self.rows[-1] = (self.rows[-1][0], self.rows[-1][1], f"Type = {t} ({'REQUEST' if t == 1 else 'RESPONSE'})")
        self.num(2, "Seq = %")
        return self.num(4, "Length = % (payload bytes that follow)")

    def headers(self):
        n = self.num(1, "Header count = %")
        for k in range(1, n + 1):
            tag = self.num(1, "Field %d: Tag = %%" % k)
            if tag == 0:
                self.rows[-1] = (self.rows[-1][0], self.rows[-1][1], f"Field {k}: Tag = 0 (literal name follows)")
                nl = self.num(1, "NameLen = %")
                name = self.take(nl, "Name").decode()
                self.rows[-1] = (self.rows[-1][0], self.rows[-1][1], f'Name "{name}" (spelled out)')
            else:
                name = TABLE[tag - 1]
                self.rows[-1] = (self.rows[-1][0], self.rows[-1][1], f'Field {k}: Tag = {tag} (numbered: "{name}")')
            vl = self.num(2, "ValueLen = %")
            val = self.take(vl, "Value").decode()
            self.rows[-1] = (self.rows[-1][0], self.rows[-1][1], f'Value "{val}"')


def annotate(frame):
    w = Walker(frame)
    length = w.frame_header()
    end = w.i + length
    if frame[5] == 1:
        w.num(1, "Method = % (GET)")
        pl = w.num(2, "PathLen = %")
        path = w.take(pl, "Path").decode()
        w.rows[-1] = (w.rows[-1][0], w.rows[-1][1], f'Path "{path}"')
        w.headers()
    else:
        s = w.num(2, "Status = %")
        w.headers()
        body = w.take(end - w.i, "Body")
        w.rows[-1] = (w.rows[-1][0], body, f'Body: all {len(body)} remaining payload bytes = "{printable(body).replace(chr(46) * 0, "")}"'.replace('."', '\\n"') if body.endswith(b"\n") else "Body")
    assert w.i == len(frame) == 12 + length, "walker did not consume the frame exactly"
    return w.rows


def render(rows):
    out = ["```", f"{'offset':<7} {'bytes':<47}  meaning"]
    for off, b, text in rows:
        chunks = [b[x:x + 16] for x in range(0, len(b), 16)] or [b""]
        for n, c in enumerate(chunks):
            out.append(f"{off + n * 16:04x}    {hexs(c):<47}  {text if n == 0 else '(continued)'}")
    out.append("```")
    return "\n".join(out)


def capture():
    binary = os.path.join(ROOT, "pbf")
    server = os.path.join(ROOT, "pbfd")
    for p in (binary, server):
        if not os.path.exists(p):
            sys.exit(f"{p} missing: run `make` first")
    with tempfile.TemporaryDirectory() as docroot:
        f = os.path.join(docroot, "hello.txt")
        open(f, "wb").write(CONTENT)
        os.utime(f, (MTIME, MTIME))
        srv = subprocess.Popen([server, docroot, str(PORT)], stderr=subprocess.DEVNULL)
        try:
            time.sleep(0.6)
            r = subprocess.run([binary, "-v", "-H", "X-Trace: demo", f"localhost:{PORT}/hello.txt"],
                               capture_output=True)
        finally:
            srv.terminate()
            srv.wait()
    if r.returncode != 0:
        sys.exit("client failed:\n" + r.stderr.decode())
    frames, cur = [], None
    for line in r.stderr.decode().splitlines():
        if line[:1] in "<>" and "seq=" in line:
            cur = bytearray()
            frames.append(cur)
        m = re.match(r"^[0-9a-f]{8}  ((?:[0-9a-f]{2} ?){1,16}(?: (?:[0-9a-f]{2} ?)+)?)\s+\|", line)
        if m and cur is not None:
            cur.extend(bytes.fromhex(m.group(1).replace(" ", "")))
    assert len(frames) == 2, "expected one request and one response"
    return bytes(frames[0]), bytes(frames[1])


def build():
    req, resp = capture()
    rq, rs = annotate(req), annotate(resp)
    return f"""# One complete PBF/1 exchange, byte by byte

*Generated by `tools/annotate.py` from the bytes that the real `pbf` client and `pbfd` server exchanged - do not edit by hand.
Regenerate with `make hexdump`; `make hexdump-check` fails if this file is out of date.*

Command: `pbf -v -H "X-Trace: demo" localhost:9000/hello.txt` against `pbfd <root> 9000`, where `hello.txt` holds the
{len(CONTENT)} bytes `Hello, packed binary world!\\n` and was last modified at Unix time {MTIME}.
The `-H` option adds a header that is **not** in the table, so this one exchange shows both header mechanisms:
numbered names (1 byte) and a length-prefixed literal.

## REQUEST - {len(req)} bytes on the wire (12 header + {len(req) - 12} payload)

{render(rq)}

The client spells **0** of its three standard header names: `Host`, `User-Agent` and `Accept` are tags 1, 2 and 3. Only
`X-Trace` is written out (tag 0, then NameLen 7 and its 7 letters).

## RESPONSE - {len(resp)} bytes on the wire (12 header + {len(resp) - 12} payload)

{render(rs)}

Seq is `1`, copied from the request. There is no body length anywhere: the body is whatever is left of the payload after
the last header, i.e. `Length - 2 (Status) - (header list)` = {len(CONTENT)} bytes. `Content-Length: {len(CONTENT)}` is only a hint.
`Last-Modified` is `{MTIME}` and the `ETag` is `"{len(CONTENT):x}-{MTIME:x}"` (size and mtime in hex).

## Size

| | with numbered names | if every name were spelled out | saved |
|---|---:|---:|---:|
| REQUEST payload | {len(req) - 12} | {len(req) - 12 + sum(len(TABLE[t - 1]) for t in (1, 2, 3))} | {sum(len(TABLE[t - 1]) for t in (1, 2, 3))} |
| RESPONSE payload | {len(resp) - 12} | {len(resp) - 12 + sum(len(TABLE[t - 1]) for t in range(4, 11))} | {sum(len(TABLE[t - 1]) for t in range(4, 11))} |

(A spelled-out field needs a 1-byte NameLen plus the name instead of a 1-byte Tag, so a table hit saves exactly the name's length.)
"""


if __name__ == "__main__":
    text = build()
    path = os.path.join(ROOT, "HEXDUMP.md")
    if "--check" in sys.argv:
        sys.exit(0 if os.path.exists(path) and open(path).read() == text else 1)
    open(path, "w").write(text)
    print("wrote", path)
