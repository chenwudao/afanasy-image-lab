#include "inode_fs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <list>
#include <unordered_map>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace image_lab {
namespace {
constexpr uint32_t blockSize = 4096, end = UINT32_MAX, payload = blockSize - sizeof(uint32_t);
constexpr uint32_t magic = 0x42465331; // B filesystem, format 1, little-endian Linux
struct Super { uint32_t magic, version, blocks, inodes, bankBlocks; };
struct Header { uint64_t generation; uint32_t crc, marker; };
struct Inode {
    uint64_t length;
    uint32_t parent, first, kind; // 0 unused, 1 directory, 2 regular file
    char name[76];
};
static_assert(sizeof(Inode) == 96, "inode disk layout changed");
uint32_t ceiling(uint64_t n, uint32_t d) { return uint32_t((n + d - 1) / d); }
void ioError(const char *what) { throw std::system_error(errno, std::generic_category(), what); }
void require(bool ok, const char *msg) { if (!ok) throw std::runtime_error(msg); }

void transfer(int fd, bool writing, void *data, size_t len, off_t offset, uint64_t *preads = nullptr) {
    auto buf = static_cast<unsigned char *>(data);
    while (len) {
        if (preads) ++*preads;
        ssize_t n = writing ? pwrite(fd, buf, len, offset) : pread(fd, buf, len, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) ioError("disk.img I/O");
        require(n > 0, "short disk.img I/O");
        buf += n; offset += n; len -= size_t(n);
    }
}
bool marked(const Bytes &map, uint32_t i) { return (map[i/8] & (1u << (i%8))) != 0; }
void mark(Bytes &map, uint32_t i, bool value) {
    if (value) map[i/8] |= 1u << (i%8);
    else map[i/8] &= ~(1u << (i%8));
}
struct Fd {
    int value = -1;
    ~Fd() { if (value >= 0) close(value); }
};
}

struct Disk::Impl {
    using Page = std::array<unsigned char, blockSize>;
    using Entries = std::list<std::pair<uint32_t, Page>>;
    Entries recent; // MRU at the front. All entries belong to this locked image.
    std::unordered_map<uint32_t, Entries::iterator> byBlock;
    Page scratch;
    CacheStats stats;
    Fd fd;
    Super s{};
    std::vector<Inode> nodes;
    Bytes bitmap, reserved;
    bool writer;
    bool poisoned = false;
    int active = 1;
    uint64_t generation = 0;
    uint32_t hint = 0;

    uint32_t dataStart() const { return 1 + 2*s.bankBlocks; }
    off_t bank(int slot) const { return off_t(1 + slot*s.bankBlocks)*blockSize; }
    size_t metadataSize() const { return sizeof(Inode)*nodes.size() + bitmap.size(); }
    void sync() { if (fdatasync(fd.value)) ioError("disk.img sync"); }

