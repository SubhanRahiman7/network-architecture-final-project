// Tests for PBF/1: codec unit tests, then end-to-end tests that drive the real
// server over TCP, both with hand-built frames and with the real client code.
// No framework: CHECK() counts failures, main() returns non-zero if any.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#include "client.hpp"
#include "server.hpp"

using namespace pbf;
using namespace std::chrono_literals;

static int failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ++failures;                                                               \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
        }                                                                             \
    } while (0)

static Bytes bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

// ---- fixture ----------------------------------------------------------------

static fs::path root;
static std::uint16_t port;
static std::mutex log_mu;
static std::vector<std::string> logs;

static std::string hostport() { return "127.0.0.1:" + std::to_string(port); }

static int count_logs(const std::string& needle) {
    std::lock_guard<std::mutex> lk(log_mu);
    int n = 0;
    for (const std::string& l : logs)
        if (l.find(needle) != std::string::npos) ++n;
    return n;
}

static void write_file(const fs::path& p, const Bytes& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

static Bytes random_bytes(std::size_t n) {
    std::mt19937 rng(42);
    Bytes b(n);
    for (auto& x : b) x = static_cast<std::uint8_t>(rng());
    return b;
}

static const std::string hello = "Hello, packed binary world!\n";
static const Bytes blob = random_bytes(1 << 20);
static std::string long_path;  // exactly MAX_PATH_LEN bytes
static bool long_path_exists = false;

static net::socket_t connect() {
    std::string err;
    net::socket_t s = net::connect_to("127.0.0.1", std::to_string(port), err);
    if (s == net::invalid_socket) {
        std::cerr << "cannot connect: " << err << "\n";
        std::exit(1);
    }
    return s;
}

static Bytes get_payload(const std::string& path, std::uint8_t method = METHOD_GET) {
    Request q;
    q.method = method;
    q.path = path;
    q.headers = {{"Host", hostport()}};
    return q.marshal();
}

static void send_raw(net::socket_t s, const Bytes& b) { CHECK(net::send_all(s, b.data(), b.size())); }

// Sends raw payload bytes as a REQUEST frame and returns the parsed response status.
static Response raw_request(net::socket_t s, const Bytes& payload, std::uint16_t seq = 1) {
    send_raw(s, Frame{TYPE_REQUEST, seq, payload}.marshal());
    ReadResult r = read_frame(s, MAX_PAYLOAD);
    CHECK(r.status == Read::ok);
    CHECK(r.frame.type == TYPE_RESPONSE);
    CHECK(r.frame.seq == seq);
    return r.status == Read::ok ? parse_response(r.frame.payload) : Response{};
}

// ---- unit tests: codec ------------------------------------------------------

static void test_roundtrip() {
    Request q;
    q.path = "/a/b.txt";
    q.headers = {{"Host", "h:1"}, {"X", ""}};
    Request q2 = parse_request(q.marshal());
    CHECK(q2.method == METHOD_GET && q2.path == q.path && q2.headers.size() == 2);
    CHECK(q2.headers[0].name == "Host" && q2.headers[0].value == "h:1" && q2.headers[1].value.empty());

    Response r;
    r.status = 404;
    r.headers = {{"Content-Type", "x/y"}};
    r.body = Bytes{0, 1, 2, 0, 255};  // NUL bytes inside a body are ordinary data
    Response r2 = parse_response(r.marshal());
    CHECK(r2.status == 404 && r2.headers.size() == 1 && r2.body == r.body);
    Response empty;
    CHECK(parse_response(empty.marshal()).body.empty());

    Frame f{TYPE_REQUEST, 0xBEEF, q.marshal()};
    Bytes w = f.marshal();
    CHECK(w.size() == HEADER_SIZE + f.payload.size());
    CHECK(w[0] == 0x7F && w[1] == 'P' && w[2] == 'B' && w[3] == 'F' && w[4] == 1 && w[5] == 1 && w[6] == 0xBE && w[7] == 0xEF);
    Frame g;
    std::uint32_t len = 0;
    CHECK(decode_header(w.data(), g, len) && g.seq == 0xBEEF && len == f.payload.size());
}

static void test_header_table() {
    // A table name costs one byte; any other name is a length-prefixed literal.
    Bytes numbered, literal;
    append_headers(numbered, {{"Host", "a"}});
    append_headers(literal, {{"X-Trace", "a"}});
    CHECK((numbered == Bytes{1, 1, 0, 1, 'a'}));
    CHECK((literal == Bytes{1, 0, 7, 'X', '-', 'T', 'r', 'a', 'c', 'e', 0, 1, 'a'}));

    // Every table entry round-trips through its number, in any letter case.
    for (std::size_t i = 0; i < TABLE_SIZE; ++i) {
        CHECK(table_index(HEADER_TABLE[i]) == i + 1);
        Bytes b;
        append_headers(b, {{HEADER_TABLE[i], "v"}});
        CHECK(b.size() == 5 && b[1] == i + 1);
        Cursor c{b.data(), b.size()};
        auto hs = parse_headers(c);
        CHECK(hs.size() == 1 && hs[0].name == HEADER_TABLE[i] && hs[0].value == "v" && c.left == 0);
    }
    CHECK(table_index("host") == 1 && table_index("USER-AGENT") == 2 && table_index("X-Trace") == 0);

    // A literal spelled like a table name is legal on the wire and decodes to the same name.
    Bytes lit{1, 0, 4, 'H', 'o', 's', 't', 0, 0};
    Cursor c{lit.data(), lit.size()};
    CHECK(parse_headers(c)[0].name == "Host");
}

static Bytes from_hex(const std::string& text) {
    Bytes b;
    std::istringstream in(text);
    std::string tok;
    while (in >> tok) b.push_back(static_cast<std::uint8_t>(std::stoul(tok, nullptr, 16)));
    return b;
}

static const Header* find_header(const Response& r, const char* name) {
    for (const Header& h : r.headers)
        if (iequals(h.name, name)) return &h;
    return nullptr;
}

static void test_file_headers() {
    net::socket_t c = connect();
    Response r = raw_request(c, get_payload("/hello.txt"));
    CHECK(r.status == 200);
    std::error_code ec;
    std::int64_t mtime = mtime_seconds(root / "hello.txt", ec);
    const Header* lm = find_header(r, "Last-Modified");
    const Header* et = find_header(r, "ETag");
    CHECK(lm && lm->value == std::to_string(mtime));
    CHECK(et && et->value == "\"" + hex(hello.size()) + "-" + hex(static_cast<std::uint64_t>(mtime)) + "\"");
    CHECK(find_header(r, "Cache-Control") && find_header(r, "Accept-Ranges"));
    CHECK(find_header(r, "Content-Length") && find_header(r, "Content-Length")->value == std::to_string(hello.size()));
    // An error response carries no file metadata.
    Response e = raw_request(c, get_payload("/missing"), 2);
    CHECK(e.status == 404 && !find_header(e, "ETag") && !find_header(e, "Last-Modified"));
    net::close_socket(c);
}

static void test_concurrent_connections() {
    // A connection that never sends anything must not block another client.
    net::socket_t idle = connect();
    net::socket_t c = connect();
    CHECK(raw_request(c, get_payload("/hello.txt")).status == 200);
    net::close_socket(c);

    // Several clients at once, each with its own run of requests on one connection.
    std::atomic<int> good{0};
    std::vector<std::thread> clients;
    for (int t = 0; t < 8; ++t)
        clients.emplace_back([&] {
            net::socket_t s = connect();
            for (std::uint16_t i = 1; i <= 20; ++i) {
                Request q;
                q.path = "/hello.txt";
                send_raw(s, Frame{TYPE_REQUEST, i, q.marshal()}.marshal());
                ReadResult r = read_frame(s, MAX_PAYLOAD);
                if (r.status == Read::ok && r.frame.seq == i && parse_response(r.frame.payload).status == 200) ++good;
            }
            net::close_socket(s);
        });
    for (auto& t : clients) t.join();
    CHECK(good == 8 * 20);
    net::close_socket(idle);
}

// Sets a file's mtime to FIXED_MTIME + 0.5 s (the half second keeps clock-conversion jitter from crossing a second).
static constexpr std::int64_t FIXED_MTIME = 1760000000;
static void set_mtime(const fs::path& p, std::int64_t unix_seconds) {
    auto target = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(unix_seconds)) + 500ms;
    fs::last_write_time(p, fs::file_time_type::clock::now() + (target - std::chrono::system_clock::now()));
}

