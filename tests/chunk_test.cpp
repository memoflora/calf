// A Release build defines NDEBUG, and then every assert below is removed.
#undef NDEBUG

#include "chunk/bitstream.h"
#include "chunk/xor_chunk.h"
#include "head.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

using calf::BitReader;
using calf::BitWriter;
using calf::Chunk;
using calf::ChunkIterator;
using calf::Sample;

constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
constexpr int64_t kMax = std::numeric_limits<int64_t>::max();

uint64_t bits_of(double v) { return std::bit_cast<uint64_t>(v); }

// Values are compared as bits: NaN != NaN as a double, and -0.0 == 0.0.
bool same(const std::vector<Sample>& a, const std::vector<Sample>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].ts_ms != b[i].ts_ms || bits_of(a[i].value) != bits_of(b[i].value)) return false;
    }
    return true;
}

// Decodes bytes. ok is false if the iterator reported an error.
std::vector<Sample> decode(std::span<const std::byte> bytes, bool& ok) {
    std::vector<Sample> out;
    ChunkIterator it(bytes);
    while (it.next()) out.push_back({it.ts(), it.value()});
    ok = !it.error();
    return out;
}

void round_trip(const std::vector<Sample>& in) {
    Chunk c;
    for (const Sample& s : in) assert(c.append(s.ts_ms, s.value));
    bool ok = false;
    assert(same(decode(c.bytes(), ok), in));
    assert(ok);
    assert(c.count() == in.size());
}

std::vector<std::byte> to_bytes(std::initializer_list<unsigned> v) {
    std::vector<std::byte> out;
    for (unsigned b : v) out.push_back(static_cast<std::byte>(b));
    return out;
}

void test_bit_order() {
    BitWriter w;
    w.write_bits(0b101, 3);
    w.write_bit(true);
    assert(w.bytes() == to_bytes({0xb0}));  // 1011 0000
}

void test_bitstream_random() {
    std::mt19937_64 rng(1);
    std::vector<std::pair<uint64_t, int>> pairs;
    BitWriter w;
    for (int i = 0; i < 10000; ++i) {
        const int n = static_cast<int>(rng() % 64) + 1;
        const uint64_t v = n == 64 ? rng() : rng() & ((uint64_t{1} << n) - 1);
        pairs.emplace_back(v, n);
        w.write_bits(v, n);
    }
    BitReader r(w.bytes());
    for (auto [v, n] : pairs) assert(r.read_bits(n) == v);
}

void test_bitstream_64() {
    BitWriter w;
    w.write_bit(true);  // so the 64-bit values cross byte boundaries
    w.write_bits(0xfedcba9876543210, 64);
    w.write_bits(~uint64_t{0}, 64);
    BitReader r(w.bytes());
    assert(r.read_bit() == true);
    assert(r.read_bits(64) == 0xfedcba9876543210);
    assert(r.read_bits(64) == ~uint64_t{0});
}

void test_bitstream_past_end() {
    BitWriter w;
    w.write_bits(0xabc, 12);  // 2 bytes, 16 readable bits
    BitReader r(w.bytes());
    assert(r.read_bits(16).has_value());
    assert(!r.read_bit().has_value());
    BitReader r2(w.bytes());
    assert(!r2.read_bits(17).has_value());
    BitReader empty({});
    assert(!empty.read_bits(1).has_value());
}

void test_varints() {
    for (int64_t v : {int64_t{0}, int64_t{1}, int64_t{-1}, int64_t{127}, int64_t{128}, kMin, kMax}) {
        BitWriter w;
        write_varint(w, v);
        write_uvarint(w, static_cast<uint64_t>(v));
        BitReader r(w.bytes());
        assert(read_varint(r) == v);
        assert(read_uvarint(r) == static_cast<uint64_t>(v));
        assert(!r.read_bit().has_value());
    }
    // Known encodings: zigzag maps -1 to 1, and 128 needs two groups.
    BitWriter w;
    write_varint(w, -1);
    write_uvarint(w, 128);
    assert(w.bytes() == to_bytes({0x01, 0x80, 0x01}));

    BitReader truncated(std::span(w.bytes()).first(2));  // 0x01, then 0x80 with no end
    assert(read_varint(truncated) == -1);
    assert(!read_uvarint(truncated).has_value());
}

// Written by hand from step4.md as bit strings, not with Chunk, so a shared
// bug cannot make both sides agree. It covers sample 0, sample 1, dod 0,
// dod +1 and -1, a new window, a reused window, and an equal value.
void test_golden() {
    Chunk c;
    assert(c.append(1000, 1.0));
    assert(c.append(1015, 1.0));
    assert(c.append(1030, 2.0));
    assert(c.append(1046, 3.0));
    assert(c.append(1061, 2.0));
    const auto expected = to_bytes({0x05, 0x00, 0xd0, 0x0f, 0x3f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x0f, 0x30, 0x97, 0xff, 0xe0, 0x00, 0x76, 0x03, 0xbf, 0xff, 0xa0});
    assert(std::ranges::equal(c.bytes(), expected));
}

