#pragma once

#include "head.h"
#include "protocol.h"
#include "record.h"
#include "wal.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace calf {

enum class Status : uint8_t { ok = kStatusOk, rejected = kStatusError };

// The writer thread runs forever, so a Writer must never be destroyed:
// destroying a joinable std::thread calls std::terminate.
class Writer {
public:
    // head and next_id come from replay. head must outlive the Writer.
    // max_batch 0 means no limit.
    Writer(Wal wal, Head& head, uint32_t next_id, size_t max_batch);

    // The future is ready after the request is durable or rejected.
    std::future<Status> submit(WriteRequest req);

private:
    struct Pending {
        WriteRequest req;
        std::promise<Status> done;
    };

    // Per-batch state for requests that are accepted but not yet applied.
    struct BatchState {
        std::unordered_map<std::string, int64_t> last_ts;
        std::unordered_map<std::string, uint32_t> new_ids;
    };

    void run();
    void process(std::vector<Pending>& batch);
    bool accept(const WriteRequest& req, BatchState& state, std::vector<std::byte>& out,
                std::vector<CommitRecord>& recs);
    uint32_t series_id(const std::string& name, const BatchState& state) const;

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Pending> queue_;  // guarded by mu_

    // Used only by the writer thread.
    Wal wal_;
    Head& head_;  // query threads read it too, under its lock
    const size_t max_batch_;
    uint32_t next_id_;

    // Last member: the thread starts in the constructor and uses the members
    // above, so they must be constructed first.
    std::thread thread_;
};

}  // namespace calf
