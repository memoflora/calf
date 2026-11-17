#pragma once

#include "head.h"
#include "wal.h"

#include <cstddef>
#include <cstdint>

namespace calf {

// Listens on 127.0.0.1:port. Returns the exit code of a setup error. After the
// writer starts it never returns, so head outlives every thread that uses it.
// head and next_id come from replay. max_batch 0 means no limit.
int run_server(uint16_t port, Wal wal, Head head, uint32_t next_id, size_t max_batch);

}  // namespace calf
