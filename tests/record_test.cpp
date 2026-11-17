// A Release build defines NDEBUG, and then every assert below is removed.
#undef NDEBUG

#include "head.h"
#include "record.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <zlib.h>

namespace {

// Written out from design.md with Python's struct and zlib.crc32, not with
// encode_commit, so a shared bug cannot make both sides agree.
const std::vector<unsigned char> kExpected = {
    0x01,                    // type COMMIT
    0x37, 0x00, 0x00, 0x00,  // payload length 55
    0x95, 0x0e, 0x48, 0x57,  // crc32 0x57480e95
    0x01, 0x00,              // new_series_count 1
    0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x63, 0x70, 0x75,  // id 0, "cpu"
    0x02, 0x00, 0x00, 0x00,  // sample_count 2
    0x00, 0x00, 0x00, 0x00,  // series 0
    0xe8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ts 1000
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x3f,  // 1.5
    0x07, 0x00, 0x00, 0x00,  // series 7
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  // ts -1
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0,  // -2.0
};

calf::CommitRecord sample_record() {
    return {{{0, "cpu"}}, {{0, 1000, 1.5}, {7, -1, -2.0}}};
}

bool equals(const std::vector<std::byte>& got, size_t offset) {
    if (got.size() - offset != kExpected.size()) return false;
    for (size_t i = 0; i < kExpected.size(); ++i) {
        if (std::to_integer<unsigned char>(got[offset + i]) != kExpected[i]) return false;
    }
    return true;
}

// About 50 records of different sizes. Record i creates series i, and from
// record 3 on also adds samples to older series.
std::vector<calf::CommitRecord> many_records() {
    std::vector<calf::CommitRecord> recs;
    for (uint32_t i = 0; i < 50; ++i) {
        calf::CommitRecord r;
        r.new_series.push_back({i, "series_" + std::string(i % 7, 'x') + std::to_string(i)});
        for (uint32_t j = 0; j <= i % 5; ++j) {
            r.samples.push_back({i, 1000 + j, static_cast<double>(i * 10 + j)});
        }
        if (i >= 3) r.samples.push_back({i - 3, 2000 + static_cast<int64_t>(i), -1.0 * i});
        recs.push_back(std::move(r));
    }
    return recs;
}

std::vector<calf::CommitRecord> prefix(const std::vector<calf::CommitRecord>& v, size_t n) {
    return {v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n)};
}

void test_round_trip() {
    std::vector<std::byte> bytes;
    calf::encode_commit(sample_record(), bytes);
    calf::CommitRecord empty_names{{{3, ""}, {4, "a"}}, {{3, 5, 0.0}}};
    calf::encode_commit(empty_names, bytes);

    calf::DecodeResult res = calf::decode_records(bytes);
    assert(res.stop == calf::Stop::End);
    assert(res.good_offset == bytes.size());
    assert(res.records.size() == 2);
    assert(res.records[0] == sample_record());
    assert(res.records[1] == empty_names);

    assert(calf::decode_records({}).stop == calf::Stop::End);
}

void test_truncation() {
    auto recs = many_records();
    std::vector<std::byte> bytes;
    std::vector<size_t> ends;
    for (const auto& r : recs) {
        calf::encode_commit(r, bytes);
        ends.push_back(bytes.size());
    }

    for (size_t cut = 0; cut <= bytes.size(); ++cut) {
        size_t complete = 0;
        while (complete < ends.size() && ends[complete] <= cut) ++complete;
        size_t good = complete == 0 ? 0 : ends[complete - 1];

        calf::DecodeResult res = calf::decode_records(std::span(bytes).first(cut));
        assert(res.records == prefix(recs, complete));
        assert(res.good_offset == good);
        assert(res.stop == (cut == good ? calf::Stop::End : calf::Stop::Short));
    }
}

void test_corruption() {
    auto recs = many_records();
    std::vector<std::byte> bytes;
    std::vector<size_t> starts;
    for (const auto& r : recs) {
        starts.push_back(bytes.size());
        calf::encode_commit(r, bytes);
    }
    starts.push_back(bytes.size());

    for (size_t i = 0; i < recs.size(); ++i) {
        for (size_t pos = starts[i]; pos < starts[i + 1]; ++pos) {
            std::vector<std::byte> copy = bytes;
            copy[pos] ^= std::byte{0xFF};
            calf::DecodeResult res = calf::decode_records(copy);
            assert(res.records == prefix(recs, i));
            assert(res.good_offset == starts[i]);
            assert(res.stop != calf::Stop::End && res.stop != calf::Stop::BadPayload);
        }
    }
}

void test_bad_type_and_length() {
    std::vector<std::byte> bytes;
    calf::encode_commit(sample_record(), bytes);
    std::vector<std::byte> zeros(bytes.size(), std::byte{0});

    // A tail of zeros, for example from a filesystem that extended the file.
    std::vector<std::byte> tail = bytes;
    tail.insert(tail.end(), zeros.begin(), zeros.end());
    calf::DecodeResult res = calf::decode_records(tail);
    assert(res.records.size() == 1 && res.stop == calf::Stop::BadType);

    std::vector<std::byte> type2 = bytes;
    type2[0] = std::byte{2};
    assert(calf::decode_records(type2).stop == calf::Stop::BadType);

    std::vector<std::byte> huge = bytes;
    huge[4] = std::byte{0xFF};  // length byte 3: length becomes about 4 GiB
    assert(calf::decode_records(huge).stop == calf::Stop::BadLength);
}

