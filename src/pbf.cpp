// pbf fetches files over PBF/1, like a tiny curl.
//
//     pbf [-v] [-H "Name: value" ...] host:port/path [/path | host:port/path ...]
//
// Every path goes over the same TCP connection. Bodies go to stdout, byte for
// byte; with -v every frame is dumped to stderr; -H adds a request header.
#include <iostream>

#include "client.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);  // no "\n" -> "\r\n" rewriting of binary bodies
#endif
    return pbf::client_main(std::vector<std::string>(argv + 1, argv + argc), std::cout, std::cerr);
}