static void test_hexdump_example() {
    // The response in HEXDUMP.md (generated by tools/annotate.py from the real programs), byte for byte,
    // produced here by serve_file() from a file with the same name, content and modification time.
    fs::path dir = root / "example";
    write_file(dir / "hello.txt", bytes(hello));
    set_mtime(dir / "hello.txt", FIXED_MTIME);
    Bytes resp = Frame{TYPE_RESPONSE, 1, serve_file(resolve_root(dir.string()), "/hello.txt").marshal()}.marshal();
    CHECK(resp == from_hex("7f 50 42 46 01 02 00 01 00 00 00 78 00 c8 07 04 00 19 74 65 78 74 2f 70 6c 61 69 6e 3b 20 63 68 61 72 "
                           "73 65 74 3d 75 74 66 2d 38 05 00 02 32 38 06 00 06 70 62 66 64 2f 31 07 00 0a 31 37 36 30 30 30 30 30 "
                           "30 30 08 00 0d 22 31 63 2d 36 38 65 37 37 38 30 30 22 09 00 08 6e 6f 2d 63 61 63 68 65 0a 00 04 6e 6f "
                           "6e 65 48 65 6c 6c 6f 2c 20 70 61 63 6b 65 64 20 62 69 6e 61 72 79 20 77 6f 72 6c 64 21 0a"));

    // The same request as in HEXDUMP.md, built from the structs.
    Request q;
    q.path = "/hello.txt";
    q.headers = {{"Host", "localhost:9000"}, {"User-Agent", "pbf/1"}, {"Accept", "*/*"}, {"X-Trace", "demo"}};
    Bytes req = Frame{TYPE_REQUEST, 1, q.marshal()}.marshal();
    CHECK(req ==
          from_hex("7f 50 42 46 01 01 00 01 00 00 00 3c 01 00 0a 2f 68 65 6c 6c 6f 2e 74 78 74 04 01 00 0e 6c 6f 63 61 6c 68 6f 73 "
                   "74 3a 39 30 30 30 02 00 05 70 62 66 2f 31 03 00 03 2a 2f 2a 00 07 58 2d 54 72 61 63 65 00 04 64 65 6d 6f"));
}

