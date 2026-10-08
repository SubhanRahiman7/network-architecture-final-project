// pbfd serves files from a document root over PBF/1.
//
//     pbfd <document_root> <port>
#include <iostream>
#include <mutex>

#include "server.hpp"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: pbfd <document_root> <port>\n";
        return 2;
    }
    std::string err;
    net::socket_t ln = net::listen_on("0.0.0.0", argv[2], err);
    if (ln == net::invalid_socket) {
        std::cerr << "pbfd: " << err << "\n";
        return 1;
    }
    pbf::fs::path root;
    try {
        root = pbf::resolve_root(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << "pbfd: " << e.what() << "\n";
        return 1;
    }
    static std::mutex log_mu;  // connections are served on their own threads
    auto log = [](const std::string& line) {
        std::lock_guard<std::mutex> lk(log_mu);
        std::cerr << "pbfd: " << line << std::endl;
    };
    log("serving " + root.string() + ", listening on port " + std::to_string(net::local_port(ln)));
    pbf::serve(ln, root, log);
    std::cerr << "pbfd: accept failed: " << net::last_error() << "\n";
    return 1;
}
