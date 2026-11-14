#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace calf {

inline constexpr uint32_t kMaxFrame = 16u * 1024 * 1024;
inline constexpr uint8_t kMsgWrite = 1;
inline constexpr uint8_t kStatusOk = 0;
inline constexpr uint8_t kStatusError = 1;

// No bounds check: b must hold at least 2, 4, or 8 bytes.
uint16_t get_u16(std::span<const std::byte> b);
uint32_t get_u32(std::span<const std::byte> b);
uint64_t get_u64(std::span<const std::byte> b);

void put_u16(std::vector<std::byte>& out, uint16_t v);
void put_u32(std::vector<std::byte>& out, uint32_t v);
void put_u64(std::vector<std::byte>& out, uint64_t v);

bool read_exact(int fd, std::span<std::byte> buf);
bool write_exact(int fd, std::span<const std::byte> buf);

struct Sample {
    int64_t ts_ms;
    double value;
};

struct Series {
    std::string name;
    std::vector<Sample> samples;
};

struct WriteRequest {
    std::vector<Series> series;
};

// payload excludes the u32 length. Returns nullopt if it is malformed.
std::optional<WriteRequest> parse_request(std::span<const std::byte> payload);

}  // namespace calf
