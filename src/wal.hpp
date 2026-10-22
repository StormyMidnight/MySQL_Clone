#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Append-only write-ahead log. Each group is "G <lsn> <len> <fnv64-hex>\n<payload>\n".
// A torn or corrupt tail group is detected by length/checksum and discarded on recovery.
struct WalGroup { uint64_t lsn = 0; std::string payload; };

class Wal {
public:
    explicit Wal(std::string path) : path_(std::move(path)) {}
    std::vector<WalGroup> readAll(size_t& validBytes) const; // stops at first torn/corrupt group

    void trimTo(size_t bytes) const; // drop a torn tail
    void append(uint64_t lsn, const std::string& payload) const; // write + fsync
    void reset() const; // truncate to empty

private:
    std::string path_;
};