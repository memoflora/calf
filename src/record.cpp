#include "record.h"

#include "protocol.h"

#include <bit>
#include <optional>
#include <span>
#include <zlib.h>

namespace calf {

namespace {

void store_u32(std::span<std::byte> b, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
}

uint32_t record_crc(std::span<const std::byte> header, std::span<const std::byte> payload) {
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(header.data()), 5);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(payload.data()),
                static_cast<uInt>(payload.size()));
    return static_cast<uint32_t>(crc);
}

// Every take() checks the bounds first, because the counts inside a payload
// come from the file and can point past its end.
class Reader {
public:
    explicit Reader(std::span<const std::byte> b) : b_(b) {}

    std::optional<std::span<const std::byte>> take(size_t n) {
        if (b_.size() - pos_ < n) return std::nullopt;
        auto s = b_.subspan(pos_, n);
        pos_ += n;
        return s;
    }
    bool done() const { return pos_ == b_.size(); }

private:
    std::span<const std::byte> b_;
    size_t pos_ = 0;
};

std::optional<CommitRecord> parse_payload(std::span<const std::byte> payload) {
    Reader r(payload);
    CommitRecord rec;

    auto count = r.take(2);
    if (!count) return std::nullopt;
    for (uint16_t i = 0, n = get_u16(*count); i < n; ++i) {
        auto head = r.take(6);
        if (!head) return std::nullopt;
        auto name = r.take(get_u16(head->subspan(4)));
        if (!name) return std::nullopt;
        rec.new_series.push_back(
            {get_u32(*head), std::string(reinterpret_cast<const char*>(name->data()), name->size())});
    }

    auto samples = r.take(4);
    if (!samples) return std::nullopt;
    for (uint32_t i = 0, n = get_u32(*samples); i < n; ++i) {
        auto s = r.take(20);
        if (!s) return std::nullopt;
        rec.samples.push_back({get_u32(*s), static_cast<int64_t>(get_u64(s->subspan(4))),
                               std::bit_cast<double>(get_u64(s->subspan(12)))});
    }

    if (!r.done()) return std::nullopt;
    return rec;
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
    store_u32(rec_bytes.subspan(5, 4),
              record_crc(rec_bytes, rec_bytes.subspan(kRecordHeader)));
}

DecodeResult decode_records(std::span<const std::byte> bytes) {
    DecodeResult res;
    for (;;) {
        std::span<const std::byte> rest = bytes.subspan(res.good_offset);
        if (rest.empty()) {
            res.stop = Stop::End;
            return res;
        }
        if (rest.size() < kRecordHeader) {
            res.stop = Stop::Short;
            return res;
        }
        if (std::to_integer<uint8_t>(rest[0]) != kRecordCommit) {
            res.stop = Stop::BadType;
            return res;
        }
        // Checked before the payload is used: a damaged length can be up to
        // 4 GiB.
        uint32_t len = get_u32(rest.subspan(1));
        if (len > kMaxRecordPayload) {
            res.stop = Stop::BadLength;
            return res;
        }
        if (rest.size() - kRecordHeader < len) {
            res.stop = Stop::Short;
            return res;
        }
        std::span<const std::byte> payload = rest.subspan(kRecordHeader, len);
        if (get_u32(rest.subspan(5)) != record_crc(rest, payload)) {
            res.stop = Stop::BadCrc;
            return res;
        }
        // A good crc means the writer wrote these bytes. If they do not
        // parse, the writer has a bug; this is not a torn write.
        std::optional<CommitRecord> rec = parse_payload(payload);
        if (!rec) {
            res.stop = Stop::BadPayload;
            return res;
        }
        res.records.push_back(std::move(*rec));
        res.good_offset += kRecordHeader + len;
    }
}

const char* stop_name(Stop stop) {
    switch (stop) {
        case Stop::End: return "end";
        case Stop::Short: return "short record";
        case Stop::BadType: return "bad type";
        case Stop::BadLength: return "bad length";
        case Stop::BadCrc: return "bad crc";
        case Stop::BadPayload: return "bad payload";
    }
    return "unknown";
}

}  // namespace calf
