#include "protocol.h"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace calf;
using Clock = std::chrono::steady_clock;

namespace {

struct ThreadResult {
    std::vector<double> latencies_ms;  // only requests inside the measured period
    std::string error;
};

template <typename T>
bool parse_number(const char* s, T& out) {
    const char* end = s + std::strlen(s);
    auto [p, ec] = std::from_chars(s, end, out);
    return ec == std::errc{} && p == end;
}

int connect_to(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        ::close(fd);
        return -1;
    }
    // Without TCP_NODELAY, Nagle and delayed ACK can add about 40 ms to a
    // reply, and the benchmark would measure that instead of the sync.
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

std::vector<std::byte> encode_write(const std::string& name, int64_t ts0, uint32_t s) {
    const uint32_t len = 1 + 2 + 2 + static_cast<uint32_t>(name.size()) + 4 + 16 * s;
    std::vector<std::byte> frame;
    frame.reserve(4 + len);
    put_u32(frame, len);
    frame.push_back(std::byte{kMsgWrite});
    put_u16(frame, 1);
    put_u16(frame, static_cast<uint16_t>(name.size()));
    for (char c : name) frame.push_back(static_cast<std::byte>(c));
    put_u32(frame, s);
    for (int64_t ts = ts0; ts < ts0 + s; ++ts) {
        put_u64(frame, static_cast<uint64_t>(ts));
        put_u64(frame, std::bit_cast<uint64_t>(static_cast<double>(ts)));
    }
    return frame;
}

struct Stats {
    uint64_t batches;
    uint64_t requests;
};

std::optional<Stats> get_stats(int fd) {
    std::vector<std::byte> frame;
    put_u32(frame, 1);
    frame.push_back(std::byte{kMsgStats});
    std::array<std::byte, 21> reply;
    if (!write_exact(fd, frame) || !read_exact(fd, reply)) return std::nullopt;
    if (get_u32(reply) != 17 || reply[4] != std::byte{kStatusOk}) return std::nullopt;
    return Stats{get_u64(std::span(reply).subspan(5)), get_u64(std::span(reply).subspan(13))};
}

void worker(int fd, const std::string& name, uint32_t s, int64_t start_ms,
            Clock::time_point warm_end, Clock::time_point end, ThreadResult& res) {
    int64_t ts = start_ms;
    for (;;) {
        const auto frame = encode_write(name, ts, s);
        ts += s;
        std::array<std::byte, 5> reply;
        const auto t0 = Clock::now();
        if (t0 >= end) break;
        if (!write_exact(fd, frame) || !read_exact(fd, reply)) {
            res.error = "connection lost";
            break;
        }
        const auto t1 = Clock::now();
        if (get_u32(reply) != 1 || reply[4] != std::byte{kStatusOk}) {
            res.error = "reply length " + std::to_string(get_u32(reply)) + " status " +
                        std::to_string(std::to_integer<int>(reply[4]));
            break;
        }
        // A request that crosses either edge of the measured period is
        // dropped, so requests/s divides only work done inside T by T.
        if (t0 >= warm_end && t1 <= end) {
            res.latencies_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
    }
    ::close(fd);
}

// Nearest rank: the smallest value with at least p of the samples at or below it.
double percentile(const std::vector<double>& sorted, double p) {
    const auto rank = static_cast<size_t>(std::ceil(p * static_cast<double>(sorted.size())));
    return sorted[std::max<size_t>(rank, 1) - 1];
}

int usage() {
    std::fprintf(stderr,
                 "usage: bench --writers W --samples S --seconds T [--warmup 1] [--mode label]"
                 " [port]\n");
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    int writers = 0;
    uint32_t samples = 0;
    int seconds = 0;
    int warmup = 1;
    std::string mode = "-";
    uint16_t port = 7070;

    for (int i = 1; i < argc; ++i) {
        auto value = [&]() -> const char* { return ++i < argc ? argv[i] : ""; };
        if (std::strcmp(argv[i], "--writers") == 0) {
            if (!parse_number(value(), writers)) return usage();
        } else if (std::strcmp(argv[i], "--samples") == 0) {
            if (!parse_number(value(), samples)) return usage();
        } else if (std::strcmp(argv[i], "--seconds") == 0) {
            if (!parse_number(value(), seconds)) return usage();
        } else if (std::strcmp(argv[i], "--warmup") == 0) {
            if (!parse_number(value(), warmup)) return usage();
        } else if (std::strcmp(argv[i], "--mode") == 0) {
            mode = value();
        } else if (!parse_number(argv[i], port)) {
            return usage();
        }
    }
    // samples <= 100000 keeps one frame far below the 16 MiB limit.
    if (writers < 1 || writers > 1024 || samples < 1 || samples > 100000 || seconds < 1 ||
        warmup < 0) {
        return usage();
    }

    const int64_t start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();

    // Connect before the clock starts.
    const int stats_fd = connect_to(port);
    std::vector<int> fds;
    for (int t = 0; t < writers; ++t) fds.push_back(connect_to(port));
    if (stats_fd < 0 || std::ranges::find(fds, -1) != fds.end()) {
        std::perror("connect");
        return 1;
    }

    const auto begin = Clock::now();
    const auto warm_end = begin + std::chrono::seconds(warmup);
    const auto end = warm_end + std::chrono::seconds(seconds);

    std::vector<ThreadResult> results(writers);
    std::vector<std::thread> threads;
    for (int t = 0; t < writers; ++t) {
        std::string name = "b" + std::to_string(start_ms) + ".w" + std::to_string(t);
        threads.emplace_back(worker, fds[t], std::move(name), samples, start_ms, warm_end, end,
                             std::ref(results[t]));
    }

    std::this_thread::sleep_until(warm_end);
    const auto before = get_stats(stats_fd);
    std::this_thread::sleep_until(end);
    const auto after = get_stats(stats_fd);
    // join() makes each thread's ThreadResult visible here, so no lock.
    for (auto& th : threads) th.join();
    ::close(stats_fd);

    bool failed = !before || !after;
    if (failed) std::fprintf(stderr, "STATS request failed\n");
    std::vector<double> all;
    for (int t = 0; t < writers; ++t) {
        if (!results[t].error.empty()) {
            std::fprintf(stderr, "writer %d: %s\n", t, results[t].error.c_str());
            failed = true;
        }
        all.insert(all.end(), results[t].latencies_ms.begin(), results[t].latencies_ms.end());
    }
    if (failed) return 1;
    if (all.empty() || after->batches == before->batches) {
        std::fprintf(stderr, "no requests finished in the measured period\n");
        return 1;
    }

    std::ranges::sort(all);
    double sum = 0;
    for (double v : all) sum += v;
    const double mean_ms = sum / static_cast<double>(all.size());
    const double rps = static_cast<double>(all.size()) / seconds;
    const double mean_batch = static_cast<double>(after->requests - before->requests) /
                              static_cast<double>(after->batches - before->batches);

    std::printf("%s,%d,%u,%.1f,%.1f,%.3f,%.3f,%.2f\n", mode.c_str(), writers, samples, rps,
                rps * samples, percentile(all, 0.50), percentile(all, 0.99), mean_batch);
    // Little's law needs the mean, which the CSV line does not carry.
    std::fprintf(stderr, "mean_ms %.3f\n", mean_ms);
    return 0;
}
