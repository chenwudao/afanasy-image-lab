#include "tile_store.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace image_lab {
namespace {
void check(bool ok, const std::string &what) {
    if (!ok) throw std::runtime_error(what);
}
std::string prefix(const std::string &dir, int tile) {
    check(tile >= 0, "invalid tile number");
    return dir + "/tile-" + std::to_string(tile);
}
class File {
public:
    explicit File(int handle): fd(handle) { check(fd >= 0, std::strerror(errno)); }
    ~File() { close(fd); }
    File(const File &) = delete;
    int fd;
};
void writeAll(int fd, const void *buf, size_t count) {
    const char *p = static_cast<const char *>(buf);
    while (count) {
        ssize_t n = write(fd, p, count);
        if (n < 0 && errno == EINTR) continue;
        check(n > 0, "write failed");
        p += n;
        count -= size_t(n);
    }
}
}
std::string jobDirectory(const std::string &key) {
    const char *root = std::getenv("IMAGE_LAB_ROOT");
    check(root && *root == '/', "IMAGE_LAB_ROOT must be an absolute path");
    check(key.size() == 32 && key.find_first_not_of("0123456789abcdef") == std::string::npos, "invalid job key");
    return std::string(root) + "/" + key;
}
Tile descriptor(const std::string &dir, int tile) {
    Tile t{};
    unsigned side = 0;
    std::ifstream in(dir + "/manifest");
    check(bool(in >> t.width >> t.height >> side >> t.operation), "invalid manifest");
    check(t.width && t.height && t.width <= 65535 && t.height <= 65535 && side && side <= 1024, "invalid dimensions");
    check(t.operation == "invert" || t.operation == "gray", "invalid operation");
    unsigned cols = (t.width + side - 1) / side;
    check(tile >= 0 && unsigned(tile) / cols < (t.height + side - 1) / side, "tile outside image");
    t.x = (unsigned(tile) % cols) * side;
    t.y = (unsigned(tile) / cols) * side;
    t.w = std::min(side, t.width - t.x);
    t.h = std::min(side, t.height - t.y);
    return t;
}
void stage(const std::string &dir, int tile, int attempt, const std::vector<unsigned char> &rgb) {
    check(rgb.size() == descriptor(dir,tile).bytes(), "incorrect tile payload length");
    std::string path = prefix(dir,tile) + ".attempt-" + std::to_string(attempt);
    std::string tmp = path + ".tmp.XXXXXX";
    std::vector<char> name(tmp.begin(), tmp.end());
    name.push_back(0);
    File file(mkstemp(name.data()));
    try {
        writeAll(file.fd,rgb.data(),rgb.size());
        check(fsync(file.fd) == 0 && rename(name.data(),path.c_str()) == 0, "cannot stage result");
    } catch (...) { unlink(name.data()); throw; }
}
void commit(const std::string &dir, int tile, int attempt) {
    auto expected = descriptor(dir,tile).bytes();
    auto staged = prefix(dir,tile) + ".attempt-" + std::to_string(attempt);
    auto final = prefix(dir,tile) + ".rgb";
    struct stat info{};
    if (stat(final.c_str(), &info) != 0) {
        check(errno == ENOENT, "cannot inspect committed tile");
        check(stat(staged.c_str(), &info) == 0 && S_ISREG(info.st_mode) && size_t(info.st_size) == expected, "missing or partial staged tile");
        check(link(staged.c_str(), final.c_str()) == 0 || errno == EEXIST, "cannot publish tile");
    }
    check(stat(final.c_str(), &info) == 0 && S_ISREG(info.st_mode) && size_t(info.st_size) == expected, "invalid committed tile");
    File directory(open(dir.c_str(), O_RDONLY | O_DIRECTORY));
    check(fsync(directory.fd) == 0, "cannot sync committed tile");
    unlink(staged.c_str());
}
void event(const std::string &dir, int tile, int attempt, const std::string &state, int pid) {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    std::ostringstream line;
    line << ms << ' ' << tile << ' ' << attempt << ' ' << state << ' ' << pid << '\n';
    auto record = line.str();
    File log(open((dir+"/events.tsv").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600));
    writeAll(log.fd,record.data(),record.size());
}
}
