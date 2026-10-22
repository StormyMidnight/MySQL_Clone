#include "wal.hpp"

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {
    uint64_t fnv(const std::string& s) {
        uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
        return h;
    }
} // namespace

std::vector<WalGroup> Wal::readAll(size_t& validBytes) const {
    validBytes = 0;
    std::vector<WalGroup> out;
    std::ifstream in(path_, std::ios::binary);
    if (!in) return out;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string data = ss.str();
    size_t pos = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) break;
        unsigned long long lsn = 0, len = 0, sum = 0;
        if (std::sscanf(data.substr(pos, nl - pos).c_str(), "G %llu %llu %llx", &lsn, &len, &sum) != 3) break;
        size_t start = nl + 1, n = static_cast<size_t>(len);
        if (data.size() - start < n + 1 || data[start + n] != '\n') break;
        std::string payload = data.substr(start, n);
        if (fnv(payload) != sum) break;
        out.push_back({ static_cast<uint64_t>(lsn), std::move(payload) });
        pos = start + n + 1;
        validBytes = pos;
    }
    return out;
}

void Wal::trimTo(size_t bytes) const {
    if (::truncate(path_.c_str(), static_cast<off_t>(bytes)) != 0 && access(path_.c_str(), F_OK) == 0)
        throw std::runtime_error("cannot truncate " + path_);
}

void Wal::append(uint64_t lsn, const std::string& payload) const {
    char hdr[96];
    std::snprintf(hdr, sizeof hdr, "G %llu %zu %llx\n", static_cast<unsigned long long>(lsn), payload.size(),
        static_cast<unsigned long long>(fnv(payload)));
    std::string rec = std::string(hdr) + payload + "\n";
    int fd = ::open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) throw std::runtime_error("cannot open " + path_);
    size_t off = 0;
    while (off < rec.size()) {
        ssize_t n = ::write(fd, rec.data() + off, rec.size() - off);
        if (n <= 0) { ::close(fd); throw std::runtime_error("WAL write failed"); }
        off += static_cast<size_t>(n);
    }
    bool ok = ::fsync(fd) == 0;
    ::close(fd);
    if (!ok) throw std::runtime_error("WAL fsync failed");
}

void Wal::reset() const {
    int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) ::close(fd);
}