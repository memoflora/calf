#include "protocol.h"

#include <bit>
#include <cerrno>
#include <sys/socket.h>

namespace calf {

// Widen each byte before <<: a byte promotes to int, and shifting into the
// sign bit of an int is UB.
uint16_t get_u16(std::span<const std::byte> b) {
    return static_cast<uint16_t>(std::to_integer<uint32_t>(b[0]) |
                                 std::to_integer<uint32_t>(b[1]) << 8);
}

uint32_t get_u32(std::span<const std::byte> b) {
    return std::to_integer<uint32_t>(b[0]) |
           std::to_integer<uint32_t>(b[1]) << 8 |
           std::to_integer<uint32_t>(b[2]) << 16 |
           std::to_integer<uint32_t>(b[3]) << 24;
}

uint64_t get_u64(std::span<const std::byte> b) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= std::to_integer<uint64_t>(b[i]) << (8 * i);
    }
    return v;
}

void put_u16(std::vector<std::byte>& out, uint16_t v) {
    for (int i = 0; i < 2; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }
}

void put_u32(std::vector<std::byte>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }
}

void put_u64(std::vector<std::byte>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
    }
}

bool read_exact(int fd, std::span<std::byte> buf) {
    size_t got = 0;
    while (got < buf.size()) {
        ssize_t r = ::recv(fd, buf.data() + got, buf.size() - got, 0);
        if (r > 0) {
            got += static_cast<size_t>(r);
        } else if (r == 0) {
            return false;
        } else if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

bool write_exact(int fd, std::span<const std::byte> buf) {
    size_t sent = 0;
    while (sent < buf.size()) {
        ssize_t r = ::send(fd, buf.data() + sent, buf.size() - sent, 0);
        if (r > 0) {
            sent += static_cast<size_t>(r);
        } else if (r == 0) {
            // send() should not return 0 for n > 0. Stop, or the loop can spin.
            return false;
        } else if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

namespace {

// take() checks n against rest_.size() first, so no read passes the end.
class Reader {
public:
    explicit Reader(std::span<const std::byte> buf) : rest_(buf) {}

    size_t remaining() const { return rest_.size(); }

    bool take(size_t n, std::span<const std::byte>& out) {
        if (n > rest_.size()) return false;
        out = rest_.first(n);
        rest_ = rest_.subspan(n);
        return true;
    }

    bool u8(uint8_t& v) {
        std::span<const std::byte> b;
        if (!take(1, b)) return false;
        v = std::to_integer<uint8_t>(b[0]);
        return true;
    }

    bool u16(uint16_t& v) {
        std::span<const std::byte> b;
        if (!take(2, b)) return false;
        v = get_u16(b);
        return true;
    }

    bool u32(uint32_t& v) {
        std::span<const std::byte> b;
        if (!take(4, b)) return false;
        v = get_u32(b);
        return true;
    }

    bool u64(uint64_t& v) {
        std::span<const std::byte> b;
        if (!take(8, b)) return false;
        v = get_u64(b);
        return true;
    }

private:
    std::span<const std::byte> rest_;
};

}  // namespace

std::optional<WriteRequest> parse_request(std::span<const std::byte> payload) {
    Reader r(payload);

    uint8_t msg_type;
    if (!r.u8(msg_type) || msg_type != kMsgWrite) return std::nullopt;

    uint16_t series_count;
    if (!r.u16(series_count)) return std::nullopt;

    // series_count is unchecked client input, so no reserve(series_count).
    WriteRequest req;
    for (uint32_t s = 0; s < series_count; ++s) {
        Series series;

        uint16_t name_len;
        std::span<const std::byte> name;
        if (!r.u16(name_len) || !r.take(name_len, name)) return std::nullopt;
        series.name.assign(reinterpret_cast<const char*>(name.data()), name.size());

        uint32_t sample_count;
        if (!r.u32(sample_count)) return std::nullopt;
        // Divide: sample_count * 16 can wrap. This bounds reserve(sample_count)
        // by the bytes left.
        if (sample_count > r.remaining() / 16) return std::nullopt;
        series.samples.reserve(sample_count);

        for (uint32_t i = 0; i < sample_count; ++i) {
            uint64_t ts_bits;
            uint64_t value_bits;
            if (!r.u64(ts_bits) || !r.u64(value_bits)) return std::nullopt;
            series.samples.push_back({static_cast<int64_t>(ts_bits),
                                      std::bit_cast<double>(value_bits)});
        }
        req.series.push_back(std::move(series));
    }

    if (r.remaining() != 0) return std::nullopt;
    return req;
}

}  // namespace calf
