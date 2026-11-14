#pragma once

#include <cstdint>

namespace calf {

// Listens on 127.0.0.1:port until a fatal error. Returns the exit code.
int run_server(uint16_t port);

}  // namespace calf
