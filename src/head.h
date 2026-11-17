#pragma once

#include "chunk/xor_chunk.h"
#include "protocol.h"
#include "record.h"

#include <cstdint>
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

// Only the writer thread uses Head, so it has no lock.
class Head {
public:
    // nullptr if the series does not exist.
    const SeriesData* find(const std::string& name) const;

    // All samples of the series in order. Empty if it does not exist.
    std::vector<Sample> samples(const std::string& name) const;

    // Replays one WAL record. Returns false, with Head unchanged, if the
    // record holds content the writer cannot produce.
    bool apply(const CommitRecord& rec, std::string& error);

    // Applies recs in order and stops at the first bad one. The records
    // before it stay applied.
    bool apply_batch(std::span<const CommitRecord> recs, std::string& error);

    // IDs are dense from 0, so the next ID is the number of series.
    uint32_t next_id() const { return static_cast<uint32_t>(names_.size()); }

private:
    std::unordered_map<std::string, SeriesData> series_;
    std::vector<std::string> names_;  // index is the series ID
};

}  // namespace calf
