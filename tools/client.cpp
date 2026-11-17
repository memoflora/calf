#include "protocol.h"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace calf;

namespace {

struct ThreadResult {
    int requests = 0;
    int acked = 0;
    int rejected = 0;
    int unknown = 0;
    double seconds = 0;
    std::vector<std::string> log;  // written by main after join()
};

bool parse_int(const char* s, long long min, long long max, long long& out) {
    const char* end = s + std::strlen(s);
    auto [p, ec] = std::from_chars(s, end, out);
    return ec == std::errc{} && p == end && out >= min && out <= max;
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
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

// One send() per frame: a separate header send() can stall 40 ms (Nagle +
// delayed ACK). value = ts_ms (exact below 2^53), so a checker can compute the
// bits.
std::vector<std::byte> encode_request(const std::string& name, int64_t ts0, int k) {
    const uint32_t len = 1 + 2 + 2 + static_cast<uint32_t>(name.size()) + 4 + 16 * k;
    std::vector<std::byte> frame;
    frame.reserve(4 + len);
    put_u32(frame, len);
    frame.push_back(std::byte{kMsgWrite});
    put_u16(frame, 1);
    put_u16(frame, static_cast<uint16_t>(name.size()));
    for (char c : name) frame.push_back(static_cast<std::byte>(c));
    put_u32(frame, static_cast<uint32_t>(k));
    for (int64_t ts = ts0; ts < ts0 + k; ++ts) {
        put_u64(frame, static_cast<uint64_t>(ts));
        put_u64(frame, std::bit_cast<uint64_t>(static_cast<double>(ts)));
    }
    return frame;
}

void worker(int thread_id, int n, int k, uint16_t port, int64_t start_ms, bool log,
            std::atomic<int>& next, ThreadResult& res) {
    auto t0 = std::chrono::steady_clock::now();

    // connect_to() before fetch_add: if it fails, this thread takes no index
    // and the other threads send the requests.
    int fd = connect_to(port);
    if (fd < 0) {
        std::perror("connect");
        return;
    }

    const std::string name =
        "r" + std::to_string(start_ms) + ".t" + std::to_string(thread_id);

    for (;;) {
        int i = next.fetch_add(1);
        if (i >= n) break;
        ++res.requests;

        const int64_t ts0 = start_ms + static_cast<int64_t>(i) * k;
        auto frame = encode_request(name, ts0, k);
        const std::string id = " " + name + " " + std::to_string(ts0) + " " + std::to_string(k);
        // Logged before the send: the server can write the request even if
        // this client never sees the reply.
        if (log) res.log.push_back("S" + id);

        // A failed write_exact() is unknown, not unsent: part of the frame can
        // reach the server. No retry; stop, since the stream is out of sync.
        std::array<std::byte, 5> reply;
        if (!write_exact(fd, frame) || !read_exact(fd, reply)) {
            ++res.unknown;
            break;
        }
        if (get_u32(reply) != 1) {
            ++res.unknown;
            break;
        }
        uint8_t status = std::to_integer<uint8_t>(reply[4]);
        if (status == kStatusOk) {
            ++res.acked;
            if (log) res.log.push_back("A" + id);
        } else if (status == kStatusError) {
            ++res.rejected;
            if (log) res.log.push_back("R" + id);
        } else {
            ++res.unknown;
            break;
        }
    }

    ::close(fd);
    res.seconds = std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - t0).count();
}

int usage() {
    std::fprintf(stderr,
                 "usage: client N M [port] [--samples K] [--log path]\n"
                 "       (1 <= M <= 1024, 1 <= K <= 100000)\n");
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    long long n = 0;
    long long m = 0;
    long long port = 7070;
    long long k = 1;
    const char* log_path = nullptr;

    std::vector<const char*> pos;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--samples") == 0) {
            // K <= 100000 keeps a frame far below the 16 MiB limit.
            if (++i == argc || !parse_int(argv[i], 1, 100000, k)) return usage();
        } else if (std::strcmp(argv[i], "--log") == 0) {
            if (++i == argc) return usage();
            log_path = argv[i];
        } else {
            pos.push_back(argv[i]);
        }
    }
    // Each thread's last fetch_add passes n, so the counter can reach n + m.
    // n <= INT_MAX - m keeps that in an int.
    bool ok = (pos.size() == 2 || pos.size() == 3) &&
              parse_int(pos[1], 1, 1024, m) &&
              parse_int(pos[0], 0, INT_MAX - m, n) &&
              (pos.size() == 2 || parse_int(pos[2], 1, 65535, port));
    if (!ok) return usage();

    const int64_t start_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    std::atomic<int> next{0};
    std::vector<ThreadResult> results(m);
    std::vector<std::thread> threads;
    for (int t = 0; t < m; ++t) {
        threads.emplace_back(worker, t, static_cast<int>(n), static_cast<int>(k),
                             static_cast<uint16_t>(port), start_ms, log_path != nullptr,
                             std::ref(next), std::ref(results[t]));
    }
    // join() makes each thread's ThreadResult writes visible here.
    for (auto& th : threads) th.join();

    long long acked = 0;
    long long rejected = 0;
    long long unknown = 0;
    for (int t = 0; t < m; ++t) {
        const ThreadResult& r = results[t];
        std::printf("thread %d: requests %d, time %.3f s\n", t, r.requests,
                    r.seconds);
        acked += r.acked;
        rejected += r.rejected;
        unknown += r.unknown;
    }
    long long not_sent = n - std::min<long long>(next.load(), n);

    if (log_path) {
        std::FILE* f = std::fopen(log_path, "w");
        if (!f) {
            std::perror(log_path);
            return 1;
        }
        for (const ThreadResult& r : results) {
            for (const std::string& line : r.log) std::fprintf(f, "%s\n", line.c_str());
        }
        if (std::fclose(f) != 0) {
            std::perror(log_path);
            return 1;
        }
    }

    std::printf("acked %lld\nrejected %lld\nunknown %lld\nnot sent %lld\n",
                acked, rejected, unknown, not_sent);
    if (acked + rejected + unknown + not_sent != n) {
        std::fprintf(stderr, "bug: counts do not add up to N\n");
        return 1;
    }
    return acked == n ? 0 : 1;
}
