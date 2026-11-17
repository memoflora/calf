#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace calf {

// Bits go most significant first inside each byte, as in Prometheus, so a
// hex dump reads left to right.
class BitWriter {
public:
    BitWriter() = default;
    // The writer appends after these bytes. It treats them as full.
    explicit BitWriter(std::vector<std::byte> start) : bytes_(std::move(start)) {}

    void write_bit(bool bit);
    // Writes the low n bits of value, n in 1..64.
    void write_bits(uint64_t value, int n);

    std::vector<std::byte>& bytes() { return bytes_; }
    const std::vector<std::byte>& bytes() const { return bytes_; }

private:
    std::vector<std::byte> bytes_;
    int free_ = 0;  // unused low bits in bytes_.back()
};

class BitReader {
public:
    explicit BitReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    // nullopt if the bytes end first. A truncated chunk is an error, not a
    // read past the buffer.
    std::optional<bool> read_bit();
    std::optional<uint64_t> read_bits(int n);  // n in 1..64

private:
    std::span<const std::byte> bytes_;
    size_t pos_ = 0;  // in bits
};

void write_uvarint(BitWriter& w, uint64_t v);
void write_varint(BitWriter& w, int64_t v);
std::optional<uint64_t> read_uvarint(BitReader& r);
std::optional<int64_t> read_varint(BitReader& r);

}  // namespace calf
