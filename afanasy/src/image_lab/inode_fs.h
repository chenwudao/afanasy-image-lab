#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace image_lab {
using Bytes = std::vector<unsigned char>;

// A handle owns a shared/exclusive flock for the lifetime of one transaction.
// Dropping an unsaved transaction retains the previous namespace and bitmap.
class Disk {
public:
    explicit Disk(bool writer = false, bool create = false, uint32_t blocks = 16384);
    ~Disk();
    Disk(const Disk &) = delete;
    Disk &operator=(const Disk &) = delete;
    bool exists(const std::string &path) const;
    uint64_t size(const std::string &path) const;
    Bytes read(const std::string &path) const;
    Bytes readRows(const std::string &path,uint64_t offset,size_t stride,size_t rowBytes,unsigned rows) const;
    uint32_t freeBlocks() const;
    std::vector<std::string> list(const std::string &path) const;
    void mkdir(const std::string &path);
    void put(const std::string &path, const Bytes &data);
    void erase(const std::string &path);
    void rename(const std::string &from, const std::string &to);
    void save();
private:
    struct Impl;
    Impl *p;
};
}
