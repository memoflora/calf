#pragma once

#include "wal.h"

#include <cstddef>
#include <cstdint>

namespace calf {

// Listens on 127.0.0.1:port until a fatal error. Returns the exit code.
// max_batch 0 means no limit.
int run_server(uint16_t port, Wal wal, size_t max_batch);

}  // namespace calf
