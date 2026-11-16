#include "record.h"

#include "protocol.h"

#include <bit>
#include <span>
#include <zlib.h>

namespace calf {

namespace {

void store_u32(std::span<std::byte> b, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
}

}  // namespace

void encode_commit(const CommitRecord& rec, std::vector<std::byte>& out) {
    const size_t start = out.size();
    out.push_back(std::byte{kRecordCommit});
    put_u32(out, 0);  // length, filled in below
    put_u32(out, 0);  // crc, filled in below
    const size_t payload_start = out.size();

    // new_series.size() fits u16: a request has at most 65535 series entries.
    put_u16(out, static_cast<uint16_t>(rec.new_series.size()));
    for (const NewSeries& s : rec.new_series) {
        put_u32(out, s.id);
        put_u16(out, static_cast<uint16_t>(s.name.size()));
        for (char c : s.name) out.push_back(static_cast<std::byte>(c));
    }
    put_u32(out, static_cast<uint32_t>(rec.samples.size()));
    for (const RecordSample& s : rec.samples) {
        put_u32(out, s.series_id);
        put_u64(out, static_cast<uint64_t>(s.ts_ms));
        put_u64(out, std::bit_cast<uint64_t>(s.value));
    }

    // Take the span after all push_backs: they can reallocate out.
    std::span<std::byte> rec_bytes = std::span(out).subspan(start);
    const size_t payload_len = out.size() - payload_start;
    store_u32(rec_bytes.subspan(1, 4), static_cast<uint32_t>(payload_len));

    // The crc covers type + length, then the payload, and skips its own field.
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(rec_bytes.data()), 5);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(rec_bytes.data() + 9),
                static_cast<uInt>(payload_len));
    store_u32(rec_bytes.subspan(5, 4), static_cast<uint32_t>(crc));
}

}  // namespace calf
