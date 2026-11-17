#include "head.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_set>

namespace calf {

const SeriesData* Head::find(const std::string& name) const {
    auto it = series_.find(name);
    return it == series_.end() ? nullptr : &it->second;
}

std::vector<Sample> Head::samples(const std::string& name) const {
    std::vector<Sample> out;
    const SeriesData* d = find(name);
    if (!d) return out;
    auto decode = [&out](const Chunk& c) {
        ChunkIterator it(c.bytes());
        while (it.next()) out.push_back({it.ts(), it.value()});
        // Chunks in memory come only from Chunk::append, so an error here
        // is a bug in the encoder or the decoder.
        if (it.error()) {
            std::fprintf(stderr, "chunk decode error\n");
            std::abort();
        }
    };
    for (const Chunk& c : d->full) decode(c);
    decode(d->open);
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
    for (const CommitRecord& rec : recs) {
        if (!apply(rec, error)) return false;
    }
    return true;
}

}  // namespace calf
