// A Release build defines NDEBUG, and then every assert below is removed.
#undef NDEBUG

#include "head.h"

#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <vector>

using calf::Head;
using calf::Sample;

namespace {

constexpr int64_t kStep = 15'000;
// 200 samples before the 2-hour window edge and 300 after it. With 120
// samples per chunk: full chunks [0, 119], [120, 199], [200, 319],
// [320, 439], and the open chunk [440, 499].
constexpr int64_t kT0 = calf::kChunkWindowMs - 200 * kStep;
constexpr int kN = 500;

int64_t ts(int i) { return kT0 + i * kStep; }

bool apply(Head& h, const calf::CommitRecord& rec) {
    std::string err;
    return h.apply_batch({&rec, 1}, err);
}

Head make_head() {
    Head h;
    calf::CommitRecord rec;
    rec.new_series.push_back({0, "s"});
    for (int i = 0; i < kN; ++i) rec.samples.push_back({0, ts(i), i * 1.5});
    assert(apply(h, rec));
    const calf::SeriesData* d = h.find("s");
    assert(d->full.size() == 4 && d->open.count() == 60);
    assert(d->full[1].max_t() < calf::kChunkWindowMs && d->full[2].min_t() == calf::kChunkWindowMs);
    return h;
}

// The reference: a linear scan over every sample.
std::vector<Sample> expected(int64_t start, int64_t end) {
    std::vector<Sample> out;
    for (int i = 0; i < kN; ++i) {
        if (ts(i) >= start && ts(i) <= end) out.push_back({ts(i), i * 1.5});
    }
    return out;
}

bool same(const std::vector<Sample>& a, const std::vector<Sample>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].ts_ms != b[i].ts_ms ||
            std::bit_cast<uint64_t>(a[i].value) != std::bit_cast<uint64_t>(b[i].value)) {
            return false;
        }
    }
    return true;
}

void check(const Head& h, int64_t start, int64_t end, size_t want_count) {
    auto r = h.query("s", start, end);
    assert(r);
    assert(r->size() == want_count);
    assert(same(*r, expected(start, end)));
}

void test_ranges() {
    const Head h = make_head();
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();

    check(h, kMin, ts(0) - 1, 0);                 // before all data
    check(h, ts(kN - 1) + 1, kMax, 0);            // after all data
    check(h, ts(10), ts(20), 11);                 // inside one chunk
    check(h, ts(110), ts(130), 21);               // across a chunk edge
    check(h, ts(190), ts(210), 21);               // across the window edge
    check(h, ts(430), ts(450), 21);               // into the open chunk
    check(h, ts(470), kMax, 30);                  // inside the open chunk only
    check(h, ts(120), ts(199), 80);               // exactly one chunk's min_t and max_t
    check(h, ts(0), ts(kN - 1), kN);              // exactly the first and last ts
    check(h, kMin, kMax, kN);
    check(h, ts(5), ts(5), 1);                    // start == end on a sample
    check(h, ts(5) + 1, ts(5) + 1, 0);            // start == end between samples
    check(h, ts(119) + 1, ts(120) - 1, 0);        // between two chunks

    auto r = h.query("missing", kMin, kMax);
    assert(r && r->empty());
    assert(!h.query("s", ts(5), ts(5) - 1));
    assert(!h.query("missing", 1, 0));
}

void test_limit() {
    Head h;
    calf::CommitRecord rec;
    rec.new_series.push_back({0, "big"});
    const int n = static_cast<int>(calf::kMaxQuerySamples) + 1;
    for (int i = 0; i < n; ++i) rec.samples.push_back({0, i, 0.0});
    assert(apply(h, rec));

    auto r = h.query("big", 0, n - 2);
    assert(r && r->size() == calf::kMaxQuerySamples);
    assert(!h.query("big", 0, n - 1));
}

// One thread applies batches while 4 threads query. Sample k has ts k * 10
// and value k. Each batch holds kRecs records of kPer samples.
void test_concurrent() {
    constexpr int kRecs = 3;
    constexpr int kPer = 7;
    constexpr int64_t kGap = 10;
    constexpr int64_t kBatchSamples = kRecs * kPer;
    constexpr int64_t kMaxSamples = 3'000'000;

    Head h;
    std::atomic<int64_t> applied{0};  // a hint for the readers only
    std::atomic<bool> stop{false};
    std::atomic<long> queries{0};

    std::thread writer([&] {
        int64_t k = 0;
        while (!stop.load() && k < kMaxSamples) {
            std::vector<calf::CommitRecord> recs(kRecs);
            for (auto& rec : recs) {
                if (k == 0) rec.new_series.push_back({0, "c"});
                for (int i = 0; i < kPer; ++i, ++k) {
                    rec.samples.push_back({0, k * kGap, static_cast<double>(k)});
                }
            }
            // The writer's validation reads Head without the lock while
            // queries run. This read stands in for it.
            assert(k == kBatchSamples || h.find("c") != nullptr);
            std::string err;
            assert(h.apply_batch(recs, err));
            applied.store(k);
        }
    });

    auto reader = [&](unsigned seed) {
        std::mt19937_64 rng(seed);
        while (!stop.load()) {
            const int64_t hint = applied.load();
            const int64_t a = static_cast<int64_t>(rng() % static_cast<uint64_t>(hint + 100)) * kGap;
            const int64_t start = a - static_cast<int64_t>(rng() % 50);
            const int64_t end = start + static_cast<int64_t>(rng() % 5000);
            auto r = h.query("c", start, end);
            assert(r);
            queries.fetch_add(1);
            if (r->empty()) continue;

            // No gaps: the first sample is the first ts >= start, then every
            // step follows. That also means sorted and no duplicates.
            const int64_t first = start <= 0 ? 0 : (start + kGap - 1) / kGap * kGap;
            assert((*r)[0].ts_ms == first);
            for (size_t i = 0; i < r->size(); ++i) {
                const int64_t t = (*r)[i].ts_ms;
                assert(t == first + static_cast<int64_t>(i) * kGap);
                assert(t >= start && t <= end);
                assert((*r)[i].value == static_cast<double>(t / kGap));
            }
            // If the next sample is in range but missing, the result stopped
            // at the end of the applied data, which must be a batch end.
            const int64_t last = r->back().ts_ms;
            if (last + kGap <= end) assert((last / kGap + 1) % kBatchSamples == 0);
        }
    };
    std::vector<std::thread> readers;
    for (unsigned i = 0; i < 4; ++i) readers.emplace_back(reader, i + 1);

    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop.store(true);
    writer.join();
    for (auto& t : readers) t.join();

    // The test checks nothing if the threads did not overlap.
    std::printf("concurrent: %lld samples applied, %ld queries\n",
                static_cast<long long>(applied.load()), queries.load());
    assert(applied.load() > 100 * kBatchSamples && queries.load() > 1000);
}

}  // namespace

int main() {
    test_ranges();
    test_limit();
    test_concurrent();
    std::puts("query_test: all passed");
    return 0;
}
