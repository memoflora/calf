#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace calf {

class Wal {
public:
    // Creates a new WAL file. Prints the error and returns nullopt if the
    // file exists or cannot be created and synced.
    static std::optional<Wal> create(const std::string& path);

    ~Wal();
    Wal(Wal&& other) noexcept;
    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;
    Wal& operator=(Wal&&) = delete;

    bool append(std::span<const std::byte> bytes);
    bool sync();

private:
    explicit Wal(int fd) : fd_(fd) {}
    int fd_;
};

}  // namespace calf
