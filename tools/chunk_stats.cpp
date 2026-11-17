#include "chunk/xor_chunk.h"

#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

constexpr int kSamples = 1'000'000;

// The raw mt19937_64 output is fixed by the standard. The std distributions
// are not, so libc++ and libstdc++ would print different numbers.
struct Rng {
    std::mt19937_64 gen{42};
    int64_t below(uint64_t n) { return static_cast<int64_t>(gen() % n); }
    double unit() { return static_cast<double>(gen() >> 11) * 0x1p-53; }
};

enum class Kind { counter, gauge, constant };

void run(const char* name, Kind kind) {
    Rng rng;
    std::vector<calf::Chunk> chunks(1);
    int64_t ts = 0;
    double value = kind == Kind::gauge ? 50.0 : (kind == Kind::constant ? 1.0 : 0.0);
    for (int i = 0; i < kSamples; ++i) {
        const int64_t base = int64_t{i} * 15'000;
        ts = kind == Kind::constant ? base : base + rng.below(11) - 5;
        if (kind == Kind::counter) value += static_cast<double>(rng.below(11));
        // 53 random bits fill the whole mantissa, so nearly every value bit
        // changes. This is the worst case for the XOR encoding.
        if (kind == Kind::gauge) value = 50.0 + (rng.unit() * 2.0 - 1.0);
        if (!chunks.back().fits(ts)) chunks.emplace_back();
        chunks.back().append(ts, value);
    }
    size_t bytes = 0;
    for (const calf::Chunk& c : chunks) bytes += c.bytes().size();
    std::printf("%-10s %10d %8zu %10zu %10.3f\n", name, kSamples, chunks.size(), bytes,
                static_cast<double>(bytes) / kSamples);
}

}  // namespace

int main() {
    std::printf("%-10s %10s %8s %10s %10s\n", "type", "samples", "chunks", "bytes", "bytes/smp");
    run("counter", Kind::counter);
    run("gauge", Kind::gauge);
    run("constant", Kind::constant);
    std::printf("%-10s %10d %8s %10d %10.3f\n", "raw", kSamples, "-", kSamples * 16, 16.0);
    return 0;
}
