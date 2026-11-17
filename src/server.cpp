#include "server.h"

#include "protocol.h"
#include "writer.h"

#include <arpa/inet.h>
#include <array>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

namespace calf {

namespace {

// Copy deleted: a copy closes fd twice, and the second close() can hit an fd
// that accept() has since reused for another connection.
class FdGuard {
public:
    explicit FdGuard(int fd) : fd_(fd) {}
    ~FdGuard() { ::close(fd_); }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;

private:
    int fd_;
};

std::vector<std::byte> make_reply(uint8_t status) {
    std::vector<std::byte> reply;
    put_u32(reply, 1);
    reply.push_back(std::byte{status});
    return reply;
}

std::vector<std::byte> make_query_reply(const std::optional<std::vector<Sample>>& samples) {
    if (!samples) return make_reply(kStatusError);
    const auto n = static_cast<uint32_t>(samples->size());
    std::vector<std::byte> reply;
    reply.reserve(4 + 1 + 4 + 16 * size_t{n});
    put_u32(reply, 1 + 4 + 16 * n);  // n <= kMaxQuerySamples, so this cannot wrap
    reply.push_back(std::byte{kStatusOk});
    put_u32(reply, n);
    for (const Sample& s : *samples) {
        put_u64(reply, static_cast<uint64_t>(s.ts_ms));
        put_u64(reply, std::bit_cast<uint64_t>(s.value));
    }
    return reply;
}

std::vector<std::byte> make_stats_reply(const WriterStats& s) {
    std::vector<std::byte> reply;
    put_u32(reply, 17);
    reply.push_back(std::byte{kStatusOk});
    put_u64(reply, s.batches);
    put_u64(reply, s.requests);
    return reply;
}

void handle_connection(int fd, Writer& writer, const Head& head) {
    FdGuard guard(fd);

    for (;;) {
        std::array<std::byte, 4> header;
        if (!read_exact(fd, header)) return;

        // Check len before allocating, or a bad length allocates up to 4 GiB.
        uint32_t len = get_u32(header);
        if (len > kMaxFrame) return;

        std::vector<std::byte> payload(len);
        if (!read_exact(fd, payload)) return;
        std::optional<Request> req = parse_request(payload);
        if (!req) return;

        // One request in flight: no new frame until the reply is sent.
        std::vector<std::byte> reply;
        if (auto* w = std::get_if<WriteRequest>(&*req)) {
            // get() blocks until the writer has synced or rejected the request.
            Status status = writer.submit(std::move(*w)).get();
            reply = make_reply(static_cast<uint8_t>(status));
        } else if (auto* q = std::get_if<QueryRequest>(&*req)) {
            reply = make_query_reply(head.query(q->name, q->start_ms, q->end_ms));
        } else {
            reply = make_stats_reply(writer.stats());
        }
        if (!write_exact(fd, reply)) return;
    }
}

}  // namespace

int run_server(uint16_t port, Wal wal, Head head, uint32_t next_id, size_t max_batch) {
    int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        std::perror("socket");
        return 1;
    }
    FdGuard listen_guard(lfd);

    int one = 1;
    if (::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) {
        std::perror("setsockopt(SO_REUSEADDR)");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        std::perror("bind");
        return 1;
    }
    if (::listen(lfd, SOMAXCONN) < 0) {
        std::perror("listen");
        return 1;
    }
    Writer writer(std::move(wal), head, next_id, max_batch);
    std::fprintf(stderr, "listening on 127.0.0.1:%u\n", static_cast<unsigned>(port));

    for (;;) {
        int fd = ::accept(lfd, nullptr, nullptr);
        if (fd < 0) {
            // ECONNABORTED: the peer reset before accept(). Not a server error.
            if (errno == EINTR || errno == ECONNABORTED) continue;
            // Other errors (for example EMFILE) repeat at once, so a retry
            // would spin.
            std::perror("accept");
            // _exit, not return: detached threads still use writer and head,
            // which live in this frame. Acked writes are already durable.
            std::_Exit(1);
        }

        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        // [fd], not [&fd]: fd dies with this iteration, the detached thread
        // does not.
        try {
            // &writer and &head are safe: both live until the process ends,
            // because this function never returns after the writer starts.
            std::thread([fd, &writer, &head] { handle_connection(fd, writer, head); }).detach();
        } catch (const std::system_error& e) {
            // No thread started, so no FdGuard owns fd. Close it here.
            std::fprintf(stderr, "thread: %s\n", e.what());
            ::close(fd);
        }
    }
}

}  // namespace calf
