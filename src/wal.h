#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace calf {

// Reads the whole file. Prints the error and returns nullopt on failure.
std::optional<std::vector<std::byte>> read_file(const std::string& path);

class Wal {
public:
    // Opens the WAL file, or creates it if it does not exist. Prints the
    // error and returns nullopt on failure.
    static std::optional<Wal> open(const std::string& path);

    ~Wal();
    Wal(Wal&& other) noexcept;
    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;
    Wal& operator=(Wal&&) = delete;

    std::optional<std::vector<std::byte>> read_all();

    // Cuts the file to offset and makes the new size durable.
    bool truncate_to(size_t offset);

    bool append(std::span<const std::byte> bytes);
    bool sync();

private:
    explicit Wal(int fd) : fd_(fd) {}
    int fd_;
};

}  // namespace calf
