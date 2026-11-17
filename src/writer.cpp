#include "writer.h"

#include "record.h"

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <optional>

namespace calf {

Writer::Writer(Wal wal, Head head, uint32_t next_id, size_t max_batch)
    : wal_(std::move(wal)),
      head_(std::move(head)),
      max_batch_(max_batch),
      next_id_(next_id),
      thread_([this] { run(); }) {}

std::future<Status> Writer::submit(WriteRequest req) {
    Pending p{std::move(req), {}};
    std::future<Status> fut = p.done.get_future();
    {
        std::lock_guard lock(mu_);
        queue_.push_back(std::move(p));
    }
    // Notify after the unlock, so the writer does not wake up only to block
    // on mu_ that this thread still holds.
    cv_.notify_one();
    return fut;
}

void Writer::run() {
    for (;;) {
        std::vector<Pending> batch;
        {
            std::unique_lock lock(mu_);
            // The predicate is checked again after every wakeup, so a spurious
            // wakeup with an empty queue goes back to sleep. It is also checked
            // before the first wait, so a notify sent while the writer was busy
            // is not lost.
            cv_.wait(lock, [this] { return !queue_.empty(); });
            if (max_batch_ == 0 || queue_.size() <= max_batch_) {
                batch.swap(queue_);
            } else {
                auto end = queue_.begin() + static_cast<std::ptrdiff_t>(max_batch_);
                batch.assign(std::make_move_iterator(queue_.begin()),
                             std::make_move_iterator(end));
                queue_.erase(queue_.begin(), end);
            }
        }
        // The lock is released here: connection threads can queue new
        // requests during the write and the sync. They go into the next batch.
        process(batch);
    }
}

void Writer::process(std::vector<Pending>& batch) {
    BatchState state;
    std::vector<std::byte> records;
    std::vector<Status> status;
    status.reserve(batch.size());
    for (Pending& p : batch) {
        status.push_back(accept(p.req, state, records) ? Status::ok : Status::rejected);
    }

    if (!records.empty()) {
        // After a failed write or sync, the file state is unknown: a retry of
        // fsync can report success for data that the kernel already dropped.
        // Stop at once and let recovery read the WAL.
        if (!wal_.append(records)) {
            std::perror("WAL write");
            std::abort();
        }
        if (!wal_.sync()) {
            std::perror("WAL sync");
            std::abort();
        }
    }

    // Apply only after the sync, so the store never holds a write that a
    // crash could undo. Reads from the store come in a later step.
    for (size_t i = 0; i < batch.size(); ++i) {
        if (status[i] != Status::ok) continue;
        for (const Series& s : batch[i].req.series) {
            if (s.samples.empty()) continue;
            head_.append(s.name, series_id(s.name, state), s.samples);
        }
    }

    for (size_t i = 0; i < batch.size(); ++i) {
        batch[i].done.set_value(status[i]);
    }
}

// Checks req against the store and the earlier accepted requests in this
// batch. If it is accepted, assigns IDs, updates state, and appends one
// record to out.
bool Writer::accept(const WriteRequest& req, BatchState& state, std::vector<std::byte>& out) {
    // Last timestamp per series inside this request. state is changed only
    // after the whole request passes, so a rejection leaves no trace.
    std::unordered_map<std::string, int64_t> local;
    for (const Series& s : req.series) {
        if (s.samples.empty()) continue;
        if (s.name.empty()) return false;

        std::optional<int64_t> last;
        if (auto it = local.find(s.name); it != local.end()) {
            last = it->second;
        } else if (auto it = state.last_ts.find(s.name); it != state.last_ts.end()) {
            last = it->second;
        } else if (const SeriesData* d = head_.find(s.name)) {
            last = d->samples.back().ts_ms;
        }
        for (const Sample& smp : s.samples) {
            if (last && smp.ts_ms <= *last) return false;
            last = smp.ts_ms;
        }
        local[s.name] = *last;
    }
    if (local.empty()) return true;

    CommitRecord rec;
    for (const Series& s : req.series) {
        if (s.samples.empty()) continue;
        if (!head_.find(s.name) && !state.new_ids.contains(s.name)) {
            state.new_ids[s.name] = next_id_;
            rec.new_series.push_back({next_id_, s.name});
            ++next_id_;
        }
        uint32_t id = series_id(s.name, state);
        for (const Sample& smp : s.samples) {
            rec.samples.push_back({id, smp.ts_ms, smp.value});
        }
    }
    for (auto& [name, ts] : local) state.last_ts[name] = ts;
    encode_commit(rec, out);
    return true;
}

uint32_t Writer::series_id(const std::string& name, const BatchState& state) const {
    if (const SeriesData* d = head_.find(name)) return d->id;
    return state.new_ids.at(name);
}

}  // namespace calf
