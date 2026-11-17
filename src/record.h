#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace calf {

inline constexpr uint8_t kRecordCommit = 1;
inline constexpr size_t kRecordHeader = 9;  // type + length + crc

// A valid payload stays below 21 MiB (1 Mi samples x 20 bytes). A larger length
// is damaged.
inline constexpr uint32_t kMaxRecordPayload = 32u * 1024 * 1024;

struct NewSeries {
    uint32_t id;
    std::string name;
    bool operator==(const NewSeries&) const = default;
};

struct RecordSample {
    uint32_t series_id;
    int64_t ts_ms;
    double value;
    bool operator==(const RecordSample&) const = default;
};

struct CommitRecord {
    std::vector<NewSeries> new_series;
    std::vector<RecordSample> samples;
    bool operator==(const CommitRecord&) const = default;
};

enum class Stop { End, Short, BadType, BadLength, BadCrc, BadPayload };

struct DecodeResult {
    std::vector<CommitRecord> records;
    size_t good_offset = 0;
    Stop stop = Stop::End;
};

void encode_commit(const CommitRecord& rec, std::vector<std::byte>& out);

// Stops at the first record that is incomplete or invalid.
DecodeResult decode_records(std::span<const std::byte> bytes);

const char* stop_name(Stop stop);

}  // namespace calf