static void test_mtime_is_exact() {
    // Regression: Last-Modified was once derived through two clock reads and came out one second low.
    fs::path f = root / "example" / "hello.txt";
    set_mtime(f, FIXED_MTIME);
    for (int i = 0; i < 300; ++i) {
        std::error_code ec;
        CHECK(mtime_seconds(f, ec) == FIXED_MTIME && !ec);
    }
}

static void test_spec_example_bytes() {
    // The worked example in SPEC.md / HEXDUMP.md (without the server's file headers), byte for byte.
    Request q;
    q.path = "/hello.txt";
    q.headers = {{"Host", "localhost:9000"}, {"User-Agent", "pbf/1"}};
    Bytes req = Frame{TYPE_REQUEST, 1, q.marshal()}.marshal();
    CHECK(req == from_hex("7f 50 42 46 01 01 00 01 00 00 00 27 01 00 0a 2f 68 65 6c 6c 6f 2e 74 78 74 02 01 00 0e 6c 6f 63 61 6c "
                          "68 6f 73 74 3a 39 30 30 30 02 00 05 70 62 66 2f 31"));

    Response r = ok("text/plain; charset=utf-8", bytes(hello));
    Bytes resp = Frame{TYPE_RESPONSE, 1, r.marshal()}.marshal();
    CHECK(resp == from_hex("7f 50 42 46 01 02 00 01 00 00 00 49 00 c8 03 04 00 19 74 65 78 74 2f 70 6c 61 69 6e 3b 20 63 68 61 72 "
                           "73 65 74 3d 75 74 66 2d 38 05 00 02 32 38 06 00 06 70 62 66 64 2f 31 48 65 6c 6c 6f 2c 20 70 61 63 6b "
                           "65 64 20 62 69 6e 61 72 79 20 77 6f 72 6c 64 21 0a"));
}

template <typename F>
static bool throws_malformed(F f) {
    try {
        f();
    } catch (const Malformed&) {
        return true;
    }
    return false;
}

