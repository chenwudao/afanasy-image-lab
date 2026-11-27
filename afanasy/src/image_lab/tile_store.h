#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace image_lab {
struct Tile {
    unsigned width, height, x, y, w, h;
    std::string operation;
    size_t bytes() const { return size_t(w) * h * 3; }
};

// Events use the host directory; all job payloads use the inode namespace.
void migrateCompleted(const std::string &key);
std::string jobDirectory(const std::string &key);
std::vector<unsigned char> source(const std::string &dir,const Tile &t);
Tile descriptor(const std::string &dir, int tile);
void stage(const std::string &dir, int tile, int attempt, const std::vector<unsigned char> &rgb);
void commit(const std::string &dir, int tile, int attempt);
void event(const std::string &dir, int tile, int attempt, const std::string &state, int pid = 0, uint64_t dispatchSequence = 0);
void observe(const std::string &key, int tile, int attempt, const std::string &state, uint64_t dispatchSequence = 0, int pid = 0);
}
