// A Release build defines NDEBUG, and then every assert below is removed.
#undef NDEBUG

#include "record.h"

#include <cassert>
#include <cstdio>
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

    std::puts("record_test: all passed");
    return 0;
}