static void test_malformed_payloads() {
    Bytes good = get_payload("/index.html");
    // Every possible truncation of a valid request is rejected.
    for (std::size_t i = 0; i < good.size(); ++i)
        CHECK(throws_malformed([&] { parse_request(Bytes(good.begin(), good.begin() + static_cast<long>(i))); }));
    Bytes trailing = good;
    trailing.push_back(0);
    CHECK(throws_malformed([&] { parse_request(trailing); }));
    CHECK(throws_malformed([&] { parse_request({}); }));
    CHECK(throws_malformed([&] { parse_request({METHOD_GET, 0, 0, 0}); }));                   // empty path
    CHECK(throws_malformed([&] { parse_request({METHOD_GET, 0x7f, 0xff, '/', 0}); }));        // lying path length
    CHECK(throws_malformed([&] { parse_request({METHOD_GET, 0, 1, 'x', 0}); }));              // no leading slash
    CHECK(throws_malformed([&] { parse_request({METHOD_GET, 0, 2, '/', 0, 0}); }));           // NUL in path
    CHECK(throws_malformed([&] { parse_request({METHOD_GET, 0, 1, '/', 17}); }));             // too many headers
    CHECK(throws_malformed([&] { parse_request({METHOD_GET, 0, 1, '/', 1, 0, 0, 0, 0}); }));  // literal with empty name
    CHECK(throws_malformed([&] { parse_request({METHOD_GET, 0, 1, '/', 1, 11, 0, 0}); }));    // reserved tag 11
    CHECK(throws_malformed([&] { parse_request({METHOD_GET, 0, 1, '/', 1, 255, 0, 0}); }));   // reserved tag 255
    CHECK(throws_malformed([&] { parse_request({METHOD_GET, 0, 1, '/', 1, 0, 1, 'a', 0xff, 0xff}); }));  // value too long
    CHECK(parse_request({3, 0, 1, '/', 0}).method == 3);  // unknown methods decode; the server answers 405

    // A path of exactly MAX_PATH_LEN is fine, one more is not.
    Request q;
    q.path = "/" + std::string(MAX_PATH_LEN - 1, 'a');
    CHECK(parse_request(q.marshal()).path.size() == MAX_PATH_LEN);
    q.path += "a";
    bool threw = false;
    try {
        q.marshal();
    } catch (const std::length_error&) {
        threw = true;
    }
    CHECK(threw);

    // Responses: every truncation inside status/headers is rejected; bad status is rejected.
    Response r;
    r.headers = {{"A", "b"}};
    Bytes rp = r.marshal();
    for (std::size_t i = 0; i < rp.size(); ++i)
        CHECK(throws_malformed([&] { parse_response(Bytes(rp.begin(), rp.begin() + static_cast<long>(i))); }));
    CHECK(throws_malformed([&] { parse_response({0, 99, 0}); }));
    CHECK(throws_malformed([&] { parse_response({2, 0x58, 0}); }));  // 600
}

static void test_frame_reading() {
    // Run the frame reader over a real socket pair so partial reads are exercised.
    std::string err;
    net::socket_t ln = net::listen_on("127.0.0.1", "0", err);
    CHECK(ln != net::invalid_socket);
    std::string p = std::to_string(net::local_port(ln));
    net::socket_t a = net::connect_to("127.0.0.1", p, err);
    net::socket_t b = ::accept(ln, nullptr, nullptr);
    CHECK(a != net::invalid_socket && b != net::invalid_socket);

    Bytes bad_magic = Frame{TYPE_REQUEST, 1, {}}.marshal();
    bad_magic[1] = 'X';
    send_raw(a, bad_magic);
    CHECK(read_frame(b, MAX_PAYLOAD).status == Read::desync);

    Bytes bad_version = Frame{TYPE_REQUEST, 1, {}}.marshal();
    bad_version[4] = 2;
    send_raw(a, bad_version);
    CHECK(read_frame(b, MAX_PAYLOAD).status == Read::desync);

    // Too large: drained, reported, and the next frame still decodes.
    send_raw(a, Frame{TYPE_REQUEST, 7, Bytes(100, 'x')}.marshal());
    send_raw(a, Frame{0x42, 8, bytes("unknown type")}.marshal());
    ReadResult r = read_frame(b, 50);
    CHECK(r.status == Read::too_large && r.frame.seq == 7);
    r = read_frame(b, 50);
    CHECK(r.status == Read::ok && r.frame.type == 0x42 && r.frame.seq == 8 && r.frame.payload == bytes("unknown type"));

    // Partial reads: one byte at a time.
    Bytes whole = Frame{TYPE_RESPONSE, 9, bytes("dribbled")}.marshal();
    std::thread dribble([&] {
        for (std::uint8_t c : whole) {
            net::send_all(a, &c, 1);
            std::this_thread::sleep_for(1ms);
        }
    });
    r = read_frame(b, MAX_PAYLOAD);
    dribble.join();
    CHECK(r.status == Read::ok && r.frame.seq == 9 && r.frame.payload == bytes("dribbled"));

    // Truncated: the peer disappears mid-frame.
    Bytes half = Frame{TYPE_REQUEST, 1, Bytes(40, 'y')}.marshal();
    half.resize(30);
    send_raw(a, half);
    net::close_socket(a);
    CHECK(read_frame(b, MAX_PAYLOAD).status == Read::truncated);
    CHECK(read_frame(b, MAX_PAYLOAD).status == Read::closed);
    net::close_socket(b);
    net::close_socket(ln);
}

