#include "chunk/xor_chunk.h"

#include <algorithm>
#include <bit>

namespace calf {

namespace {

struct Bucket {
    uint64_t code;
    int code_len;
    int bits;
};

// Buckets after '0' (dod == 0). The last one, '1111', holds 64 bits.
// fits() bounds dod to 24 bits, but 64 matches Prometheus and is rare.
constexpr Bucket kBuckets[] = {{0b10, 2, 14}, {0b110, 3, 17}, {0b1110, 4, 20}};

// The decoder can be given damaged bytes. Unsigned addition wraps, so a bad
// delta gives a wrong ts instead of signed overflow.
int64_t wrap_add(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
}

int64_t window(int64_t ts) { return floor_div(ts, kChunkWindowMs); }

}  // namespace

int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if (a % b != 0 && (a < 0) != (b < 0)) --q;
    return q;
}

Chunk::Chunk() : w_(std::vector<std::byte>(2)) {}

bool Chunk::fits(int64_t ts) const {
    if (count_ == 0) return true;
    return count_ < kChunkMaxSamples && ts > max_t_ && window(ts) == window(min_t_);
}

bool Chunk::append(int64_t ts, double value) {
    if (!fits(ts)) return false;
    write_ts(ts);
    write_value(std::bit_cast<uint64_t>(value));
    if (count_ == 0) min_t_ = ts;
    max_t_ = ts;
    ++count_;
    w_.bytes()[0] = static_cast<std::byte>(count_ & 0xff);
    w_.bytes()[1] = static_cast<std::byte>(count_ >> 8);
    return true;
}

void Chunk::write_ts(int64_t ts) {
    if (count_ == 0) {
        write_varint(w_, ts);
        return;
    }
    const int64_t delta = ts - max_t_;
    if (count_ == 1) {
        write_uvarint(w_, static_cast<uint64_t>(delta));
    } else {
        const int64_t dod = delta - prev_delta_;
        if (dod == 0) {
            w_.write_bit(false);
        } else {
            const Bucket* b = std::find_if(std::begin(kBuckets), std::end(kBuckets),
                                           [dod](const Bucket& b) {
                                               const int64_t half = int64_t{1} << (b.bits - 1);
                                               return dod >= -half && dod < half;
                                           });
            // The cast keeps the two's complement bits, and write_bits keeps
            // the low b->bits of them. The reader sign-extends them back.
            if (b != std::end(kBuckets)) {
                w_.write_bits(b->code, b->code_len);
                w_.write_bits(static_cast<uint64_t>(dod), b->bits);
            } else {
                w_.write_bits(0b1111, 4);
                w_.write_bits(static_cast<uint64_t>(dod), 64);
            }
        }
    }
    prev_delta_ = delta;
}

void Chunk::write_value(uint64_t bits) {
    if (count_ == 0) {
        w_.write_bits(bits, 64);
        prev_bits_ = bits;
        return;
    }
    const uint64_t x = bits ^ prev_bits_;
    prev_bits_ = bits;
    if (x == 0) {
        w_.write_bit(false);
        return;
    }
    // The leading count has only 5 bits. A capped count still decodes
    // correctly: the extra zeros go into the meaningful bits.
    const int leading = std::min(std::countl_zero(x), 31);
    const int trailing = std::countr_zero(x);
    if (has_window_ && leading >= leading_ && trailing >= trailing_) {
        w_.write_bits(0b10, 2);
        w_.write_bits(x >> trailing_, 64 - leading_ - trailing_);
        return;
    }
    // sigbits is 1..64 because x != 0. 64 does not fit in 6 bits, and 0 can
    // never occur, so 0 stands for 64.
    const int sigbits = 64 - leading - trailing;
    w_.write_bits(0b11, 2);
    w_.write_bits(static_cast<uint64_t>(leading), 5);
    w_.write_bits(static_cast<uint64_t>(sigbits == 64 ? 0 : sigbits), 6);
    w_.write_bits(x >> trailing, sigbits);
    has_window_ = true;
    leading_ = leading;
    trailing_ = trailing;
}

ChunkIterator::ChunkIterator(std::span<const std::byte> bytes)
    : r_(bytes.size() >= 2 ? bytes.subspan(2) : std::span<const std::byte>{}) {
    if (bytes.size() < 2) {
        error_ = true;
        return;
    }
    count_ = static_cast<uint16_t>(std::to_integer<unsigned>(bytes[0]) |
                                   (std::to_integer<unsigned>(bytes[1]) << 8));
}

double ChunkIterator::value() const { return std::bit_cast<double>(bits_); }

bool ChunkIterator::next() {
    if (error_ || i_ >= count_) return false;
    if (!read_ts() || !read_value()) {
        error_ = true;
        return false;
    }
    ++i_;
    return true;
}

bool ChunkIterator::read_ts() {
    if (i_ == 0) {
        auto v = read_varint(r_);
        if (!v) return false;
        ts_ = *v;
        return true;
    }
    if (i_ == 1) {
        auto d = read_uvarint(r_);
        if (!d) return false;
        delta_ = static_cast<int64_t>(*d);
    } else {
        int ones = 0;
        while (ones < 4) {
            auto bit = r_.read_bit();
            if (!bit) return false;
            if (!*bit) break;
            ++ones;
        }
        int64_t dod = 0;
        if (ones == 4) {
            auto v = r_.read_bits(64);
            if (!v) return false;
            dod = static_cast<int64_t>(*v);
        } else if (ones > 0) {
            const int n = kBuckets[ones - 1].bits;
            auto v = r_.read_bits(n);
            if (!v) return false;
            // Move the field's sign bit to bit 63. The arithmetic right shift
            // (defined in C++20) then copies it into the high bits.
            dod = static_cast<int64_t>(*v << (64 - n)) >> (64 - n);
        }
        delta_ = wrap_add(delta_, dod);
    }
    ts_ = wrap_add(ts_, delta_);
    return true;
}

bool ChunkIterator::read_value() {
    if (i_ == 0) {
        auto v = r_.read_bits(64);
        if (!v) return false;
        bits_ = *v;
        return true;
    }
    auto changed = r_.read_bit();
    if (!changed) return false;
    if (!*changed) return true;
    auto new_window = r_.read_bit();
    if (!new_window) return false;
    if (*new_window) {
        auto l = r_.read_bits(5);
        auto s = r_.read_bits(6);
        if (!l || !s) return false;
        const int sigbits = *s == 0 ? 64 : static_cast<int>(*s);
        // Damaged bytes can claim a window wider than 64 bits. The shift
        // below would then be negative.
        if (static_cast<int>(*l) + sigbits > 64) return false;
        has_window_ = true;
        leading_ = static_cast<int>(*l);
        trailing_ = 64 - leading_ - sigbits;
    } else if (!has_window_) {
        return false;
    }
    auto x = r_.read_bits(64 - leading_ - trailing_);
    if (!x) return false;
    bits_ ^= *x << trailing_;
    return true;
}

}  // namespace calf
