#include "chunk/bitstream.h"

#include <algorithm>
#include <cassert>

namespace calf {

void BitWriter::write_bit(bool bit) { write_bits(bit ? 1 : 0, 1); }

// A shift by 64 is undefined behavior, so the value is moved in pieces of at
// most 8 bits. The largest shift is n - k <= 63.
void BitWriter::write_bits(uint64_t value, int n) {
    assert(n >= 1 && n <= 64);
    while (n > 0) {
        if (free_ == 0) {
            bytes_.push_back(std::byte{0});
            free_ = 8;
        }
        const int k = std::min(n, free_);
        const auto piece = static_cast<unsigned>((value >> (n - k)) & ((1u << k) - 1));
        bytes_.back() |= static_cast<std::byte>(piece << (free_ - k));
        free_ -= k;
        n -= k;
    }
}

std::optional<bool> BitReader::read_bit() {
    auto v = read_bits(1);
    if (!v) return std::nullopt;
    return *v != 0;
}

// Same as the writer: pieces of at most 8 bits, so no shift reaches 64. The
// bits shifted out of the top of result are already consumed.
std::optional<uint64_t> BitReader::read_bits(int n) {
    assert(n >= 1 && n <= 64);
    if (bytes_.size() * 8 - pos_ < static_cast<size_t>(n)) return std::nullopt;
    uint64_t result = 0;
    while (n > 0) {
        const auto byte = std::to_integer<unsigned>(bytes_[pos_ / 8]);
        const int avail = 8 - static_cast<int>(pos_ % 8);
        const int k = std::min(n, avail);
        result = (result << k) | ((byte >> (avail - k)) & ((1u << k) - 1));
        pos_ += static_cast<size_t>(k);
        n -= k;
    }
    return result;
}

void write_uvarint(BitWriter& w, uint64_t v) {
    while (v >= 0x80) {
        w.write_bits((v & 0x7f) | 0x80, 8);
        v >>= 7;
    }
    w.write_bits(v, 8);
}

// Zigzag on uint64_t: the shifts are then defined for every input, and
// 0 - (u >> 63) is all ones exactly when v is negative.
void write_varint(BitWriter& w, int64_t v) {
    const auto u = static_cast<uint64_t>(v);
    write_uvarint(w, (u << 1) ^ (0 - (u >> 63)));
}

// A u64 needs at most 10 groups, and the 10th holds only 1 bit. Any more is
// a damaged value, and its shift would reach 64.
std::optional<uint64_t> read_uvarint(BitReader& r) {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        auto b = r.read_bits(8);
        if (!b) return std::nullopt;
        if (shift == 63 && *b > 1) return std::nullopt;
        v |= (*b & 0x7f) << shift;
        if ((*b & 0x80) == 0) return v;
    }
    return std::nullopt;
}

std::optional<int64_t> read_varint(BitReader& r) {
    auto u = read_uvarint(r);
    if (!u) return std::nullopt;
    return static_cast<int64_t>((*u >> 1) ^ (0 - (*u & 1)));
}

}  // namespace calf
