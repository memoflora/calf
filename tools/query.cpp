#include "protocol.h"

#include <arpa/inet.h>
#include <array>
#include <charconv>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace calf;

namespace {

template <typename T>
bool parse_number(const char* s, T& out) {
    const char* end = s + std::strlen(s);
    auto [p, ec] = std::from_chars(s, end, out);
    return ec == std::errc{} && p == end;
}

int usage() {
    std::fprintf(stderr,
                 "usage: query name start_ms end_ms [port]\n"
                 "       exit 0 on OK, 2 on status ERROR, 1 on other errors\n");
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) return usage();
    const std::string name = argv[1];
    int64_t start = 0;
    int64_t end = 0;
    uint16_t port = 7070;
    if (name.size() > UINT16_MAX || !parse_number(argv[2], start) ||
        !parse_number(argv[3], end) || (argc == 5 && !parse_number(argv[4], port))) {
        return usage();
    }

    std::vector<std::byte> frame;
    put_u32(frame, static_cast<uint32_t>(1 + 2 + name.size() + 8 + 8));
    frame.push_back(std::byte{kMsgQuery});
    put_u16(frame, static_cast<uint16_t>(name.size()));
    for (char c : name) frame.push_back(static_cast<std::byte>(c));
    put_u64(frame, static_cast<uint64_t>(start));
    put_u64(frame, static_cast<uint64_t>(end));

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        std::perror("connect");
        return 1;
    }
    if (!write_exact(fd, frame)) {
        std::perror("send");
        return 1;
    }

    // Check len before allocating, as the server does for requests.
    std::array<std::byte, 4> header;
    if (!read_exact(fd, header)) {
        std::fprintf(stderr, "no reply\n");
        return 1;
    }
    const uint32_t len = get_u32(header);
    if (len < 1 || len > kMaxFrame) {
        std::fprintf(stderr, "bad reply length %u\n", len);
        return 1;
    }
    std::vector<std::byte> reply(len);
    if (!read_exact(fd, reply)) {
        std::fprintf(stderr, "short reply\n");
        return 1;
    }
    ::close(fd);

    const auto status = std::to_integer<uint8_t>(reply[0]);
    if (status != kStatusOk) {
        std::fprintf(stderr, "status %u\n", status);
        return len == 1 ? 2 : 1;
    }
    // Divide: count * 16 can wrap.
    const uint32_t count = len >= 5 ? get_u32(std::span(reply).subspan(1)) : 0;
    if (len < 5 || count != (len - 5) / 16 || (len - 5) % 16 != 0) {
        std::fprintf(stderr, "bad OK reply: length %u\n", len);
        return 1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        auto s = std::span(reply).subspan(5 + 16 * size_t{i}, 16);
        std::printf("%" PRId64 " %016" PRIx64 "\n", static_cast<int64_t>(get_u64(s)),
                    get_u64(s.subspan(8)));
    }
    std::fprintf(stderr, "status %u, %u samples\n", status, count);
    return std::fflush(stdout) == 0 ? 0 : 1;
}