    Impl(bool write, bool create, uint32_t blocks): writer(write) {
        const char *setting = std::getenv("IMAGE_LAB_CACHE_BLOCKS");
        std::string value = setting ? setting : "64";
        require(!value.empty() && value.find_first_not_of("0123456789") == std::string::npos,
                "IMAGE_LAB_CACHE_BLOCKS must be a non-negative integer");
        stats.capacity = std::stoull(value);
        require(stats.capacity <= 262144, "IMAGE_LAB_CACHE_BLOCKS exceeds maximum disk size");
        const char *root = std::getenv("IMAGE_LAB_ROOT");
        require(root && *root == '/', "set absolute IMAGE_LAB_ROOT");
        std::string file = std::string(root)+"/disk.img";
        fd.value = open(file.c_str(), (write ? O_RDWR : O_RDONLY) | O_CLOEXEC | (create ? O_CREAT : 0), 0600);
        if (fd.value < 0) ioError("open disk.img");
        while (flock(fd.value, write ? LOCK_EX : LOCK_SH)) if (errno != EINTR) ioError("flock");
        struct stat st{};
        if (fstat(fd.value, &st)) ioError("stat disk.img");
        bool fresh = st.st_size == 0 && create && write;
        if (fresh) {
            require(blocks >= 16 && blocks <= 262144, "disk capacity must be 16..262144 blocks");
            s = {magic,1,blocks,std::max(32u, blocks/4),0};
            s.bankBlocks = ceiling(sizeof(Header)+sizeof(Inode)*s.inodes+ceiling(blocks,8),blockSize);
            if (ftruncate(fd.value,off_t(blocks)*blockSize)) ioError("size disk.img");
            transfer(fd.value,true,&s,sizeof(s),0);
        } else {
            transfer(fd.value,false,&s,sizeof(s),0,&stats.metadataPreads);
            require(s.magic==magic && s.version==1 && s.blocks>=16 && s.blocks<=262144 &&
                    st.st_size==off_t(s.blocks)*blockSize && s.inodes==std::max(32u,s.blocks/4) &&
                    s.bankBlocks==ceiling(sizeof(Header)+sizeof(Inode)*s.inodes+ceiling(s.blocks,8),blockSize),
                    "invalid disk.img superblock");
        }
        nodes.resize(s.inodes);
        bitmap.resize(ceiling(s.blocks,8));
        if (fresh) {
            nodes[0].kind = 1;
            for (uint32_t i=0;i<dataStart();++i) mark(bitmap,i,true);
            publish();
        } else {
            Header headers[2]{};
            for (int i=0;i<2;++i) transfer(fd.value,false,&headers[i],sizeof(Header),bank(i),&stats.metadataPreads);
            int first = headers[1].generation > headers[0].generation ? 1 : 0;
            require(load(first,headers[first]) || load(1-first,headers[1-first]), "no valid inode checkpoint");
        }
        reserved = bitmap;
        hint = dataStart();
    }

    bool load(int slot, const Header &h) {
        if (!h.generation || h.marker!=magic) return false;
        Bytes data(metadataSize());
        transfer(fd.value,false,data.data(),data.size(),bank(slot)+sizeof(Header),&stats.metadataPreads);
        if (crc32(0,data.data(),data.size())!=h.crc) return false;
        size_t n = sizeof(Inode)*nodes.size();
        std::memcpy(nodes.data(),data.data(),n);
        std::copy(data.begin()+n,data.end(),bitmap.begin());
        active=slot; generation=h.generation;
        return true;
    }

    void publish() {
        require(writer && !poisoned,"read-only or aborted transaction");
        Bytes data(metadataSize());
        size_t n = sizeof(Inode)*nodes.size();
        std::memcpy(data.data(),nodes.data(),n);
        std::copy(bitmap.begin(),bitmap.end(),data.begin()+n);
        int slot=1-active;
        transfer(fd.value,true,data.data(),data.size(),bank(slot)+sizeof(Header));
        sync(); // New data and its metadata reach disk before the commit header.
        Header h{generation+1,uint32_t(crc32(0,data.data(),data.size())),magic};
        transfer(fd.value,true,&h,sizeof(h),bank(slot));
        sync();
        active=slot; generation=h.generation; reserved=bitmap; hint=dataStart();
    }

