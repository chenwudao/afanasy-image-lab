#include "inode_fs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
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

void transfer(int fd, bool writing, void *data, size_t len, off_t offset) {
    auto buf = static_cast<unsigned char *>(data);
    while (len) {
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
    Fd fd;
    Super s{};
    std::vector<Inode> nodes;
    Bytes bitmap, reserved;
    bool writer;
    int active = 1;
    uint64_t generation = 0;
    uint32_t hint = 0;

    uint32_t dataStart() const { return 1 + 2*s.bankBlocks; }
    off_t bank(int slot) const { return off_t(1 + slot*s.bankBlocks)*blockSize; }
    size_t metadataSize() const { return sizeof(Inode)*nodes.size() + bitmap.size(); }
    void sync() { if (fdatasync(fd.value)) ioError("disk.img sync"); }

    Impl(bool write, bool create, uint32_t blocks): writer(write) {
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
            transfer(fd.value,false,&s,sizeof(s),0);
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
            for (int i=0;i<2;++i) transfer(fd.value,false,&headers[i],sizeof(Header),bank(i));
            int first = headers[1].generation > headers[0].generation ? 1 : 0;
            require(load(first,headers[first]) || load(1-first,headers[1-first]), "no valid inode checkpoint");
        }
        reserved = bitmap;
        hint = dataStart();
    }

    bool load(int slot, const Header &h) {
        if (!h.generation || h.marker!=magic) return false;
        Bytes data(metadataSize());
        transfer(fd.value,false,data.data(),data.size(),bank(slot)+sizeof(Header));
        if (crc32(0,data.data(),data.size())!=h.crc) return false;
        size_t n = sizeof(Inode)*nodes.size();
        std::memcpy(nodes.data(),data.data(),n);
        std::copy(data.begin()+n,data.end(),bitmap.begin());
        active=slot; generation=h.generation;
        return true;
    }

    void publish() {
        require(writer,"read-only transaction");
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
        active=slot; generation=h.generation; reserved=bitmap;
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
    std::vector<uint32_t> chain(const Inode &node) const {
        require(node.kind==2 && node.length<=uint64_t(s.blocks)*payload,"invalid file inode");
        std::vector<uint32_t> blocks;
        uint32_t b=node.first;
        for (uint32_t i=0;i<ceiling(node.length,payload);++i) {
            require(b>=dataStart() && b<s.blocks && marked(bitmap,b),"invalid data block");
            blocks.push_back(b);
            transfer(fd.value,false,&b,sizeof(b),off_t(blocks.back())*blockSize);
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
    auto blocks=p->chain(node);
    Bytes bytes(node.length);
    size_t pos=0;
    for (auto b:blocks) {
        size_t n=std::min<size_t>(payload,bytes.size()-pos);
        transfer(p->fd.value,false,bytes.data()+pos,n,off_t(b)*blockSize+4);
        pos+=n;
    }
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
            transfer(p->fd.value,false,result.data()+size_t(y)*rowBytes+x,n,
                     off_t(blocks[pos/payload])*blockSize+4+within);
            x+=n; pos+=n;
        }
    }
    return result;
}
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
void Disk::put(const std::string &path,const Bytes &bytes) {
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
        transfer(p->fd.value,true,&next,sizeof(next),pos);
        size_t offset=i*payload;
        transfer(p->fd.value,true,const_cast<unsigned char*>(bytes.data()+offset),
                 std::min<size_t>(payload,bytes.size()-offset),pos+4);
    }
    node.first=blocks.empty()?end:blocks.front(); node.length=bytes.size();
}
void Disk::rename(const std::string &from,const std::string &to) {
    require(p->writer && !exists(to),"invalid rename");
    auto i=p->lookup(from);
    require(p->nodes[i].kind==2,"rename requires a file");
    auto parts=Impl::split(to);
    p->nodes[i].parent=p->lookup(parts.first);
    std::memset(p->nodes[i].name,0,sizeof(Inode::name));
    std::memcpy(p->nodes[i].name,parts.second.data(),parts.second.size());
}
void Disk::save() { p->publish(); }
}
