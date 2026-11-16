#include "server.h"
#include "wal.h"

#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

namespace {

int usage() {
    std::fprintf(stderr, "usage: server [--max-batch N] [port] [wal_path]\n");
    return 1;
}

template <typename T>
bool parse_number(const char* s, T& out) {
    const char* end = s + std::strlen(s);
    auto [p, ec] = std::from_chars(s, end, out);
    return ec == std::errc{} && p == end;
}

}  // namespace

int main(int argc, char** argv) {
    // SIGPIPE kills the process by default. With SIG_IGN, send() to a closed
    // peer returns EPIPE and only that connection ends.
    std::signal(SIGPIPE, SIG_IGN);

    uint16_t port = 7070;
    std::string wal_path = "calf.wal";
    size_t max_batch = 0;

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--max-batch") == 0) {
            if (++i == argc || !parse_number(argv[i], max_batch)) return usage();
        } else if (positional == 0) {
            if (!parse_number(argv[i], port) || port == 0) return usage();
            ++positional;
        } else if (positional == 1) {
            wal_path = argv[i];
            ++positional;
        } else {
            return usage();
        }
    }

    std::optional<calf::Wal> wal = calf::Wal::create(wal_path);
    if (!wal) return 1;
    return calf::run_server(port, std::move(*wal), max_batch);
}