static void test_dump() {
    std::ostringstream o;
    dump(o, ">", Frame{TYPE_REQUEST, 1, get_payload("/x")}.marshal());
    std::string s = o.str();
    CHECK(s.find("> REQUEST seq=1") != std::string::npos && s.find("path=/x") != std::string::npos);
    CHECK(s.find("00000000  7f 50 42 46 01 01 00 01  00 00 00") != std::string::npos);
    CHECK(s.find("|.PBF") != std::string::npos);
    o.str("");
    dump(o, "<", Frame{0x33, 2, {}}.marshal());
    CHECK(o.str().find("UNKNOWN type=0x33") != std::string::npos);
    o.str("");
    dump(o, "<", Frame{TYPE_RESPONSE, 3, ok("a/b", Bytes(5000, 'z')).marshal()}.marshal());
    CHECK(o.str().find("more bytes not shown") != std::string::npos);
}

// ---- end-to-end: hand-built frames against the real server ------------------

static void test_get_and_404() {
    net::socket_t s = connect();
    Response r = raw_request(s, get_payload("/hello.txt"));
    CHECK(r.status == 200 && r.body == bytes(hello));
    bool has_type = false;
    for (const Header& h : r.headers) has_type |= h.name == "Content-Type" && h.value.rfind("text/plain", 0) == 0;
    CHECK(has_type);
    r = raw_request(s, get_payload("/missing.txt"), 2);
    CHECK(r.status == 404);
    r = raw_request(s, get_payload("/sub"), 3);  // a directory is not a file
    CHECK(r.status == 404);
    r = raw_request(s, get_payload("/"), 4);
    CHECK(r.status == 404);
    net::close_socket(s);
}

static void test_bodies() {
    net::socket_t s = connect();
    CHECK(raw_request(s, get_payload("/blob.bin")).body == blob);
    CHECK(raw_request(s, get_payload("/empty.txt"), 2).body.empty());
    Response r = raw_request(s, get_payload("/empty.txt"), 3);
    CHECK(r.status == 200);
    net::close_socket(s);
}

static void test_many_requests_one_connection() {
    int before = count_logs("accepted");
    net::socket_t s = connect();
    CHECK(raw_request(s, get_payload("/index.html"), 1).status == 200);
    CHECK(raw_request(s, get_payload("/hello.txt"), 2).status == 200);
    CHECK(raw_request(s, get_payload("/missing.txt"), 3).status == 404);
    for (std::uint16_t i = 4; i < 40; ++i) CHECK(raw_request(s, get_payload("/hello.txt"), i).body == bytes(hello));
    net::close_socket(s);
    std::this_thread::sleep_for(50ms);
    CHECK(count_logs("accepted") - before == 1);
    CHECK(count_logs("closed after 39 request(s)") == 1);
}

static void test_malformed_requests_get_400_and_stay_in_sync() {
    net::socket_t s = connect();
    const Bytes cases[] = {
        {},                                         // empty payload
        {METHOD_GET, 0x7f, 0xff, '/', 'a'},         // lying path length
        {METHOD_GET, 0, 2, '/', 'a', 0, 9},         // stray byte after headers
        {METHOD_GET, 0, 1, 'a', 0},                 // no leading slash
        {METHOD_GET, 0, 3, '/', 0, 'a', 0},         // NUL inside the path
        {METHOD_GET, 0, 1, '/', 1, 4, 'H', 'o'},    // header cut short
    };
    std::uint16_t seq = 1;
    for (const Bytes& c : cases) {
        CHECK(raw_request(s, c, seq++).status == 400);
        CHECK(raw_request(s, get_payload("/hello.txt"), seq++).status == 200);  // still in sync
    }
    CHECK(raw_request(s, get_payload("/hello.txt", 2), seq++).status == 405);
    CHECK(raw_request(s, get_payload("/hello.txt"), seq++).status == 200);
    net::close_socket(s);
}

