#include "tile_store.h"
#include "inode_fs.h"
#include <png.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
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
static Tile describe(const Bytes &manifest, int tile) {
    Tile t{};
    unsigned side = 0;
    std::istringstream in(std::string(manifest.begin(),manifest.end()));
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
static std::string imageDir(const std::string &dir) {
    return "/" + dir.substr(dir.rfind('/')+1);
}
Tile descriptor(const std::string &dir,int tile) {
    return describe(Disk().read(imageDir(dir)+"/manifest"),tile);
}
Bytes source(const std::string &dir,const Tile &t) {
    return Disk().readRows(imageDir(dir)+"/source.rgb",(uint64_t(t.y)*t.width+t.x)*3,
                           size_t(t.width)*3,size_t(t.w)*3,t.h);
}

void stage(const std::string &dir, int tile, int attempt, const Bytes &rgb) {
    Disk disk(true);
    auto base=imageDir(dir);
    check(rgb.size()==describe(disk.read(base+"/manifest"),tile).bytes(),"incorrect tile payload length");
    disk.put(prefix(base,tile)+".attempt-"+std::to_string(attempt),rgb);
    disk.save();
}

static Bytes assemble(Disk &disk,const std::string &base,const Bytes &manifest,unsigned count) {
    Tile first=describe(manifest,0);
    Bytes canvas(size_t(first.width)*first.height*3);
    for (unsigned i=0;i<count;++i) {
        Tile t=describe(manifest,i);
        Bytes rgb=disk.read(prefix(base,i)+".rgb");
        check(rgb.size()==t.bytes(),"incorrect committed tile length");
        for (unsigned row=0;row<t.h;++row) {
            size_t dst=(size_t(t.y+row)*t.width+t.x)*3;
            std::copy_n(rgb.data()+size_t(row)*t.w*3,t.w*3,canvas.data()+dst);
        }
    }
    png_image img{};
    img.version=PNG_IMAGE_VERSION; img.width=first.width; img.height=first.height; img.format=PNG_FORMAT_RGB;
    size_t length=0;
    check(png_image_write_to_memory(&img,nullptr,&length,0,canvas.data(),0,nullptr)!=0,"PNG sizing failed");
    Bytes png(length);
    check(png_image_write_to_memory(&img,png.data(),&length,0,canvas.data(),0,nullptr)!=0,"PNG encoding failed");
    png.resize(length);
    return png;
}

void migrateCompleted(const std::string &key) {
    const std::string legacy=jobDirectory(key), base="/"+key;
    Disk disk(true,true);
    if (disk.exists(base+"/result.png")) return;
    auto load=[&legacy](const std::string &name) {
        std::ifstream in(legacy+"/"+name,std::ios::binary);
        check(in.is_open(),"missing legacy job file");
        Bytes bytes((std::istreambuf_iterator<char>(in)),{});
        check(!in.bad(),"legacy file read failed");
        return bytes;
    };
    disk.mkdir(base);
    auto manifest=load("manifest");
    auto first=describe(manifest,0);
    auto source=load("source.rgb");
    check(source.size()==size_t(first.width)*first.height*3,"invalid legacy input size");
    disk.put(base+"/manifest",manifest);
    disk.put(base+"/source.rgb",source);
    for (const std::string suffix:{"png","jpg"}) {
        auto name="original."+suffix;
        if (access((legacy+"/"+name).c_str(),F_OK)==0) disk.put(base+"/"+name,load(name));
    }
    unsigned count=((first.width+first.w-1)/first.w)*((first.height+first.h-1)/first.h);
    for (unsigned i=0;i<count;++i) {
        auto name="tile-"+std::to_string(i)+".rgb";
        auto rgb=load(name);
        check(rgb.size()==describe(manifest,i).bytes(),"incomplete legacy tile");
        disk.put(base+"/"+name,rgb);
    }
    disk.put(base+"/result.png",assemble(disk,base,manifest,count));
    disk.save();
}

void commit(const std::string &dir, int tile, int attempt) {
    Disk disk(true);
    auto base=imageDir(dir);
    auto manifest=disk.read(base+"/manifest");
    auto t=describe(manifest,tile);
    auto final=prefix(base,tile)+".rgb";
    auto staged=prefix(base,tile)+".attempt-"+std::to_string(attempt);
    if (disk.exists(final)) {
        check(disk.size(final)==t.bytes(),"invalid committed tile");
        if (disk.exists(staged)) { disk.erase(staged); disk.save(); }
        return;
    }
    check(disk.size(staged)==t.bytes(),"partial staged tile");
    disk.rename(staged,final);
    auto first=describe(manifest,0);
    unsigned total=((first.width+first.w-1)/first.w)*((first.height+first.h-1)/first.h);
    auto names=disk.list(base);
    size_t committed=std::count_if(names.begin(),names.end(),[](const std::string &n) {
        return n.compare(0,5,"tile-")==0 && n.size()>4 && n.compare(n.size()-4,4,".rgb")==0;
    });
    if (committed==total) disk.put(base+"/result.png",assemble(disk,base,manifest,total));
    // Publish the last tile and final PNG in the same checkpoint before DONE.
    disk.save();
}
void event(const std::string &dir, int tile, int attempt, const std::string &state, int pid, uint64_t dispatchSequence) {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    std::ostringstream line;
    line << ms << '\t' << tile << '\t' << attempt << '\t' << state << '\t' << pid << '\t' << dispatchSequence << '\n';
    auto record = line.str();
    File log(open((dir+"/events.tsv").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600));
    writeAll(log.fd,record.data(),record.size());
}
void observe(const std::string &key, int tile, int attempt, const std::string &state, uint64_t sequence, int pid) {
    try { event(jobDirectory(key),tile,attempt,state,pid,sequence); }
    catch (const std::exception &e) { std::cerr << "image event: " << e.what() << '\n'; }
}
}
