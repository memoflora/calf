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

// One buffer, so one send() carries the frame. A separate send() for the
// header can stall about 40 ms (Nagle + delayed ACK).
std::vector<std::byte> encode_request(const std::string& name, int64_t ts_ms,
                                      double value) {
    const uint32_t len = 1 + 2 + 2 + static_cast<uint32_t>(name.size()) + 4 + 16;
    std::vector<std::byte> frame;
    frame.reserve(4 + len);
    put_u32(frame, len);
    frame.push_back(std::byte{kMsgWrite});
    put_u16(frame, 1);
    put_u16(frame, static_cast<uint16_t>(name.size()));
    for (char c : name) frame.push_back(static_cast<std::byte>(c));
    put_u32(frame, 1);
    put_u64(frame, static_cast<uint64_t>(ts_ms));
    put_u64(frame, std::bit_cast<uint64_t>(value));
    return frame;
}

void worker(int thread_id, int n, uint16_t port, int64_t start_ms,
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

        auto frame = encode_request(name, start_ms + i, static_cast<double>(i));

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
        } else if (status == kStatusError) {
            ++res.rejected;
        } else {
            ++res.unknown;
            break;
        }
    }

    ::close(fd);
    res.seconds = std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
    long long n = 0;
    long long m = 0;
    long long port = 7070;
    // Each thread's last fetch_add passes n, so the counter can reach n + m.
    // n <= INT_MAX - m keeps that in an int.
    bool ok = (argc == 3 || argc == 4) &&
              parse_int(argv[2], 1, 1024, m) &&
              parse_int(argv[1], 0, INT_MAX - m, n) &&
              (argc == 3 || parse_int(argv[3], 1, 65535, port));
    if (!ok) {
        std::fprintf(stderr, "usage: client N M [port]  (1 <= M <= 1024)\n");
        return 1;
    }

    const int64_t start_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    std::atomic<int> next{0};
    std::vector<ThreadResult> results(m);
    std::vector<std::thread> threads;
    for (int t = 0; t < m; ++t) {
        threads.emplace_back(worker, t, static_cast<int>(n),
                             static_cast<uint16_t>(port), start_ms,
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

    std::printf("acked %lld\nrejected %lld\nunknown %lld\nnot sent %lld\n",
                acked, rejected, unknown, not_sent);
    if (acked + rejected + unknown + not_sent != n) {
        std::fprintf(stderr, "bug: counts do not add up to N\n");
        return 1;
    }
    return acked == n ? 0 : 1;
}