static void test_oversized_frame() {
    net::socket_t s = connect();
    Bytes huge(MAX_REQUEST_PAYLOAD + 1, 0);
    CHECK(raw_request(s, huge, 1).status == 400);
    CHECK(raw_request(s, get_payload("/hello.txt"), 2).status == 200);
    net::close_socket(s);
}

static void test_bad_magic_and_version_close() {
    for (int which = 0; which < 2; ++which) {
        net::socket_t s = connect();
        Bytes f = Frame{TYPE_REQUEST, 1, get_payload("/hello.txt")}.marshal();
        if (which == 0)
            f[0] = 'G';
        else
            f[4] = 9;
        send_raw(s, f);
        CHECK(read_frame(s, MAX_PAYLOAD).status == Read::closed);
        net::close_socket(s);
    }
    std::this_thread::sleep_for(50ms);
    CHECK(count_logs("closing: bad magic or version") == 2);
}

static void test_unknown_frame_type_is_skipped() {
    net::socket_t s = connect();
    send_raw(s, Frame{0x7e, 5, Bytes(300, 'q')}.marshal());
    send_raw(s, Frame{0x00, 6, {}}.marshal());
    CHECK(raw_request(s, get_payload("/hello.txt"), 7).status == 200);
    net::close_socket(s);
    std::this_thread::sleep_for(50ms);
    CHECK(count_logs("skipped frame of unknown type 0x7e (300 bytes)") == 1);
    CHECK(count_logs("skipped frame of unknown type 0x0 (0 bytes)") == 1);
}

static void test_partial_reads_over_tcp() {
    net::socket_t s = connect();
    Bytes f = Frame{TYPE_REQUEST, 1, get_payload("/hello.txt")}.marshal();
    for (std::uint8_t c : f) {  // one byte per segment
        net::send_all(s, &c, 1);
        std::this_thread::sleep_for(1ms);
    }
    ReadResult r = read_frame(s, MAX_PAYLOAD);
    CHECK(r.status == Read::ok && parse_response(r.frame.payload).body == bytes(hello));
    net::close_socket(s);
}

static void test_partial_writes_to_slow_reader() {
    net::socket_t s = connect();
    send_raw(s, Frame{TYPE_REQUEST, 1, get_payload("/blob.bin")}.marshal());
    // Read the 1 MiB response in small sips so the server's send() cannot
    // complete in one call once the socket buffers fill.
    Bytes all;
    std::uint8_t buf[1024];
    std::size_t want = HEADER_SIZE;
    while (all.size() < want) {
        std::size_t n = net::recv_exact(s, buf, std::min(sizeof buf, want - all.size()));
        if (n == 0) break;
        all.insert(all.end(), buf, buf + n);
        if (all.size() == HEADER_SIZE) want += get32(all.data() + 8);
        if (all.size() % (64 * 1024) < 1024) std::this_thread::sleep_for(2ms);
    }
    Frame f;
    std::uint32_t len = 0;
    CHECK(decode_header(all.data(), f, len) && all.size() == HEADER_SIZE + len);
    CHECK(parse_response(Bytes(all.begin() + HEADER_SIZE, all.end())).body == blob);
    net::close_socket(s);
}

static void test_long_path() {
    net::socket_t s = connect();
    Response r = raw_request(s, get_payload(long_path));
    if (long_path_exists)
        CHECK(r.status == 200 && r.body == bytes("deep\n"));
    else
        CHECK(r.status == 404);  // filesystem refused the fixture; the protocol still accepts the path
    net::close_socket(s);
}

static void test_path_traversal() {
    net::socket_t s = connect();
    const char* rejected[] = {"/../secret.txt", "/sub/../../secret.txt", "/..", "/../", "/sub/..", "/..\\secret.txt",
                              "/sub\\..\\secret.txt", "/C:/Windows/win.ini"};
    std::uint16_t seq = 1;
    for (const char* p : rejected) {
        Response r = raw_request(s, get_payload(p), seq++);
        CHECK((r.status == 400 || r.status == 404) && r.status != 200);
        CHECK(r.body != bytes("secret\n"));
    }
    CHECK(raw_request(s, get_payload("/%2e%2e/secret.txt"), seq++).status == 404);  // no percent-decoding
    CHECK(raw_request(s, get_payload("/sub/deeper.txt"), seq++).status == 200);
    CHECK(raw_request(s, get_payload("/sub/../hello.txt"), seq++).status == 400);  // even when it would stay inside
    std::error_code ec;
    fs::create_symlink(root.parent_path() / "secret.txt", root / "escape.txt", ec);
    if (ec)
        std::cerr << "note: symlink escape test skipped (" << ec.message() << ")\n";
    else
        CHECK(raw_request(s, get_payload("/escape.txt"), seq++).status == 400);
    net::close_socket(s);
}

