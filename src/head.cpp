#include "head.h"

namespace calf {

const SeriesData* Head::find(const std::string& name) const {
    auto it = series_.find(name);
    return it == series_.end() ? nullptr : &it->second;
}

void Head::append(const std::string& name, uint32_t id, std::span<const Sample> samples) {
    auto it = series_.try_emplace(name, SeriesData{id, {}}).first;
    it->second.samples.insert(it->second.samples.end(), samples.begin(), samples.end());
}

}  // namespace calf
