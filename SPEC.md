# PBF/1 - Packed Binary Fetch, version 1

MUST / MUST NOT / SHOULD / MAY are used as in RFC 2119. This document is normative and complete: an implementation written
from it alone interoperates with the reference server (`pbfd`) and client (`pbf`). One annotated exchange is in
[HEXDUMP.md](HEXDUMP.md); the reasoning behind the choices is in [docs/DESIGN.md](docs/DESIGN.md).

## 1. Model

PBF/1 fetches files over **one TCP connection**. The client sends a REQUEST frame, the server answers with one RESPONSE
frame, repeat. A client MUST NOT open a second connection for more requests; a server MUST NOT close after answering.
Exchanges are strictly sequential (no request before the previous response), so responses arrive in request order.
The client ends the connection by closing it; a server MAY drop a connection idle for a long time (reference: 30 s).

All integers are **unsigned big-endian**. A **string** is a length followed by that many bytes (no terminator, any byte
value). TCP is a byte stream: receivers MUST assemble frames from the length field, never assume one read = one frame,
and senders MUST cope with short writes.

## 2. Frame

Every frame is a **12-byte header** and then `Length` payload bytes.

| Offset | Size | Field   | Meaning |
|-------:|-----:|---------|---------|
| 0      | 4    | Magic   | `7F 50 42 46` (`0x7F`, "PBF") |
| 4      | 1    | Version | `1` |
| 5      | 1    | Type    | `01` REQUEST, `02` RESPONSE; anything else is unknown |
| 6      | 2    | Seq     | chosen by the client per request, copied unchanged into the response |
| 8      | 4    | Length  | payload bytes that follow (0 is allowed) |

**Why these widths.** *Magic* 4 bytes: `0x7F` starts neither an HTTP/1 request (a letter) nor an HTTP/2 preface (`P`), so a
wrong peer is spotted on byte one, and `PBF` is easy to see in a dump. *Version* 1 byte: 255 revisions is plenty, and it
sits in the first five bytes so a receiver knows early whether it understands the rest. *Type* 1 byte: two types now, 254
free. *Seq* 2 bytes: with strictly sequential exchanges it only has to tell neighbours apart; wrap-around is harmless.
*Length* 4 bytes: HTTP/2's 24 bits fix a 16 MiB ceiling in the format; 32 bits make the ceiling a policy (section 7) that
a later version can raise. There is no flags byte: version and type already carry every distinction PBF/1 needs.

**Receiver procedure**, in this order, before looking at any payload byte:
1. Magic is not `7F 50 42 46` -> the stream is not PBF/1 or lost sync: close.
2. Version is not `1` -> the next frame cannot be located: close.
3. Length above the receiver's limit (section 7) -> do not allocate; read and discard exactly Length bytes; reject (section 8).
4. **Type unknown -> read and discard exactly Length bytes and carry on, silently, connection open.** This is how later
   versions add frame types. A server treats a RESPONSE, and a client a REQUEST, as unknown.

Then read the whole payload and decode it by Type.

## 3. Header list

Both payloads contain a header list: `Count u8` (<= 16), then Count fields. Each field starts with a one-byte **Tag**:

| Tag | Form | Layout after the tag |
|----:|------|----------------------|
| 1..10 | **numbered** name | `ValueLen u16` `Value` |
| 0 | **literal** name | `NameLen u8` (1..64) `Name` `ValueLen u16` (<= 1024) `Value` |
| 11..255 | reserved | the receiver cannot parse past it: the payload is malformed |

The ten numbered names are exactly the ones the reference programs send:

| 1 Host | 2 User-Agent | 3 Accept | 4 Content-Type | 5 Content-Length |
|---|---|---|---|---|
| **6 Server** | **7 Last-Modified** | **8 ETag** | **9 Cache-Control** | **10 Accept-Ranges** |

A numbered name costs 1 byte instead of 1 + its length. A sender MAY write a table name as a literal; names are compared
case-insensitively; a receiver MUST ignore names it does not understand and MUST accept repeated names (first wins).
Values are opaque bytes. `Content-Length` is only a hint: the frame Length is authoritative. `Last-Modified` is the
file's modification time as decimal Unix seconds; `ETag` is `"<size hex>-<mtime hex>"`.

## 4. REQUEST payload (Type 01)

`Method u8` · `PathLen u16` (1..1024) · `Path` · header list. Method `1` = GET; any other value is well-formed and a
server answers `405`. Path MUST start with `/`, MUST NOT contain `00`, and is raw bytes (no percent-encoding). The payload
ends exactly after the last header field; a trailing byte makes it malformed. A REQUEST has no body.

## 5. RESPONSE payload (Type 02)

`Status u16` (100..599) · header list · **Body = every remaining payload byte** (possibly none). There is deliberately no
body length: a second length could disagree with the frame Length. Statuses used: **200** file sent, **400** malformed or
forbidden request, **404** no such regular file, **405** method not GET, **500** cannot be served (unreadable, or larger than
a frame). A client MUST accept 100..599 and judge unknown codes by their first digit. Error bodies are short human text.

## 6. Paths and the document root

A server MUST never send a byte from outside its document root. The reference rules, in order: a path segment equal to
`..` -> 400 (the raw path is checked, not normalised); a backslash anywhere -> 400; resolve the path under the root with
symlinks followed: missing -> 404, outside the root -> 400; not a regular file (e.g. a directory, so `/` too) -> 404;
unreadable or over 16 MiB -> 500.

## 7. Limits

REQUEST payload <= **4096** bytes (server checks Length first). RESPONSE payload <= **16 MiB** (client checks).
PathLen 1..1024, Count <= 16, literal NameLen 1..64, ValueLen <= 1024. Every length MUST be checked against its limit **and**
against the bytes left in the payload *before* anything is allocated or copied; a length that overruns a complete frame is
malformed, not a reason to wait for more bytes.

## 8. Errors: can the receiver still find the next frame?

| Condition | Frame boundary | Server | Client |
|-----------|----------------|--------|--------|
| bad Magic or Version, or connection cut inside a frame | lost | close | report, close |
| Length over limit | kept (payload drained) | `400`, keep open | report, close |
| unknown Type | kept | skip, send nothing | skip, keep waiting |
| malformed payload / forbidden path | kept | `400` (or `405`), keep open | report, close, use nothing from it |
| response Seq differs from the request's | - | - | report, close |
| status not 2xx | - | - | deliver the body, exit non-zero |

A `400`/`405` response echoes the Seq of the frame it answers.

## 9. Versioning

Any change to the frame header, to a payload layout or to the meaning of a field needs a new Version value; a version-1
receiver closes on any other version (no negotiation). New frame types, new header names and new status codes need no new
version: old receivers skip, ignore, or read them by first digit.