// Patches the length and crc of the one record in bytes after a payload
// change, so the crc is valid. encode_commit cannot make these records.
void fix_header(std::vector<std::byte>& bytes) {
    uint32_t len = static_cast<uint32_t>(bytes.size() - 9);
    for (int i = 0; i < 4; ++i) bytes[1 + i] = static_cast<std::byte>((len >> (8 * i)) & 0xFF);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(bytes.data()), 5);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(bytes.data() + 9), len);
    for (int i = 0; i < 4; ++i) bytes[5 + i] = static_cast<std::byte>((crc >> (8 * i)) & 0xFF);
}

void expect_bad_payload(const std::vector<std::byte>& bytes) {
    calf::DecodeResult res = calf::decode_records(bytes);
    assert(res.stop == calf::Stop::BadPayload);
    assert(res.records.empty() && res.good_offset == 0);
}

void test_bad_payload() {
    std::vector<std::byte> good;
    calf::encode_commit({{}, {{0, 1, 1.0}}}, good);

    // sample_count says 2, but only 1 sample follows.
    std::vector<std::byte> missing = good;
    missing[11] = std::byte{2};  // payload starts at 9: u16 count, then u32 samples
    fix_header(missing);
    expect_bad_payload(missing);

    // One byte after the last sample.
    std::vector<std::byte> extra = good;
    extra.push_back(std::byte{0});
    fix_header(extra);
    expect_bad_payload(extra);

    // name_len points past the end of the payload.
    std::vector<std::byte> name;
    calf::encode_commit({{{0, "ab"}}, {{0, 1, 1.0}}}, name);
    name[15] = std::byte{0xFF};  // 9 + u16 count + u32 id = 15: name_len
    fix_header(name);
    expect_bad_payload(name);
}

// Head with series "a" (id 0) holding ts 10.
calf::Head head_with_a() {
    calf::Head h;
    std::string err;
    assert(h.apply({{{0, "a"}}, {{0, 10, 1.0}}}, err));
    return h;
}

void expect_bad(const calf::CommitRecord& rec) {
    calf::Head h = head_with_a();
    std::string err;
    assert(!h.apply(rec, err));
    assert(!err.empty());
    // Head is unchanged: the next valid record still applies.
    assert(h.next_id() == 1);
    assert(h.find("a")->samples.size() == 1);
    assert(h.apply({{{1, "b"}}, {{0, 11, 0.0}, {1, 1, 0.0}}}, err));
}

void test_bad_content() {
    expect_bad({{{2, "b"}}, {{2, 1, 0.0}}});                     // id skips 1
    expect_bad({{{1, "a"}}, {{1, 11, 0.0}}});                    // name exists
    expect_bad({{{1, "b"}, {2, "b"}}, {{1, 1, 0.0}, {2, 1, 0.0}}});  // name twice
    expect_bad({{}, {{5, 11, 0.0}}});                            // unknown id
    expect_bad({{}, {{0, 10, 0.0}}});                            // ts equal
    expect_bad({{}, {{0, 9, 0.0}}});                             // ts lower
    expect_bad({{{1, "b"}}, {{1, 5, 0.0}, {1, 5, 0.0}}});        // ts equal in record
    expect_bad({{{1, "b"}}, {}});                                // no samples
    expect_bad({{{1, "b"}, {2, "c"}}, {{1, 5, 0.0}}});           // new series, no samples
    // A bad sample after a good one must not leave the good one applied.
    expect_bad({{}, {{0, 11, 0.0}, {0, 11, 0.0}}});

    calf::Head h = head_with_a();
    std::string err;
    assert(h.apply({{{1, "b"}}, {{1, 1, 0.0}, {0, 20, 0.0}, {1, 2, 0.0}}}, err));
    assert(h.next_id() == 2);
    assert(h.find("a")->samples.back().ts_ms == 20);
    assert(h.find("b")->id == 1 && h.find("b")->samples.size() == 2);
}

}  // namespace

int main() {
    // Pins the algorithm: the standard CRC-32 check value.
    const char* check = "123456789";
    assert(crc32(0L, reinterpret_cast<const Bytef*>(check), 9) == 0xCBF43926);

    std::vector<std::byte> out;
    calf::encode_commit(sample_record(), out);
    assert(equals(out, 0));

    // encode_commit appends. A record after other bytes must be the same.
    std::vector<std::byte> appended(3, std::byte{0xAA});
    calf::encode_commit(sample_record(), appended);
    assert(appended[0] == std::byte{0xAA} && appended[2] == std::byte{0xAA});
    assert(equals(appended, 3));

    test_round_trip();
    test_truncation();
    test_corruption();
    test_bad_type_and_length();
    test_bad_payload();
    test_bad_content();

    std::puts("record_test: all passed");
    return 0;
}