static void test_file_too_large_is_500() {
    net::socket_t s = connect();
    Response r = raw_request(s, get_payload("/big.bin"));
    CHECK(r.status == 500);
    CHECK(raw_request(s, get_payload("/hello.txt"), 2).status == 200);
    net::close_socket(s);
}

// ---- end-to-end: the real client code ---------------------------------------

static int run_client(std::vector<std::string> args, std::string& out, std::string& err) {
    std::ostringstream o, e;
    int code = client_main(std::move(args), o, e);
    out = o.str();
    err = e.str();
    return code;
}

static void test_client() {
    std::string out, err;
    CHECK(run_client({hostport() + "/hello.txt"}, out, err) == 0);
    CHECK(out == hello && err.empty());

    CHECK(run_client({hostport() + "/blob.bin"}, out, err) == 0);
    CHECK(out == std::string(blob.begin(), blob.end()));

    CHECK(run_client({hostport() + "/missing.txt"}, out, err) == 1);
    CHECK(out == "404 Not Found\n" && err.find("status 404") != std::string::npos);

    CHECK(run_client({hostport() + "/big.bin"}, out, err) == 1);
    CHECK(err.find("status 500") != std::string::npos);

    CHECK(run_client({hostport() + "/../secret.txt"}, out, err) == 1);
    CHECK(err.find("status 400") != std::string::npos);

    int before = count_logs("accepted");
    CHECK(run_client({hostport() + "/index.html", "/hello.txt", "/missing.txt", hostport() + "/empty.txt"}, out, err) == 1);
    std::ifstream index(root / "index.html", std::ios::binary);
    std::string index_text((std::istreambuf_iterator<char>(index)), {});
    CHECK(out == index_text + hello + "404 Not Found\n");
    std::this_thread::sleep_for(50ms);
    CHECK(count_logs("accepted") - before == 1);
    CHECK(count_logs("closed after 4 request(s)") >= 1);

    CHECK(run_client({"-v", hostport() + "/hello.txt"}, out, err) == 0);
    CHECK(out == hello);
    CHECK(err.find("> REQUEST seq=1") != std::string::npos && err.find("< RESPONSE seq=1") != std::string::npos);
    CHECK(err.find("00000000  7f 50 42 46 01 01") != std::string::npos && err.find("00000000  7f 50 42 46 01 02") != std::string::npos);
    CHECK(err.find("path=/hello.txt") != std::string::npos && err.find("status=200") != std::string::npos);

    // The client always sends Host, User-Agent and Accept; -H adds more (numbered when the name is in the table).
    CHECK(run_client({"-v", "-H", "X-Trace: demo", "-H", "accept-language:en", hostport() + "/hello.txt"}, out, err) == 0);
    CHECK(out == hello);
    CHECK(err.find("Accept=\"*/*\"") != std::string::npos);
    CHECK(err.find("X-Trace=\"demo\"") != std::string::npos && err.find("accept-language=\"en\"") != std::string::npos);
    CHECK(run_client({"-H", "no-colon", hostport() + "/hello.txt"}, out, err) == 2);
    CHECK(run_client({"-x", hostport() + "/hello.txt"}, out, err) == 2);
    CHECK(err.find("unknown option") != std::string::npos);

    CHECK(run_client({}, out, err) == 2);
    CHECK(run_client({"/hello.txt"}, out, err) == 2);
    CHECK(run_client({hostport() + "/a", "other:1/b"}, out, err) == 2);
    CHECK(run_client({hostport() + "/" + std::string(MAX_PATH_LEN, 'a')}, out, err) == 2);
    CHECK(run_client({"127.0.0.1:1/hello.txt"}, out, err) == 2);  // connection refused
    CHECK(err.find("cannot connect") != std::string::npos);

    // A server that speaks something else entirely.
    std::string lerr;
    net::socket_t ln = net::listen_on("127.0.0.1", "0", lerr);
    std::thread fake([&] {
        net::socket_t c = ::accept(ln, nullptr, nullptr);
        char sink[256];
        ::recv(c, sink, sizeof sink, 0);  // read the request first: closing with unread data would send a reset
        std::string http = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
        net::send_all(c, http.data(), http.size());
        net::close_socket(c);
    });
    CHECK(run_client({"127.0.0.1:" + std::to_string(net::local_port(ln)) + "/x"}, out, err) == 2);
    CHECK(err.find("out of sync") != std::string::npos);
    fake.join();
    net::stop_listening(ln);
}

