#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace calf {

inline constexpr uint8_t kRecordCommit = 1;

struct NewSeries {
    uint32_t id;
    std::string name;
};

struct RecordSample {
    uint32_t series_id;
    int64_t ts_ms;
    double value;
};

struct CommitRecord {
    std::vector<NewSeries> new_series;
    std::vector<RecordSample> samples;
};

// Appends one COMMIT record to out.
void encode_commit(const CommitRecord& rec, std::vector<std::byte>& out);

}  // namespace calf