void test_values() {
    const double nan1 = std::bit_cast<double>(uint64_t{0x7ff8000000000001});
    const double nan2 = std::bit_cast<double>(uint64_t{0x7ff8000000000002});
    const double inf = std::numeric_limits<double>::infinity();
    round_trip({{0, 5.0}, {1, 5.0}, {2, 5.0}});
    round_trip({{0, nan1}, {1, nan2}, {2, nan1}, {3, nan1}});
    round_trip({{0, inf}, {1, -inf}, {2, inf}});
    round_trip({{0, 0.0}, {1, -0.0}, {2, 0.0}, {3, -0.0}});
    // XOR of these has bit 63 and bit 0 set: 64 sigbits, stored as 0.
    round_trip({{0, std::bit_cast<double>(uint64_t{0})},
                {1, std::bit_cast<double>(uint64_t{0x8000000000000001})},
                {2, 1.0}});
    // XOR of 1 and 3 is 2: 62 leading zeros, capped at 31.
    round_trip({{0, std::bit_cast<double>(uint64_t{1})},
                {1, std::bit_cast<double>(uint64_t{3})},
                {2, std::bit_cast<double>(uint64_t{1})},
                {3, std::bit_cast<double>(uint64_t{0x100000001})}});
}

// Puts dod in sample 2 after a delta of 600,000, checks the round trip, and
// returns the width of the dod field that the encoder chose.
int dod_field_bits(int64_t dod) {
    constexpr int64_t d1 = 600'000;
    const std::vector<Sample> in = {{0, 1.0}, {d1, 1.0}, {2 * d1 + dod, 1.0}};
    round_trip(in);
    Chunk c;
    for (const Sample& s : in) c.append(s.ts_ms, s.value);
    BitReader r(c.bytes().subspan(2));
    assert(read_varint(r) == 0);
    assert(r.read_bits(64) == bits_of(1.0));
    assert(read_uvarint(r) == static_cast<uint64_t>(d1));
    assert(r.read_bit() == false);  // value of sample 1 unchanged
    int ones = 0;
    while (ones < 4 && r.read_bit() == true) ++ones;
    constexpr int widths[] = {0, 14, 17, 20, 64};
    return widths[ones];
}

