#include "wal.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>

namespace calf {

namespace {

// Makes appended data durable.
bool sync_fd(int fd) {
#ifdef __APPLE__
    // macOS fsync() only moves data to the drive, which can keep it in a
    // volatile cache. F_FULLFSYNC also makes the drive flush that cache.
    return ::fcntl(fd, F_FULLFSYNC) == 0;
#else
    return ::fdatasync(fd) == 0;
#endif
}

// pread from offset 0, so the result does not depend on the fd offset.
std::optional<std::vector<std::byte>> read_fd(int fd) {
    std::vector<std::byte> out;
    std::byte buf[1 << 16];
    for (;;) {
        ssize_t r = ::pread(fd, buf, sizeof buf, static_cast<off_t>(out.size()));
        if (r > 0) {
            out.insert(out.end(), buf, buf + r);
        } else if (r == 0) {
            return out;
        } else if (errno != EINTR) {
            return std::nullopt;
        }
    }
}

bool sync_parent_dir(const std::string& path) {
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    if (dir.empty()) dir = ".";
    int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (dfd < 0) {
        std::perror(dir.c_str());
        return false;
    }
    bool ok = sync_fd(dfd);
    ::close(dfd);
    if (!ok) std::perror("sync WAL directory");
    return ok;
}

}  // namespace

std::optional<std::vector<std::byte>> read_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        std::perror(path.c_str());
        return std::nullopt;
    }
    auto bytes = read_fd(fd);
    if (!bytes) std::perror(path.c_str());
    ::close(fd);
    return bytes;
}

std::optional<Wal> Wal::open(const std::string& path) {
    // O_EXCL first, to know if this call created the file: only a new file
    // needs the directory sync.
    const int flags = O_RDWR | O_APPEND | O_CLOEXEC;
    bool created = true;
    int fd = ::open(path.c_str(), flags | O_CREAT | O_EXCL, 0644);
    if (fd < 0 && errno == EEXIST) {
        created = false;
        fd = ::open(path.c_str(), flags);
    }
    if (fd < 0) {
        std::perror(path.c_str());
        return std::nullopt;
    }
    Wal wal(fd);

    // The new directory entry is not durable until the directory is synced.
    // Without this, a crash can lose the whole file, acked records included.
    if (created && !sync_parent_dir(path)) return std::nullopt;
    return wal;
}

Wal::~Wal() {
    if (fd_ >= 0) ::close(fd_);
}

// The moved-from Wal gets fd -1, so only one destructor closes the fd.
Wal::Wal(Wal&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

bool Wal::append(std::span<const std::byte> bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t r = ::write(fd_, bytes.data() + done, bytes.size() - done);
        if (r > 0) {
            done += static_cast<size_t>(r);
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool Wal::sync() { return sync_fd(fd_); }

std::optional<std::vector<std::byte>> Wal::read_all() { return read_fd(fd_); }

bool Wal::truncate_to(size_t offset) {
    if (::ftruncate(fd_, static_cast<off_t>(offset)) != 0) return false;
#ifdef __APPLE__
    return sync_fd(fd_);
#else
    // fsync, not fdatasync: only the size changes, and the size is metadata.
    return ::fsync(fd_) == 0;
#endif
}

}  // namespace calf