// ---- main -------------------------------------------------------------------

int main() {
    net::init();
    root = fs::temp_directory_path() / ("pbf-test-" + std::to_string(std::random_device{}()));
    fs::create_directories(root / "sub");
    write_file(root / "hello.txt", bytes(hello));
    write_file(root / "index.html", bytes("<html><body>PBF/1 test page</body></html>\n"));
    write_file(root / "empty.txt", {});
    write_file(root / "blob.bin", blob);
    write_file(root / "sub" / "deeper.txt", bytes("deeper\n"));
    write_file(root.parent_path() / "secret.txt", bytes("secret\n"));
    write_file(root / "big.bin", {});
    fs::resize_file(root / "big.bin", MAX_PAYLOAD + 1);
    {
        // "/" + 4 x (200 chars + "/") + 219 chars = 1024 bytes
        std::string seg(200, 'd');
        long_path = "/" + seg + "/" + seg + "/" + seg + "/" + seg + "/" + std::string(219, 'f');
        std::error_code ec;
        fs::create_directories(root / fs::path(long_path.substr(1)).parent_path(), ec);
        if (!ec) {
            std::ofstream f(root / fs::path(long_path.substr(1)), std::ios::binary);
            f << "deep\n";
            long_path_exists = static_cast<bool>(f);
        }
        if (!long_path_exists) std::cerr << "note: 1024-byte path fixture could not be created on this filesystem\n";
    }

    std::string err;
    net::socket_t ln = net::listen_on("127.0.0.1", "0", err);
    if (ln == net::invalid_socket) {
        std::cerr << err << "\n";
        return 1;
    }
    port = net::local_port(ln);
    fs::path canonical_root = resolve_root(root.string());
    std::thread server([&] {
        serve(ln, canonical_root, [](const std::string& line) {
            std::lock_guard<std::mutex> lk(log_mu);
            logs.push_back(line);
        });
    });

    struct {
        const char* name;
        void (*fn)();
    } tests[] = {
        {"roundtrip", test_roundtrip},
        {"header table", test_header_table},
        {"spec example bytes", test_spec_example_bytes},
        {"hexdump example", test_hexdump_example},
        {"mtime exact", test_mtime_is_exact},
        {"file headers", test_file_headers},
        {"concurrent connections", test_concurrent_connections},
        {"malformed payloads", test_malformed_payloads},
        {"frame reading", test_frame_reading},
        {"dump", test_dump},
        {"GET 200 / 404", test_get_and_404},
        {"binary and empty bodies", test_bodies},
        {"many requests on one connection", test_many_requests_one_connection},
        {"malformed requests -> 400, stay in sync", test_malformed_requests_get_400_and_stay_in_sync},
        {"oversized frame -> 400", test_oversized_frame},
        {"bad magic / version close the connection", test_bad_magic_and_version_close},
        {"unknown frame type skipped", test_unknown_frame_type_is_skipped},
        {"partial reads", test_partial_reads_over_tcp},
        {"partial writes", test_partial_writes_to_slow_reader},
        {"long path", test_long_path},
        {"path traversal", test_path_traversal},
        {"file too large -> 500", test_file_too_large_is_500},
        {"client", test_client},
    };
    for (auto& t : tests) {
        int before = failures;
        t.fn();
        std::cout << (failures == before ? "ok   " : "FAIL ") << t.name << std::endl;
    }

    net::stop_listening(ln);  // makes the server's accept() fail, so serve() returns
    server.join();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove(root.parent_path() / "secret.txt", ec);
    std::cout << (failures == 0 ? "ALL TESTS PASSED\n" : std::to_string(failures) + " CHECK(S) FAILED\n");
    return failures == 0 ? 0 : 1;
}
