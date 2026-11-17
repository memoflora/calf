#pragma once

#include "chunk/xor_chunk.h"
#include "protocol.h"
#include "record.h"

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace calf {

// The series name is the key in Head's map.
struct SeriesData {
    uint32_t id;
    std::vector<Chunk> full;  // never change after the cut
    Chunk open;
    int64_t last_ts = 0;  // valid once the series has a sample
};

// Lock rule: apply_batch, the only function that changes Head, holds the
// unique lock. Query threads hold the shared lock. The writer thread reads
// without the lock: a data race needs a write on another thread, and the
// writer is the only thread that writes.
class Head {
public:
    Head() = default;

    // shared_mutex cannot move, so the default move is deleted. This moves
    // the data and gives the new Head its own mutex. It is safe only before
    // other threads can see either Head: main moves it at startup.
    Head(Head&& other);
    Head& operator=(Head&&) = delete;

    // Writer thread only, because it takes no lock. nullptr if the series
    // does not exist.
    const SeriesData* find(const std::string& name) const;

    // All samples of the series in order. Empty if it does not exist.
    std::vector<Sample> samples(const std::string& name) const;

    // Samples with start <= ts <= end. nullopt means ERROR: start > end, or
    // more than kMaxQuerySamples samples. An unknown series gives 0 samples.
    std::optional<std::vector<Sample>> query(const std::string& name, int64_t start,
                                             int64_t end) const;

    // Applies recs in order and stops at the first bad one. The records
    // before it stay applied, and a bad record leaves no partial state.
    bool apply_batch(std::span<const CommitRecord> recs, std::string& error);

    // IDs are dense from 0, so the next ID is the number of series.
    uint32_t next_id() const { return static_cast<uint32_t>(names_.size()); }

private:
    bool apply(const CommitRecord& rec, std::string& error);

    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, SeriesData> series_;
    std::vector<std::string> names_;  // index is the series ID
};

}  // namespace calf
