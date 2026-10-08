// The PBF/1 file server: one thread per connection, many requests per connection.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <utility>

#include <sys/stat.h>
#include <sys/types.h>

#include "net.hpp"
#include "wire.hpp"

namespace pbf {

namespace fs = std::filesystem;
using Log = std::function<void(const std::string&)>;

// A connection that completes no frame for this long is dropped, so a client
// that connects and goes silent cannot hold a thread (and a socket) forever.
constexpr int IDLE_TIMEOUT_SECONDS = 30;
// Upper bound on simultaneous connections; further ones are closed at once.
constexpr int MAX_CONNECTIONS = 64;

inline std::string content_type(const fs::path& p) {
    static const std::map<std::string, std::string> types = {
        {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"},
        {".txt", "text/plain; charset=utf-8"},  {".css", "text/css"},
        {".js", "text/javascript"},             {".json", "application/json"},
        {".png", "image/png"},                  {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},                {".gif", "image/gif"},
        {".svg", "image/svg+xml"},              {".pdf", "application/pdf"},
    };
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    auto it = types.find(ext);
    return it == types.end() ? "application/octet-stream" : it->second;
}

inline Response ok(std::string type, Bytes body) {
    Response r;
    r.headers = {{"Content-Type", std::move(type)}, {"Content-Length", std::to_string(body.size())}, {"Server", "pbfd/1"}};
    r.body = std::move(body);
    return r;
}

// Seconds since the Unix epoch of a file's last modification. stat() reports it
// exactly; converting std::filesystem's file_time_type through the clocks' "now"
// is off by up to a second whenever the two clocks are read at different instants.
inline std::int64_t mtime_seconds(const fs::path& p, std::error_code& ec) {
    struct stat st;
    if (::stat(p.string().c_str(), &st) != 0) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return 0;
    }
    ec.clear();
    return static_cast<std::int64_t>(st.st_mtime);
}

inline std::string hex(std::uint64_t v) {
    std::ostringstream o;
    o << std::hex << v;
    return o.str();
}

inline Response text(std::uint16_t status, const std::string& msg) {
    Response r = ok("text/plain", Bytes(msg.begin(), msg.end()));
    r.status = status;
    return r;
}

// resolve_root turns the document root into a canonical absolute path (symlinks
// resolved) so that every served file can be checked against it. Throws.
inline fs::path resolve_root(const std::string& dir) {
    std::error_code ec;
    fs::path root = fs::canonical(dir, ec);
    if (ec || !fs::is_directory(root)) throw std::runtime_error(dir + " is not a directory");
    return root;
}

inline Response serve_file(const fs::path& root, const std::string& path) {
    // Reject traversal on the raw path rather than normalising it away.
    for (std::size_t start = 0;;) {
        std::size_t slash = path.find('/', start);
        if (path.substr(start, slash - start) == "..") return text(400, "400 Bad Request: path traversal\n");
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (path.find('\\') != std::string::npos) return text(400, "400 Bad Request: backslash in path\n");

    // canonical() resolves symlinks, so a link that points outside the root
    // fails the containment check just like "/C:/..." would.
    std::error_code ec;
    fs::path real = fs::canonical(root / fs::path(path.substr(1)), ec);
    if (ec) return text(404, "404 Not Found\n");
    fs::path rel = real.lexically_relative(root);
    if (rel.empty() || *rel.begin() == "..") return text(400, "400 Bad Request: path escapes the document root\n");

    if (!fs::is_regular_file(real, ec)) return text(404, "404 Not Found\n");
    std::uintmax_t size = fs::file_size(real, ec);
    if (ec) return text(500, "500 Internal Server Error: cannot stat file\n");
    if (size > MAX_PAYLOAD) return text(500, "500 Internal Server Error: file does not fit in one frame\n");

    std::ifstream in(real, std::ios::binary);
    Bytes body(static_cast<std::size_t>(size));
    if (!in || (size > 0 && !in.read(reinterpret_cast<char*>(body.data()), static_cast<std::streamsize>(size))))
        return text(500, "500 Internal Server Error: cannot read file\n");
    std::int64_t mtime = mtime_seconds(real, ec);
    Response r = ok(content_type(real), std::move(body));
    // Last-Modified is decimal Unix seconds; the ETag names this size and mtime.
    r.headers.push_back({"Last-Modified", std::to_string(ec ? 0 : mtime)});
    r.headers.push_back({"ETag", "\"" + hex(size) + "-" + hex(static_cast<std::uint64_t>(ec ? 0 : mtime)) + "\""});
    r.headers.push_back({"Cache-Control", "no-cache"});
    r.headers.push_back({"Accept-Ranges", "none"});
    return r;
}

// answer maps one received frame (or the failure reading it) to a response
// and a one-line description for the log.
inline std::pair<Response, std::string> answer(const fs::path& root, const ReadResult& r) {
    if (r.status == Read::too_large) return {text(400, "400 Bad Request: " + r.detail + "\n"), "oversized request (" + r.detail + ")"};
    Request q;
    try {
        q = parse_request(r.frame.payload);
    } catch (const Malformed& e) {
        return {text(400, std::string("400 Bad Request: ") + e.what() + "\n"), std::string("malformed request (") + e.what() + ")"};
    }
    if (q.method != METHOD_GET) return {text(405, "405 Method Not Allowed\n"), "method " + std::to_string(q.method) + " " + q.path};
    return {serve_file(root, q.path), "GET " + q.path};
}

inline void serve_conn(net::socket_t c, const fs::path& root, const Log& log, int id) {
    std::string tag = "[conn " + std::to_string(id) + "] ";
    log(tag + "accepted " + net::peer_name(c));
    net::set_recv_timeout(c, IDLE_TIMEOUT_SECONDS);
    int served = 0;
    for (;;) {
        ReadResult r = read_frame(c, MAX_REQUEST_PAYLOAD);
        if (r.status == Read::closed) break;
        if (r.status == Read::ok && r.frame.type != TYPE_REQUEST) {
            std::ostringstream o;
            o << tag << "skipped frame of unknown type 0x" << std::hex << unsigned(r.frame.type) << std::dec << " ("
              << r.frame.payload.size() << " bytes)";
            log(o.str());
            continue;
        }
        if (r.status != Read::ok && r.status != Read::too_large) {
            log(tag + "closing: " + r.detail);
            break;
        }
        ++served;
        auto [resp, what] = answer(root, r);
        log(tag + "#" + std::to_string(served) + " " + what + " -> " + std::to_string(resp.status));
        if (!write_frame(c, Frame{TYPE_RESPONSE, r.frame.seq, resp.marshal()})) {
            log(tag + "write failed: " + net::last_error());
            break;
        }
    }
    log(tag + "closed after " + std::to_string(served) + " request(s)");
    net::close_socket(c);
}

// serve accepts connections until the listener fails (closed) and handles each
// on its own thread. It returns only after every connection thread has finished.
inline void serve(net::socket_t listener, const fs::path& root, const Log& log) {
    struct State {
        std::mutex mu;
        std::condition_variable cv;
        int active = 0;
    };
    auto st = std::make_shared<State>();
    for (int id = 1;; ++id) {
        net::socket_t c = ::accept(listener, nullptr, nullptr);
        if (c == net::invalid_socket) {
            if (net::interrupted()) continue;
            break;
        }
        {
            std::lock_guard<std::mutex> lk(st->mu);
            if (st->active >= MAX_CONNECTIONS) {
                log("[conn " + std::to_string(id) + "] refused: " + std::to_string(MAX_CONNECTIONS) + " connections already open");
                net::close_socket(c);
                continue;
            }
            ++st->active;
        }
        // Captured by value: the thread owns its copies, so it never touches freed stack.
        std::thread([c, root, log, id, st] {
            serve_conn(c, root, log, id);
            std::lock_guard<std::mutex> lk(st->mu);
            --st->active;
            st->cv.notify_all();
        }).detach();
    }
    std::unique_lock<std::mutex> lk(st->mu);
    st->cv.wait(lk, [&] { return st->active == 0; });
}

}  // namespace pbf
