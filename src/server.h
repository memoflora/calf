#pragma once

#include "head.h"
#include "wal.h"

#include <cstddef>
#include <cstdint>

namespace calf {

// Returns only on a setup error. After the writer starts it never returns, so
// head outlives every thread. max_batch 0 means no limit.
int run_server(uint16_t port, Wal wal, Head head, uint32_t next_id, size_t max_batch);

}  // namespace calf
