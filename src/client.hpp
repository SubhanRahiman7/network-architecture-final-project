// The PBF/1 client: one connection, any number of GETs, curl-like output.
#pragma once

#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "net.hpp"
#include "wire.hpp"

namespace pbf {

// fetch sends one GET for `path` and returns the matching response. Frames of
// unknown type are skipped. With a non-null dump stream every frame sent or
// received is dumped. Throws std::runtime_error on a transport or protocol error.
inline Response fetch(net::socket_t s, const std::string& host, const std::string& path, std::uint16_t seq,
                      std::ostream* dump_to, const std::vector<Header>& extra = {}) {
    Request q;
    q.path = path;
    q.headers = {{"Host", host}, {"User-Agent", "pbf/1"}, {"Accept", "*/*"}};
    q.headers.insert(q.headers.end(), extra.begin(), extra.end());
    Bytes req = Frame{TYPE_REQUEST, seq, q.marshal()}.marshal();
    if (dump_to) dump(*dump_to, ">", req);
    if (!net::send_all(s, req.data(), req.size())) throw std::runtime_error("send failed: " + net::last_error());
    for (;;) {
        ReadResult r = read_frame(s, MAX_PAYLOAD);
        if (r.status == Read::closed) throw std::runtime_error("server closed the connection before answering");
        if (r.status != Read::ok) throw std::runtime_error(r.detail);
        if (dump_to) dump(*dump_to, "<", r.frame.marshal());
        if (r.frame.type != TYPE_RESPONSE) continue;
        if (r.frame.seq != seq)
            throw std::runtime_error("response seq " + std::to_string(r.frame.seq) + " does not match request seq " +
                                     std::to_string(seq));
        try {
            return parse_response(r.frame.payload);
        } catch (const Malformed& e) {
            throw std::runtime_error(std::string("malformed response: ") + e.what());
        }
    }
}

constexpr const char* USAGE = "usage: pbf [-v] [-H \"Name: value\" ...] host:port/path [/path | host:port/path ...]\n";

// targets splits "host:port/path" arguments into one host:port and the paths.
// A bare "/path" reuses the host; a different host is refused because every
// request must travel over the one connection.
inline std::string targets(const std::vector<std::string>& args, std::vector<std::string>& paths) {
    if (args.empty()) throw std::runtime_error("no target given");
    std::string host;
    for (const std::string& a : args) {
        std::size_t slash = a.find('/');
        std::string h = a.substr(0, slash);
        std::string p = slash == std::string::npos ? "/" : a.substr(slash);
        if (h.empty() && host.empty()) throw std::runtime_error(a + ": the first target must be host:port/path");
        if (host.empty()) host = h;
        if (!h.empty() && h != host)
            throw std::runtime_error(a + ": all targets must use " + host + " so they share one connection");
        paths.push_back(p);
    }
    return host;
}

// client_main is the whole program behind `pbf`, separated from main() so the
// tests can run it in-process. Exit status: 0 all 2xx, 1 some non-2xx,
// 2 usage, network or protocol error.
inline int client_main(std::vector<std::string> args, std::ostream& out, std::ostream& err) {
    std::ostream* dump_to = nullptr;
    std::vector<Header> extra;  // from -H: sent as literal (or numbered) header fields
    while (!args.empty() && args[0].size() > 1 && args[0][0] == '-') {
        if (args[0] == "-v") {
            dump_to = &err;
            args.erase(args.begin());
        } else if (args[0] == "-H" && args.size() > 1) {
            std::size_t colon = args[1].find(':');
            if (colon == std::string::npos || colon == 0) {
                err << "pbf: -H expects \"Name: value\", got \"" << args[1] << "\"\n" << USAGE;
                return 2;
            }
            std::size_t v = colon + 1;
            while (v < args[1].size() && args[1][v] == ' ') ++v;
            extra.push_back({args[1].substr(0, colon), args[1].substr(v)});
            args.erase(args.begin(), args.begin() + 2);
        } else {
            err << "pbf: unknown option " << args[0] << "\n" << USAGE;
            return 2;
        }
    }
    std::vector<std::string> paths;
    std::string hostport;
    try {
        hostport = targets(args, paths);
    } catch (const std::runtime_error& e) {
        err << "pbf: " << e.what() << "\n" << USAGE;
        return 2;
    }
    std::size_t colon = hostport.rfind(':');
    if (colon == std::string::npos) {
        err << "pbf: " << hostport << ": expected host:port\n" << USAGE;
        return 2;
    }
    std::string error;
    net::socket_t s = net::connect_to(hostport.substr(0, colon), hostport.substr(colon + 1), error);
    if (s == net::invalid_socket) {
        err << "pbf: " << error << "\n";
        return 2;
    }
    int code = 0;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        try {
            Response r = fetch(s, hostport, paths[i], static_cast<std::uint16_t>(i + 1), dump_to, extra);
            out.write(reinterpret_cast<const char*>(r.body.data()), static_cast<std::streamsize>(r.body.size()));
            out.flush();
            if (r.status / 100 != 2) {
                err << "pbf: " << paths[i] << ": status " << r.status << "\n";
                code = 1;
            }
        } catch (const std::exception& e) {  // runtime_error from fetch, length_error from marshal
            err << "pbf: " << paths[i] << ": " << e.what() << "\n";
            code = 2;
            break;
        }
    }
    net::close_socket(s);
    return code;
}

}  // namespace pbf
