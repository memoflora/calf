#include "wal.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>

namespace calf {

namespace {

// The only call that makes written data durable.
bool sync_fd(int fd) {
#ifdef __APPLE__
    // macOS fsync() only moves data to the drive, which can keep it in a
    // volatile cache. F_FULLFSYNC also makes the drive flush that cache.
    return ::fcntl(fd, F_FULLFSYNC) == 0;
#else
    return ::fdatasync(fd) == 0;
#endif
}

}  // namespace

std::optional<Wal> Wal::create(const std::string& path) {
    // O_EXCL: Step 2 has no replay. Series IDs from an old file would clash
    // with the new IDs, which start at 0 again.
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        std::perror(path.c_str());
        return std::nullopt;
    }
    Wal wal(fd);

    // The new directory entry is not durable until the directory is synced.
    // Without this, a crash can lose the whole file, acked records included.
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    if (dir.empty()) dir = ".";
    int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (dfd < 0) {
        std::perror(dir.c_str());
        return std::nullopt;
    }
    bool ok = sync_fd(dfd);
    ::close(dfd);
    if (!ok) {
        std::perror("sync WAL directory");
        return std::nullopt;
    }
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

}  // namespace calf