void test_timestamps() {
    round_trip({{0, 1.0}, {1, 1.0}, {2, 1.0}, {3, 1.0}});  // delta 1
    assert(dod_field_bits(0) == 0);
    assert(dod_field_bits(1) == 14);
    assert(dod_field_bits(-1) == 14);
    assert(dod_field_bits(8191) == 14);
    assert(dod_field_bits(8192) == 17);
    assert(dod_field_bits(-8192) == 14);
    assert(dod_field_bits(-8193) == 17);
    assert(dod_field_bits(65535) == 17);
    assert(dod_field_bits(65536) == 20);
    assert(dod_field_bits(-65536) == 17);
    assert(dod_field_bits(-65537) == 20);
    assert(dod_field_bits(524287) == 20);
    assert(dod_field_bits(524288) == 64);
    assert(dod_field_bits(-524288) == 20);
    assert(dod_field_bits(-524289) == 64);
    assert(dod_field_bits(-599'999) == 64);
    round_trip({{0, 1.0}, {1, 1.0}, {1'000'001, 1.0}});  // deltas 1, then 1,000,000

    round_trip({{-7'000'000, 1.0}, {-6'999'999, 2.0}, {-5'000'000, 3.0}, {-1, 4.0}});
    round_trip({{kMin, 1.0}, {kMin + 1, 1.0}, {kMin + 15'000, 1.0}, {kMin + 30'003, 2.0}});
    round_trip({{kMax - 30'000, 1.0}, {kMax - 15'000, 1.0}, {kMax, 1.0}});
}

void test_cut_rule() {
    assert(calf::floor_div(-1, calf::kChunkWindowMs) == -1);
    assert(calf::floor_div(-calf::kChunkWindowMs, calf::kChunkWindowMs) == -1);
    assert(calf::floor_div(-calf::kChunkWindowMs - 1, calf::kChunkWindowMs) == -2);
    assert(calf::floor_div(calf::kChunkWindowMs - 1, calf::kChunkWindowMs) == 0);

    Chunk c;
    assert(c.fits(kMin) && c.fits(kMax));  // empty fits any ts
    for (int i = 0; i < 120; ++i) assert(c.append(i, 1.0));
    assert(!c.fits(120));
    const std::vector<std::byte> before(c.bytes().begin(), c.bytes().end());
    assert(!c.append(120, 2.0));
    assert(std::ranges::equal(c.bytes(), before) && c.count() == 120 && c.max_t() == 119);

    Chunk w;
    assert(w.append(7'199'998, 1.0));
    assert(w.fits(7'199'999));
    assert(!w.fits(7'200'000));  // next window
    assert(!w.fits(7'199'998));  // equal to max_t
    assert(!w.fits(5));          // before max_t
    assert(!w.append(7'200'000, 1.0));
    assert(w.count() == 1 && w.max_t() == 7'199'998);

    // -1 and 0 are 1 ms apart, but in different windows.
    Chunk neg;
    assert(neg.append(-calf::kChunkWindowMs, 1.0));
    assert(neg.fits(-1));
    assert(!neg.fits(0));
}

void test_truncated() {
    std::mt19937_64 rng(2);
    std::vector<Sample> in;
    Chunk c;
    int64_t ts = 1'000;
    for (int i = 0; i < 120; ++i) {
        ts += 1 + static_cast<int64_t>(rng() % 20'000);
        in.push_back({ts, i % 3 == 0 ? std::bit_cast<double>(rng()) : 42.0});
        assert(c.append(in.back().ts_ms, in.back().value));
    }
    const std::span<const std::byte> full = c.bytes();
    for (size_t cut = 0; cut < full.size(); ++cut) {
        // An exact-size copy, so ASan sees any read past the end.
        std::vector<std::byte> part(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(cut));
        bool ok = true;
        std::vector<Sample> got = decode(part, ok);
        assert(!ok);
        assert(got.size() < in.size());
        assert(same(got, std::vector<Sample>(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(got.size()))));
    }
}

double random_value(std::mt19937_64& rng, double prev) {
    switch (rng() % 4) {
        case 0: return prev;
        case 1: return prev + static_cast<double>(rng() % 10);
        case 2: return std::bit_cast<double>(rng());  // includes NaNs
        default: return static_cast<double>(rng() % 1000) / 7.0;
    }
}

void test_chunk_random() {
    std::mt19937_64 rng(3);
    for (int round = 0; round < 500; ++round) {
        std::vector<Sample> in;
        int64_t ts = static_cast<int64_t>(rng() % 2'000'000) - 1'000'000;
        Chunk c;
        double v = 0;
        while (true) {
            ts += 1 + static_cast<int64_t>(rng() % 120'000);
            v = random_value(rng, v);
            if (!c.append(ts, v)) break;
            in.push_back({ts, v});
        }
        bool ok = false;
        assert(same(decode(c.bytes(), ok), in));
        assert(ok);
    }
}

void test_head() {
    std::mt19937_64 rng(4);
    calf::Head h;
    std::vector<std::vector<Sample>> expected;
    std::vector<std::string> names;
    std::vector<int64_t> last;
    std::vector<std::vector<std::vector<std::byte>>> full_copies;  // per series

    for (int r = 0; r < 2000; ++r) {
        calf::CommitRecord rec;
        if (names.empty() || rng() % 10 == 0) {
            const auto id = static_cast<uint32_t>(names.size());
            names.push_back("s" + std::to_string(id));
            expected.emplace_back();
            last.push_back(static_cast<int64_t>(rng() % 100'000'000) - 50'000'000);
            full_copies.emplace_back();
            rec.new_series.push_back({id, names.back()});
            rec.samples.push_back({id, last[id], 1.0});
            expected[id].push_back({last[id], 1.0});
        }
        const int n = 1 + static_cast<int>(rng() % 20);
        for (int i = 0; i < n; ++i) {
            const auto id = static_cast<uint32_t>(rng() % names.size());
            // Mostly regular steps; sometimes a gap that crosses windows.
            last[id] += rng() % 50 == 0 ? 1 + static_cast<int64_t>(rng() % 20'000'000)
                                        : 15'000 + static_cast<int64_t>(rng() % 11) - 5;
            const double v = random_value(rng, expected[id].back().value);
            rec.samples.push_back({id, last[id], v});
            expected[id].push_back({last[id], v});
        }
        std::string error;
        assert(h.apply(rec, error));

        // Full chunks never change after the cut: earlier copies still match.
        for (size_t id = 0; id < names.size(); ++id) {
            const calf::SeriesData* d = h.find(names[id]);
            auto& copies = full_copies[id];
            assert(d->full.size() >= copies.size());
            for (size_t k = 0; k < copies.size(); ++k) {
                assert(std::ranges::equal(d->full[k].bytes(), copies[k]));
            }
            for (size_t k = copies.size(); k < d->full.size(); ++k) {
                copies.emplace_back(d->full[k].bytes().begin(), d->full[k].bytes().end());
            }
        }
    }

    size_t chunks = 0;
    for (size_t id = 0; id < names.size(); ++id) {
        assert(same(h.samples(names[id]), expected[id]));
        const calf::SeriesData* d = h.find(names[id]);
        assert(d->last_ts == last[id]);
        for (const Chunk& c : d->full) {
            assert(c.count() >= 1 && c.count() <= 120);
            assert(calf::floor_div(c.min_t(), calf::kChunkWindowMs) ==
                   calf::floor_div(c.max_t(), calf::kChunkWindowMs));
        }
        chunks += d->full.size() + 1;
    }
    assert(h.samples("missing").empty());
    // The test checks nothing about cuts if no series was ever cut.
    assert(chunks > 2 * names.size());
}

}  // namespace

int main() {
    test_bit_order();
    test_bitstream_random();
    test_bitstream_64();
    test_bitstream_past_end();
    test_varints();
    test_golden();
    test_values();
    test_timestamps();
    test_cut_rule();
    test_truncated();
    test_chunk_random();
    test_head();
    std::puts("chunk_test: all passed");
    return 0;
}
