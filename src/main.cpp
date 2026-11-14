#include "server.h"

#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    // SIGPIPE kills the process by default. With SIG_IGN, send() to a closed
    // peer returns EPIPE and only that connection ends.
    std::signal(SIGPIPE, SIG_IGN);

    uint16_t port = 7070;
    if (argc > 2) {
        std::fprintf(stderr, "usage: server [port]\n");
        return 1;
    }
    if (argc == 2) {
        const char* end = argv[1] + std::strlen(argv[1]);
        auto [p, ec] = std::from_chars(argv[1], end, port);
        if (ec != std::errc{} || p != end || port == 0) {
            std::fprintf(stderr, "usage: server [port]\n");
            return 1;
        }
    }
    return calf::run_server(port);
}
