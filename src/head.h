#pragma once

#include "protocol.h"

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

private:
    std::unordered_map<std::string, SeriesData> series_;
};

}  // namespace calf
