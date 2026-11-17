#pragma once

#include "chunk/bitstream.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace calf {

inline constexpr uint16_t kChunkMaxSamples = 120;
inline constexpr int64_t kChunkWindowMs = 7'200'000;  // 2 hours

// C++ division rounds toward zero, so -1 / 7,200,000 is 0. The window of a
// negative ts must be -1.
int64_t floor_div(int64_t a, int64_t b);

// Bytes: u16 sample count (little-endian), then the bit stream.
class Chunk {
public:
    Chunk();

    // The only place with the cut rule. It also keeps delta and dod inside
    // (-7,200,000, 7,200,000), so they cannot overflow.
    bool fits(int64_t ts) const;

    // Returns false and changes nothing if fits(ts) is false.
    bool append(int64_t ts, double value);

    uint16_t count() const { return count_; }
    int64_t min_t() const { return min_t_; }
    int64_t max_t() const { return max_t_; }
    std::span<const std::byte> bytes() const { return w_.bytes(); }

private:
    void write_ts(int64_t ts);
    void write_value(uint64_t bits);

    BitWriter w_;
    uint16_t count_ = 0;
    int64_t min_t_ = 0;
    int64_t max_t_ = 0;  // also the previous ts
    int64_t prev_delta_ = 0;
    uint64_t prev_bits_ = 0;
    bool has_window_ = false;
    int leading_ = 0;
    int trailing_ = 0;
};

class ChunkIterator {
public:
    explicit ChunkIterator(std::span<const std::byte> bytes);

    // False after count samples, or on an error. Check error() after false.
    bool next();
    bool error() const { return error_; }

    int64_t ts() const { return ts_; }
    double value() const;
    uint64_t value_bits() const { return bits_; }

private:
    bool read_ts();
    bool read_value();

    BitReader r_;
    uint16_t count_ = 0;
    uint16_t i_ = 0;
    bool error_ = false;
    int64_t ts_ = 0;
    int64_t delta_ = 0;
    uint64_t bits_ = 0;
    bool has_window_ = false;
    int leading_ = 0;
    int trailing_ = 0;
};

}  // namespace calf
