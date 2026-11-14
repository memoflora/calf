#include "server.h"

#include "protocol.h"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstdio>
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

void handle_connection(int fd) {
    FdGuard guard(fd);
    const std::vector<std::byte> ok = make_reply(kStatusOk);

    for (;;) {
        std::array<std::byte, 4> header;
        if (!read_exact(fd, header)) return;

        // Check len before allocating, or a bad length allocates up to 4 GiB.
        uint32_t len = get_u32(header);
        if (len > kMaxFrame) return;

        std::vector<std::byte> payload(len);
        if (!read_exact(fd, payload)) return;
        if (!parse_request(payload)) return;

        // Step 1 has no storage: OK does not mean durable yet.
        if (!write_exact(fd, ok)) return;
    }
}

}  // namespace

int run_server(uint16_t port) {
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
    std::fprintf(stderr, "listening on 127.0.0.1:%u\n", static_cast<unsigned>(port));

    for (;;) {
        int fd = ::accept(lfd, nullptr, nullptr);
        if (fd < 0) {
            // ECONNABORTED: the peer reset before accept(). Not a server error.
            if (errno == EINTR || errno == ECONNABORTED) continue;
            // Other errors (for example EMFILE) repeat at once, so a retry
            // spins at 100% CPU. The connection cap is for later.
            std::perror("accept");
            return 1;
        }

        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        // [fd], not [&fd]: fd dies with this iteration, the detached thread
        // does not.
        try {
            std::thread([fd] { handle_connection(fd); }).detach();
        } catch (const std::system_error& e) {
            // No thread started, so no FdGuard owns fd. Close it here.
            std::fprintf(stderr, "thread: %s\n", e.what());
            ::close(fd);
        }
    }
}

}  // namespace calf