    uint32_t child(uint32_t parent, const std::string &name) const {
        require(nodes[parent].kind==1,"parent is not a directory");
        for (uint32_t i=1;i<nodes.size();++i)
            if (nodes[i].kind && nodes[i].parent==parent && name==nodes[i].name) return i;
        return end;
    }
    uint32_t find(const std::string &path) const {
        require(!path.empty() && path[0]=='/',"expected absolute image path");
        if (path=="/") return 0;
        auto parts=split(path);
        uint32_t parent=find(parts.first);
        if (parent==end) return end;
        return child(parent,parts.second);
    }
    static std::pair<std::string,std::string> split(const std::string &path) {
        auto i=path.rfind('/');
        require(i!=std::string::npos && path.size()<=1024,"invalid image path");
        std::string name=path.substr(i+1);
        require(!name.empty() && name!="." && name!=".." && name.size()<sizeof(Inode::name)
                && name.find('\0')==std::string::npos,"invalid inode name");
        return {i ? path.substr(0,i) : "/",name};
    }
    uint32_t lookup(const std::string &path) const {
        uint32_t i=find(path); require(i!=end,"image path missing"); return i;
    }
    uint32_t allocateNode(const std::string &path,uint32_t kind) {
        require(writer && find(path)==end,"path exists or read-only");
        auto parts=split(path);
        uint32_t parent=lookup(parts.first);
        for (uint32_t i=1;i<nodes.size();++i) if (!nodes[i].kind) {
            nodes[i]={}; nodes[i].parent=parent; nodes[i].kind=kind; nodes[i].first=end;
            std::memcpy(nodes[i].name,parts.second.data(),parts.second.size());
            return i;
        }
        throw std::system_error(ENOSPC,std::generic_category(),"inode table full");
    }
    const Page &page(uint32_t block) {
        ++stats.accesses;
        auto cached = byBlock.find(block);
        if (cached != byBlock.end()) {
            ++stats.hits;
            recent.splice(recent.begin(), recent, cached->second);
            return cached->second->second;
        }
        transfer(fd.value, false, scratch.data(), scratch.size(), off_t(block)*blockSize, &stats.dataPreads);
        if (!stats.capacity) return scratch;
        if (recent.size() == stats.capacity) {
            byBlock.erase(recent.back().first);
            recent.pop_back();
        }
        recent.emplace_front(block, scratch);
        byBlock.emplace(block, recent.begin());
        return recent.front().second;
    }
    void readSlice(uint32_t block, size_t within, void *dst, size_t len) {
        if (stats.capacity) {
            std::memcpy(dst, page(block).data()+within, len);
        } else {
            ++stats.accesses;
            transfer(fd.value, false, dst, len, off_t(block)*blockSize+within, &stats.dataPreads);
        }
    }
    void checkBlock(uint32_t b) const {
        require(b>=dataStart() && b<s.blocks && marked(bitmap,b),"invalid data block");
    }
    std::vector<uint32_t> chain(const Inode &node) {
        require(node.kind==2 && node.length<=uint64_t(s.blocks)*payload,"invalid file inode");
        std::vector<uint32_t> blocks;
        uint32_t b=node.first;
        for (uint32_t i=0;i<ceiling(node.length,payload);++i) {
            checkBlock(b);
            blocks.push_back(b);
            readSlice(blocks.back(),0,&b,sizeof(b));
        }
        require(b==end,"invalid block chain length");
        return blocks;
    }
};
Disk::Disk(bool writer,bool create,uint32_t blocks):p(new Impl(writer,create,blocks)) {}
Disk::~Disk() { delete p; }
bool Disk::exists(const std::string &path) const { return p->find(path)!=end; }
uint64_t Disk::size(const std::string &path) const { return p->nodes[p->lookup(path)].length; }
std::vector<std::string> Disk::list(const std::string &path) const {
    auto parent=p->lookup(path);
    require(p->nodes[parent].kind==1,"not a directory");
    std::vector<std::string> names;
    for (size_t i=1;i<p->nodes.size();++i)
        if (p->nodes[i].kind && p->nodes[i].parent==parent) names.emplace_back(p->nodes[i].name);
    return names;
}
Bytes Disk::read(const std::string &path) const {
    const auto &node=p->nodes[p->lookup(path)];
    require(node.kind==2 && node.length<=uint64_t(p->s.blocks)*payload,"invalid file inode");
    Bytes bytes(node.length);
    uint32_t block = node.first;
    for (size_t pos=0; pos<bytes.size(); pos+=payload) {
        p->checkBlock(block);
        const auto &page = p->page(block);
        std::memcpy(&block, page.data(), sizeof(block));
        std::copy_n(page.data()+sizeof(block), std::min<size_t>(payload,bytes.size()-pos), bytes.data()+pos);
    }
    require(block==end,"invalid block chain length");
    return bytes;
}
Bytes Disk::readRows(const std::string &path,uint64_t offset,size_t stride,size_t rowBytes,unsigned rows) const {
    auto &node=p->nodes[p->lookup(path)];
    auto blocks=p->chain(node);
    require(rowBytes<=stride && rows && rowBytes && uint64_t(rows)*rowBytes<=node.length,"invalid source region");
    Bytes result(size_t(rows)*rowBytes);
    for (unsigned y=0;y<rows;++y) {
        uint64_t pos=offset+uint64_t(y)*stride;
        require(pos<=node.length && rowBytes<=node.length-pos,"source region exceeds file");
        for (size_t x=0;x<rowBytes;) {
            size_t within=pos%payload;
            size_t n=std::min(rowBytes-x,payload-within);
            p->readSlice(blocks[pos/payload], 4+within, result.data()+size_t(y)*rowBytes+x, n);
            x+=n; pos+=n;
        }
    }
    return result;
}
CacheStats Disk::cacheStats() const { return p->stats; }
uint32_t Disk::freeBlocks() const {
    uint32_t count=0;
    for (uint32_t i=p->dataStart();i<p->s.blocks;++i) if (!marked(p->bitmap,i)) ++count;
    return count;
}
void Disk::mkdir(const std::string &path) { p->allocateNode(path,1); }
void Disk::erase(const std::string &path) {
    require(p->writer && path!="/","cannot remove");
    auto i=p->lookup(path);
    if (p->nodes[i].kind==1) require(list(path).empty(),"directory not empty");
    else for (auto b:p->chain(p->nodes[i])) mark(p->bitmap,b,false);
    p->nodes[i]={};
}
void Disk::put(const std::string &path,const Bytes &bytes) try {
    require(p->writer,"read-only transaction");
    if (exists(path)) {
        require(p->nodes[p->lookup(path)].kind==2,"not a file");
        erase(path);
    }
    auto &node=p->nodes[p->allocateNode(path,2)];
    std::vector<uint32_t> blocks;
    for (size_t count=0;count<bytes.size();count+=payload) {
        while (p->hint<p->s.blocks && marked(p->reserved,p->hint)) ++p->hint;
        if (p->hint==p->s.blocks) throw std::system_error(ENOSPC,std::generic_category(),"disk.img full");
        blocks.push_back(p->hint);
        mark(p->reserved,p->hint,true); mark(p->bitmap,p->hint++,true);
    }
    for (size_t i=0;i<blocks.size();++i) {
        uint32_t next=i+1<blocks.size()?blocks[i+1]:end;
        off_t pos=off_t(blocks[i])*blockSize;
        // Writes always reach the image; discard any previous incarnation before I/O.
        auto stale = p->byBlock.find(blocks[i]);
        if (stale != p->byBlock.end()) {
            p->recent.erase(stale->second);
            p->byBlock.erase(stale);
        }
        transfer(p->fd.value,true,&next,sizeof(next),pos);
        size_t offset=i*payload;
        transfer(p->fd.value,true,const_cast<unsigned char*>(bytes.data()+offset),
                 std::min<size_t>(payload,bytes.size()-offset),pos+4);
    }
    node.first=blocks.empty()?end:blocks.front(); node.length=bytes.size();
} catch (...) {
    p->poisoned=true;
    throw;
}
void Disk::rename(const std::string &from,const std::string &to) {
    require(p->writer && !exists(to),"invalid rename");
    auto i=p->lookup(from);
    require(p->nodes[i].kind==2,"rename requires a file");
    auto parts=Impl::split(to);
    auto parent=p->lookup(parts.first);
    require(p->nodes[parent].kind==1,"parent is not a directory");
    p->nodes[i].parent=parent;
    std::memset(p->nodes[i].name,0,sizeof(Inode::name));
    std::memcpy(p->nodes[i].name,parts.second.data(),parts.second.size());
}
void Disk::save() { p->publish(); }
}
