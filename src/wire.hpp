// PBF/1 ("Packed Binary Fetch") wire format: frames, REQUEST / RESPONSE
// payloads, the hexdump used by -v. SPEC.md is the authoritative description.
// Header fields use a static table of ten names plus length-prefixed literals.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "net.hpp"

namespace pbf {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint8_t VERSION = 1;
constexpr std::uint8_t TYPE_REQUEST = 0x01;
constexpr std::uint8_t TYPE_RESPONSE = 0x02;
constexpr std::uint8_t METHOD_GET = 1;
constexpr std::size_t HEADER_SIZE = 12;

constexpr std::size_t MAX_PATH_LEN = 1024;
constexpr std::size_t MAX_HEADERS = 16;
constexpr std::size_t MAX_HEADER_NAME = 64;  // longest literal name
constexpr std::size_t MAX_HEADER_VALUE = 1024;
// A REQUEST is a path and a few headers; it never needs more than this.
constexpr std::size_t MAX_REQUEST_PAYLOAD = 4096;
// Bounds any frame, so it is also the largest file a server can send.
constexpr std::size_t MAX_PAYLOAD = 16u << 20;

// 0x7F never starts an HTTP/1 request line, so a text-protocol peer is
// detected on the very first byte; "PBF" makes frames easy to spot in a dump.
constexpr std::uint8_t MAGIC[4] = {0x7F, 'P', 'B', 'F'};

// The ten header names the reference programs actually send. A field whose name
// is in this table is written as its 1-based number (one byte) instead of the
// spelled-out name; every other name is a length-prefixed literal. Tag 0 means
// "literal follows". Tags 11..255 are reserved for a later version.
constexpr std::size_t TABLE_SIZE = 10;
constexpr const char* HEADER_TABLE[TABLE_SIZE] = {
    "Host",          // 1  request
    "User-Agent",    // 2  request
    "Accept",        // 3  request
    "Content-Type",  // 4  response
    "Content-Length",// 5  response
    "Server",        // 6  response
    "Last-Modified", // 7  response (200 only)
    "ETag",          // 8  response (200 only)
    "Cache-Control", // 9  response (200 only)
    "Accept-Ranges", // 10 response (200 only)
};
constexpr std::uint8_t TAG_LITERAL = 0;

inline bool iequals(const std::string& a, const char* b) {
    std::size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

// Returns the 1-based table number of `name`, or 0 when it is not in the table.
inline std::uint8_t table_index(const std::string& name) {
    for (std::size_t i = 0; i < TABLE_SIZE; ++i)
        if (iequals(name, HEADER_TABLE[i])) return static_cast<std::uint8_t>(i + 1);
    return 0;
}

inline void put16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
inline void put32(Bytes& b, std::uint32_t v) {
    put16(b, static_cast<std::uint16_t>(v >> 16));
    put16(b, static_cast<std::uint16_t>(v));
}
inline std::uint16_t get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
inline std::uint32_t get32(const std::uint8_t* p) { return std::uint32_t(get16(p)) << 16 | get16(p + 2); }

// ---- frames -----------------------------------------------------------------

struct Frame {
    std::uint8_t type = 0;
    std::uint16_t seq = 0;  // chosen by the client, echoed by the server
    Bytes payload;

    // The exact bytes of this frame on the wire.
    Bytes marshal() const {
        Bytes b(MAGIC, MAGIC + 4);
        b.push_back(VERSION);
        b.push_back(type);
        put16(b, seq);
        put32(b, static_cast<std::uint32_t>(payload.size()));
        b.insert(b.end(), payload.begin(), payload.end());
        return b;
    }
};

// decode_header checks magic and version of a 12-byte header and extracts the
// rest. Returns false when the stream is out of sync.
inline bool decode_header(const std::uint8_t* h, Frame& f, std::uint32_t& len) {
    if (std::memcmp(h, MAGIC, 4) != 0 || h[4] != VERSION) return false;
    f.type = h[5];
    f.seq = get16(h + 6);
    len = get32(h + 8);
    return true;
}

enum class Read {
    ok,
    closed,     // the peer closed between frames: a clean end
    truncated,  // the connection ended (or timed out) inside a frame
    desync,     // bad magic or version: the frame boundary is lost, close
    too_large,  // declared payload above the limit; it was drained, stream still in sync
};

struct ReadResult {
    Read status;
    Frame frame;  // on too_large only type and seq are set
    std::string detail;
};

// read_frame reads one frame from the socket. Every length is checked before
// memory is allocated for it.
inline ReadResult read_frame(net::socket_t s, std::size_t max_payload) {
    std::uint8_t h[HEADER_SIZE];
    std::size_t got = net::recv_exact(s, h, HEADER_SIZE);
    if (got == 0) return {Read::closed, {}, "peer closed the connection"};
    if (got < HEADER_SIZE) return {Read::truncated, {}, "connection ended inside a frame header"};
    Frame f;
    std::uint32_t len = 0;
    if (!decode_header(h, f, len)) return {Read::desync, {}, "bad magic or version: stream out of sync"};
    if (len > max_payload) {
        if (!net::discard(s, len)) return {Read::truncated, f, "connection ended inside an oversized frame"};
        return {Read::too_large, f,
                "frame too large: " + std::to_string(len) + " bytes, limit " + std::to_string(max_payload)};
    }
    f.payload.resize(len);
    if (len > 0 && net::recv_exact(s, f.payload.data(), len) != len)
        return {Read::truncated, f, "connection ended inside a frame payload"};
    return {Read::ok, f, ""};
}

inline bool write_frame(net::socket_t s, const Frame& f) {
    Bytes b = f.marshal();
    return net::send_all(s, b.data(), b.size());
}

// ---- payloads ---------------------------------------------------------------

struct Header {
    std::string name, value;
};

struct Malformed : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline void append_headers(Bytes& b, const std::vector<Header>& hs) {
    if (hs.size() > MAX_HEADERS) throw std::length_error("more than " + std::to_string(MAX_HEADERS) + " headers");
    b.push_back(static_cast<std::uint8_t>(hs.size()));
    for (const Header& h : hs) {
        if (h.name.empty() || h.name.size() > MAX_HEADER_NAME || h.value.size() > MAX_HEADER_VALUE)
            throw std::length_error("header '" + h.name + "': name or value length out of range");
        if (std::uint8_t idx = table_index(h.name)) {
            b.push_back(idx);  // numbered: one byte stands for the whole name
        } else {
            b.push_back(TAG_LITERAL);  // literal: length-prefixed name
            b.push_back(static_cast<std::uint8_t>(h.name.size()));
            b.insert(b.end(), h.name.begin(), h.name.end());
        }
        put16(b, static_cast<std::uint16_t>(h.value.size()));
        b.insert(b.end(), h.value.begin(), h.value.end());
    }
}

struct Request {
    std::uint8_t method = METHOD_GET;
    std::string path;
    std::vector<Header> headers;

    // Throws std::length_error when a field exceeds the protocol limits.
    Bytes marshal() const {
        if (path.empty() || path.size() > MAX_PATH_LEN)
            throw std::length_error("path length " + std::to_string(path.size()) + " outside 1.." + std::to_string(MAX_PATH_LEN));
        Bytes b{method};
        put16(b, static_cast<std::uint16_t>(path.size()));
        b.insert(b.end(), path.begin(), path.end());
        append_headers(b, headers);
        return b;
    }
};

struct Response {
    std::uint16_t status = 200;
    std::vector<Header> headers;
    Bytes body;  // everything after the headers; its length is implied by the frame length

    Bytes marshal() const {
        Bytes b;
        put16(b, status);
        append_headers(b, headers);
        b.insert(b.end(), body.begin(), body.end());
        return b;
    }
};

// Cursor walks a payload and throws Malformed the moment a field would run
// past the end, so no decoder ever reads outside its buffer.
struct Cursor {
    const std::uint8_t* p;
    std::size_t left;

    void need(std::size_t k) const {
        if (k > left)
            throw Malformed("a field of " + std::to_string(k) + " bytes runs past the end of the payload");
    }
    std::uint8_t u8() {
        need(1);
        --left;
        return *p++;
    }
    std::uint16_t u16() {
        need(2);
        std::uint16_t v = get16(p);
        p += 2;
        left -= 2;
        return v;
    }
    std::string str(std::size_t k) {
        need(k);
        std::string s(reinterpret_cast<const char*>(p), k);
        p += k;
        left -= k;
        return s;
    }
};

inline std::vector<Header> parse_headers(Cursor& c) {
    std::size_t n = c.u8();
    if (n > MAX_HEADERS) throw Malformed(std::to_string(n) + " headers, limit " + std::to_string(MAX_HEADERS));
    std::vector<Header> hs;
    hs.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t tag = c.u8();
        std::string name;
        if (tag == TAG_LITERAL) {
            std::size_t nl = c.u8();
            if (nl == 0 || nl > MAX_HEADER_NAME)
                throw Malformed("header name length " + std::to_string(nl) + " outside 1.." + std::to_string(MAX_HEADER_NAME));
            name = c.str(nl);
        } else if (tag <= TABLE_SIZE) {
            name = HEADER_TABLE[tag - 1];
        } else {
            // The length of whatever follows is unknown, so the rest cannot be parsed.
            throw Malformed("reserved header tag " + std::to_string(tag));
        }
        std::size_t vl = c.u16();
        if (vl > MAX_HEADER_VALUE)
            throw Malformed("header value length " + std::to_string(vl) + ", limit " + std::to_string(MAX_HEADER_VALUE));
        hs.push_back({name, c.str(vl)});
    }
    return hs;
}

// parse_request decodes a REQUEST payload or throws Malformed.
inline Request parse_request(const Bytes& payload) {
    Cursor c{payload.data(), payload.size()};
    Request q;
    q.method = c.u8();
    std::size_t n = c.u16();
    if (n == 0 || n > MAX_PATH_LEN)
        throw Malformed("path length " + std::to_string(n) + " outside 1.." + std::to_string(MAX_PATH_LEN));
    q.path = c.str(n);
    q.headers = parse_headers(c);
    if (c.left != 0) throw Malformed(std::to_string(c.left) + " trailing bytes after the last header");
    if (q.path[0] != '/' || q.path.find('\0') != std::string::npos)
        throw Malformed("path must start with '/' and contain no NUL");
    return q;
}

// parse_response decodes a RESPONSE payload or throws Malformed.
inline Response parse_response(const Bytes& payload) {
    Cursor c{payload.data(), payload.size()};
    Response r;
    r.status = c.u16();
    if (r.status < 100 || r.status > 599) throw Malformed("status " + std::to_string(r.status) + " outside 100..599");
    r.headers = parse_headers(c);
    r.body.assign(c.p, c.p + c.left);
    return r;
}

// ---- dump (-v) --------------------------------------------------------------

// Capped so a large body does not flood the terminal.
constexpr std::size_t DUMP_LIMIT = 512;

inline void hexdump(std::ostream& o, const std::uint8_t* p, std::size_t n) {
    for (std::size_t off = 0; off < n; off += 16) {
        o << std::hex << std::setfill('0') << std::setw(8) << off << " ";
        for (std::size_t i = 0; i < 16; ++i) {
            if (i == 8) o << ' ';
            if (off + i < n)
                o << ' ' << std::setw(2) << unsigned(p[off + i]);
            else
                o << "   ";
        }
        o << "  |";
        for (std::size_t i = 0; i < 16 && off + i < n; ++i) {
            unsigned char ch = p[off + i];
            o << (ch >= 0x20 && ch < 0x7f ? char(ch) : '.');
        }
        o << "|\n" << std::dec;
    }
}

inline std::string header_list(const std::vector<Header>& hs) {
    std::string s;
    for (const Header& h : hs) s += " " + h.name + "=\"" + h.value + "\"";
    return s;
}

// describe runs the real decoder over a complete frame and summarises what it
// finds, so the -v output shows what the implementation makes of the bytes.
inline std::string describe(const Bytes& b) {
    Frame f;
    std::uint32_t len = 0;
    if (b.size() < HEADER_SIZE || !decode_header(b.data(), f, len) || len != b.size() - HEADER_SIZE)
        return "undecodable frame";
    f.payload.assign(b.begin() + HEADER_SIZE, b.end());
    std::string s = "seq=" + std::to_string(f.seq) + " payload=" + std::to_string(len) + " bytes (" +
                    std::to_string(b.size()) + " on the wire)";
    try {
        if (f.type == TYPE_REQUEST) {
            Request q = parse_request(f.payload);
            return "REQUEST " + s + ": method=" + std::to_string(q.method) + " path=" + q.path + header_list(q.headers);
        }
        if (f.type == TYPE_RESPONSE) {
            Response r = parse_response(f.payload);
            return "RESPONSE " + s + ": status=" + std::to_string(r.status) + header_list(r.headers) +
                   " body=" + std::to_string(r.body.size()) + " bytes";
        }
    } catch (const Malformed& e) {
        return (f.type == TYPE_REQUEST ? "REQUEST " : "RESPONSE ") + s + ": malformed: " + e.what();
    }
    std::ostringstream t;
    t << "UNKNOWN type=0x" << std::hex << std::setw(2) << std::setfill('0') << unsigned(f.type) << " " << s << ": skipped";
    return t.str();
}

// dump writes the summary line and a hexdump. prefix: ">" sent, "<" received.
inline void dump(std::ostream& o, const char* prefix, const Bytes& b) {
    o << prefix << " " << describe(b) << "\n";
    if (b.size() <= DUMP_LIMIT) {
        hexdump(o, b.data(), b.size());
        return;
    }
    hexdump(o, b.data(), DUMP_LIMIT);
    o << prefix << " ... " << (b.size() - DUMP_LIMIT) << " more bytes not shown\n";
}

}  // namespace pbf
