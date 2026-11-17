#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace calf {

inline constexpr uint8_t kRecordCommit = 1;
inline constexpr size_t kRecordHeader = 9;  // type + length + crc

// A 16 MiB frame holds at most about 1 Mi samples at 16 bytes each. A WAL
// sample is 20 bytes, so a valid payload stays below about 21 MiB. Anything
// larger is a damaged length field.
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

// Appends one COMMIT record to out.
void encode_commit(const CommitRecord& rec, std::vector<std::byte>& out);

// Decodes records from the start of bytes until the first one that is not
// complete and valid.
DecodeResult decode_records(std::span<const std::byte> bytes);

const char* stop_name(Stop stop);

}  // namespace calf
