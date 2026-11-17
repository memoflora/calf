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

struct SeriesData {
    uint32_t id;
    std::vector<Chunk> full;  // never change after the cut
    Chunk open;
    int64_t last_ts = 0;  // valid once the series has a sample
};

// Only apply_batch writes, under the unique lock. Queries take the shared lock.
// The writer thread reads with no lock: it is the only writer, so there is no
// race.
class Head {
public:
    Head() = default;

    // shared_mutex cannot move. Safe only at startup, before other threads see
    // either Head.
    Head(Head&& other);
    Head& operator=(Head&&) = delete;

    // Writer thread only: takes no lock.
    const SeriesData* find(const std::string& name) const;

    std::vector<Sample> samples(const std::string& name) const;

    // nullopt means an ERROR reply.
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
