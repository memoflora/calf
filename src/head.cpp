#include "head.h"

#include <unordered_set>

namespace calf {

const SeriesData* Head::find(const std::string& name) const {
    auto it = series_.find(name);
    return it == series_.end() ? nullptr : &it->second;
}

void Head::append(const std::string& name, uint32_t id, std::span<const Sample> samples) {
    auto [it, created] = series_.try_emplace(name, SeriesData{id, {}});
    if (created) {
        if (names_.size() <= id) names_.resize(id + 1);
        names_[id] = name;
    }
    it->second.samples.insert(it->second.samples.end(), samples.begin(), samples.end());
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
            it = last.emplace(s.series_id, d.samples.back().ts_ms).first;
        }
        if (it != last.end() && s.ts_ms <= it->second) {
            error = "timestamp " + std::to_string(s.ts_ms) + " is not increasing";
            return false;
        }
        last[s.series_id] = s.ts_ms;
    }

    // The writer finds the last timestamp with samples.back(), so a series
    // with no samples would be undefined behavior there.
    for (const NewSeries& s : rec.new_series) {
        if (!last.contains(s.id)) {
            error = "new series '" + s.name + "' has no samples";
            return false;
        }
    }

    for (const RecordSample& s : rec.samples) {
        const std::string& name = s.series_id < old ? names_[s.series_id]
                                                    : rec.new_series[s.series_id - old].name;
        Sample smp{s.ts_ms, s.value};
        append(name, s.series_id, std::span(&smp, 1));
    }
    return true;
}

}  // namespace calf
