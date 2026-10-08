// Thin socket layer: the same six calls on Winsock and POSIX.
// recv_exact / send_all are the two loops that make partial reads and partial
// writes invisible to the protocol code above them.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace net {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t invalid_socket = INVALID_SOCKET;
inline void close_socket(socket_t s) { closesocket(s); }
inline bool interrupted() { return false; }
inline std::string last_error() { return std::system_category().message(WSAGetLastError()); }
inline void init() {
    struct Once {
        Once() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
        ~Once() { WSACleanup(); }
    };
    static Once once;
}
constexpr int send_flags = 0;
#else
using socket_t = int;
constexpr socket_t invalid_socket = -1;
inline void close_socket(socket_t s) { ::close(s); }
inline bool interrupted() { return errno == EINTR; }
inline std::string last_error() { return std::strerror(errno); }
inline void init() {}
#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;  // a peer that went away must not SIGPIPE us
#else
constexpr int send_flags = 0;
#endif
#endif

// One syscall moves at most this much; keeps the int-sized Winsock length happy.
constexpr std::size_t chunk = 1 << 20;

// recv_exact reads exactly n bytes, looping over however many partial reads
// the network delivers (TCP may hand back one byte at a time). Returns the
// number of bytes read: n on success, fewer if the peer closed, the read
// timed out or an error occurred.
inline std::size_t recv_exact(socket_t s, void* buf, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        auto r = ::recv(s, static_cast<char*>(buf) + got, static_cast<int>(std::min(n - got, chunk)), 0);
        if (r < 0 && interrupted()) continue;
        if (r <= 0) break;
        got += static_cast<std::size_t>(r);
    }
    return got;
}

// send_all writes all n bytes, looping while the kernel accepts only part of
// the buffer (short writes are normal once the socket buffer is full).
inline bool send_all(socket_t s, const void* buf, std::size_t n) {
    std::size_t sent = 0;
    while (sent < n) {
        auto r = ::send(s, static_cast<const char*>(buf) + sent, static_cast<int>(std::min(n - sent, chunk)), send_flags);
        if (r < 0 && interrupted()) continue;
        if (r <= 0) return false;
        sent += static_cast<std::size_t>(r);
    }
    return true;
}

// discard reads and drops n bytes, so a frame can be skipped without storing it.
inline bool discard(socket_t s, std::uint64_t n) {
    char buf[4096];
    while (n > 0) {
        std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(n, sizeof buf));
        if (recv_exact(s, buf, k) != k) return false;
        n -= k;
    }
    return true;
}

// set_recv_timeout makes every recv give up after `seconds` of silence.
inline void set_recv_timeout(socket_t s, int seconds) {
#ifdef _WIN32
    DWORD ms = static_cast<DWORD>(seconds) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
#else
    timeval tv{};
    tv.tv_sec = seconds;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

// listen_on binds and listens on an IPv4 host:port ("0.0.0.0", "9000").
// Port "0" asks the kernel for a free port; see local_port.
inline socket_t listen_on(const std::string& host, const std::string& port, std::string& err) {
    init();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* res = nullptr;
    if (int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res); rc != 0) {
        err = "cannot resolve " + host + ":" + port + ": " + gai_strerror(rc);
        return invalid_socket;
    }
    socket_t s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == invalid_socket) {
        err = "socket: " + last_error();
    } else {
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
        if (::bind(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0 || ::listen(s, 16) != 0) {
            err = "cannot listen on " + host + ":" + port + ": " + last_error();
            close_socket(s);
            s = invalid_socket;
        }
    }
    freeaddrinfo(res);
    return s;
}

// connect_to opens a TCP connection to host:port, trying every address the
// name resolves to.
inline socket_t connect_to(const std::string& host, const std::string& port, std::string& err) {
    init();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res); rc != 0) {
        err = "cannot resolve " + host + ":" + port + ": " + gai_strerror(rc);
        return invalid_socket;
    }
    socket_t s = invalid_socket;
    for (addrinfo* a = res; a != nullptr && s == invalid_socket; a = a->ai_next) {
        s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == invalid_socket) continue;
        if (::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) != 0) {
            err = "cannot connect to " + host + ":" + port + ": " + last_error();
            close_socket(s);
            s = invalid_socket;
        }
    }
    freeaddrinfo(res);
    return s;
}

// stop_listening closes a listening socket so that a thread blocked in
// accept() on it returns. On Linux close() alone does not wake accept();
// shutdown() does.
inline void stop_listening(socket_t s) {
#ifndef _WIN32
    ::shutdown(s, SHUT_RDWR);
#endif
    close_socket(s);
}

inline std::uint16_t local_port(socket_t s) {
    sockaddr_in a{};
    socklen_t len = sizeof a;
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    return ntohs(a.sin_port);
}

inline std::string peer_name(socket_t s) {
    sockaddr_in a{};
    socklen_t len = sizeof a;
    if (getpeername(s, reinterpret_cast<sockaddr*>(&a), &len) != 0) return "?";
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

}  // namespace net
