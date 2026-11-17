#pragma once

#include "protocol.h"
#include "record.h"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace calf {

struct SeriesData {
    uint32_t id;
    std::vector<Sample> samples;
};

// Only the writer thread uses Head, so it has no lock.
class Head {
public:
    // nullptr if the series does not exist.
    const SeriesData* find(const std::string& name) const;

    // Creates the series with id if it does not exist yet.
    void append(const std::string& name, uint32_t id, std::span<const Sample> samples);

    // Replays one WAL record. Returns false, with Head unchanged, if the
    // record holds content the writer cannot produce.
    bool apply(const CommitRecord& rec, std::string& error);

    // IDs are dense from 0, so the next ID is the number of series.
    uint32_t next_id() const { return static_cast<uint32_t>(names_.size()); }

private:
    std::unordered_map<std::string, SeriesData> series_;
    std::vector<std::string> names_;  // index is the series ID
};

}  // namespace calf
