#include "head.h"
#include "record.h"
#include "server.h"
#include "wal.h"

#include <charconv>
#include <cinttypes>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: server [--max-batch N] [port] [wal_path]\n"
                 "       server --dump wal_path\n");
    return 1;
}

template <typename T>
bool parse_number(const char* s, T& out) {
    const char* end = s + std::strlen(s);
    auto [p, ec] = std::from_chars(s, end, out);
    return ec == std::errc{} && p == end;
}

struct Replayed {
    calf::DecodeResult decoded;
    calf::Head head;
};

// Returns nullopt after it prints the error if a record has a good crc but
// bad content. That is a writer bug, not a torn write, so the caller must not
// truncate: the bytes after it may hold acked records.
std::optional<Replayed> replay(std::span<const std::byte> bytes, const std::string& path) {
    Replayed r{calf::decode_records(bytes), {}};
    const calf::DecodeResult& d = r.decoded;
    if (d.stop == calf::Stop::BadPayload) {
        std::fprintf(stderr, "%s: record %zu at offset %zu: bad payload with a good crc\n",
                     path.c_str(), d.records.size(), d.good_offset);
        return std::nullopt;
    }
    std::string error;
    for (size_t i = 0; i < d.records.size(); ++i) {
        // One record per call, so the error names the bad record.
        if (!r.head.apply_batch({&d.records[i], 1}, error)) {
            std::fprintf(stderr, "%s: record %zu: %s\n", path.c_str(), i, error.c_str());
            return std::nullopt;
        }
    }
    if (d.good_offset < bytes.size()) {
        std::fprintf(stderr, "%s: torn tail: %zu bytes at offset %zu (%s)\n", path.c_str(),
                     bytes.size() - d.good_offset, d.good_offset, calf::stop_name(d.stop));
    }
    return r;
}

int dump(const std::string& path) {
    std::optional<std::vector<std::byte>> bytes = calf::read_file(path);
    if (!bytes) return 1;
    std::optional<Replayed> r = replay(*bytes, path);
    if (!r) return 1;

    // replay() checked that IDs are dense from 0, so the index is the ID.
    std::vector<std::string> names;
    const auto& records = r->decoded.records;
    for (size_t i = 0; i < records.size(); ++i) {
        for (const calf::NewSeries& s : records[i].new_series) names.push_back(s.name);
        for (const calf::RecordSample& s : records[i].samples) {
            std::printf("%zu %s %" PRId64 " %016" PRIx64 "\n", i, names[s.series_id].c_str(),
                        s.ts_ms, std::bit_cast<uint64_t>(s.value));
        }
    }
    return std::fflush(stdout) == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    // SIGPIPE kills the process by default. With SIG_IGN, send() to a closed
    // peer returns EPIPE and only that connection ends.
    std::signal(SIGPIPE, SIG_IGN);

    if (argc >= 2 && std::strcmp(argv[1], "--dump") == 0) {
        if (argc != 3) return usage();
        return dump(argv[2]);
    }

    uint16_t port = 7070;
    std::string wal_path = "calf.wal";
    size_t max_batch = 0;

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--max-batch") == 0) {
            if (++i == argc || !parse_number(argv[i], max_batch)) return usage();
        } else if (positional == 0) {
            if (!parse_number(argv[i], port) || port == 0) return usage();
            ++positional;
        } else if (positional == 1) {
            wal_path = argv[i];
            ++positional;
        } else {
            return usage();
        }
    }

    std::optional<calf::Wal> wal = calf::Wal::open(wal_path);
    if (!wal) return 1;
    std::optional<std::vector<std::byte>> bytes = wal->read_all();
    if (!bytes) {
        std::perror(wal_path.c_str());
        return 1;
    }
    std::optional<Replayed> r = replay(*bytes, wal_path);
    if (!r) return 1;

    // Sync the cut before the first append, so the file on disk matches the
    // state the writer starts from.
    const size_t good = r->decoded.good_offset;
    if (good < bytes->size()) {
        if (!wal->truncate_to(good)) {
            std::perror("truncate WAL");
            return 1;
        }
        std::fprintf(stderr, "%s: truncated to %zu bytes\n", wal_path.c_str(), good);
    }
    const uint32_t next_id = r->head.next_id();
    std::fprintf(stderr, "%s: replayed %zu records, %u series\n", wal_path.c_str(),
                 r->decoded.records.size(), next_id);

    return calf::run_server(port, std::move(*wal), std::move(r->head), next_id, max_batch);
}
