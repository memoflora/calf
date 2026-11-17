#include "head.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>

namespace calf {

namespace {

// Chunks in memory come only from Chunk::append, so an error here is a bug
// in the encoder or the decoder.
void decode_into(std::span<const std::byte> bytes, std::vector<Sample>& out) {
    ChunkIterator it(bytes);
    while (it.next()) out.push_back({it.ts(), it.value()});
    if (it.error()) {
        std::fprintf(stderr, "chunk decode error\n");
        std::abort();
    }
}

}  // namespace

Head::Head(Head&& other)
    : series_(std::move(other.series_)), names_(std::move(other.names_)) {}

const SeriesData* Head::find(const std::string& name) const {
    auto it = series_.find(name);
    return it == series_.end() ? nullptr : &it->second;
}

std::vector<Sample> Head::samples(const std::string& name) const {
    std::shared_lock lock(mu_);
    std::vector<Sample> out;
    const SeriesData* d = find(name);
    if (!d) return out;
    for (const Chunk& c : d->full) decode_into(c.bytes(), out);
    decode_into(d->open.bytes(), out);
    return out;
}

std::optional<std::vector<Sample>> Head::query(const std::string& name, int64_t start,
                                               int64_t end) const {
    if (start > end) return std::nullopt;

    // Decode after the unlock: a slow query would block the writer, and every
    // ack waits for the writer.
    std::vector<std::vector<std::byte>> copies;
    {
        std::shared_lock lock(mu_);
        const SeriesData* d = find(name);
        if (!d) return std::vector<Sample>{};
        for (const Chunk& c : d->full) {
            if (c.min_t() > end) break;  // full chunks are sorted by time
            if (c.max_t() >= start) copies.emplace_back(c.bytes().begin(), c.bytes().end());
        }
        // Copy under the lock: the writer appends to the open chunk. min_t and
        // max_t mean nothing when count is 0.
        const Chunk& o = d->open;
        if (o.count() > 0 && o.min_t() <= end && o.max_t() >= start) {
            copies.emplace_back(o.bytes().begin(), o.bytes().end());
        }
    }

    std::vector<Sample> all;
    std::vector<Sample> out;
    for (const auto& bytes : copies) {
        all.clear();
        decode_into(bytes, all);
        for (const Sample& s : all) {
            if (s.ts_ms < start || s.ts_ms > end) continue;
            if (out.size() == kMaxQuerySamples) return std::nullopt;
            out.push_back(s);
        }
    }
    return out;
}

bool Head::apply(const CommitRecord& rec, std::string& error) {
    // Check everything first and change Head only after that, so a bad
    // record leaves no partial state behind.
    if (rec.samples.empty()) {
        error = "record has no samples";
        return false;
    }

    // New series k must have ID old + k, so an ID at or above old maps to
    // rec.new_series[id - old].
    const size_t old = names_.size();
    const size_t total = old + rec.new_series.size();
    std::unordered_set<std::string> new_names;
    for (size_t k = 0; k < rec.new_series.size(); ++k) {
        const NewSeries& s = rec.new_series[k];
        if (s.id != old + k) {
            error = "series id " + std::to_string(s.id) + " is not the next id " +
                    std::to_string(old + k);
            return false;
        }
        if (series_.contains(s.name) || !new_names.insert(s.name).second) {
            error = "series name '" + s.name + "' already exists";
            return false;
        }
    }

    std::unordered_map<uint32_t, int64_t> last;
    for (const RecordSample& s : rec.samples) {
        if (s.series_id >= total) {
            error = "unknown series id " + std::to_string(s.series_id);
            return false;
        }
        auto it = last.find(s.series_id);
        if (it == last.end() && s.series_id < old) {
            const SeriesData& d = series_.at(names_[s.series_id]);
            it = last.emplace(s.series_id, d.last_ts).first;
        }
        if (it != last.end() && s.ts_ms <= it->second) {
            error = "timestamp " + std::to_string(s.ts_ms) + " is not increasing";
            return false;
        }
        last[s.series_id] = s.ts_ms;
    }

    // The writer never produces a new series with no samples, so such a
    // record is damaged. last_ts of that series would also be invalid.
    for (const NewSeries& s : rec.new_series) {
        if (!last.contains(s.id)) {
            error = "new series '" + s.name + "' has no samples";
            return false;
        }
    }

    // The IDs were checked to be old, old + 1, ..., so push_back puts each
    // name at its ID.
    for (const NewSeries& s : rec.new_series) {
        series_.emplace(s.name, SeriesData{s.id, {}, Chunk(), 0});
        names_.push_back(s.name);
    }
    // An empty chunk accepts any ts, so the cut below depends on the check
    // above that every ts is greater than the last one of its series.
    for (const RecordSample& s : rec.samples) {
        SeriesData& d = series_.at(names_[s.series_id]);
        if (!d.open.fits(s.ts_ms)) {
            d.full.push_back(std::move(d.open));
            d.open = Chunk();
        }
        d.open.append(s.ts_ms, s.value);
        d.last_ts = s.ts_ms;
    }
    return true;
}

bool Head::apply_batch(std::span<const CommitRecord> recs, std::string& error) {
    // One lock for the whole batch, so a query sees all of it or none of it.
    std::unique_lock lock(mu_);
    for (const CommitRecord& rec : recs) {
        if (!apply(rec, error)) return false;
    }
    return true;
}

}  // namespace calf
