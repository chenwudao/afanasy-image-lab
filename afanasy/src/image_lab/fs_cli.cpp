#include "inode_fs.h"
#include "tile_store.h"
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <zlib.h>

using image_lab::Bytes;
using image_lab::Disk;
int main(int argc, char **argv) {
    try {
        if (argc < 2) throw std::runtime_error("expected init, prepare, read, list, mkdir, put, rm, backup or restore");
        std::string op=argv[1];
        if (op=="backup" && argc==3) { Disk::backup(argv[2]); return 0; }
        if (op=="restore" && argc==4) { Disk::restore(argv[2],argv[3]); return 0; }
        if (op=="import-completed" && argc==3) { image_lab::migrateCompleted(argv[2]); return 0; }
        if (op=="init" && argc==3) { Disk disk(true,true,std::stoul(argv[2])); return 0; }
        if (op=="prepare" && argc==6) {
            // One namespace checkpoint for all job inputs, including the uploaded bytes.
            std::string dir=argv[2], manifest=argv[3], suffix=argv[5];
            if (suffix!="png" && suffix!="jpg" && suffix!="-") throw std::runtime_error("invalid input format");
            size_t rgbSize=std::stoull(argv[4]);
            Bytes rgb(rgbSize);
            if (!std::cin.read(reinterpret_cast<char*>(rgb.data()),rgbSize)) throw std::runtime_error("short RGB input");
            Bytes original((std::istreambuf_iterator<char>(std::cin)),{});
            Disk disk(true,true);
            disk.mkdir(dir);
            disk.put(dir+"/manifest",Bytes(manifest.begin(),manifest.end()));
            disk.put(dir+"/source.rgb",rgb);
            if (suffix!="-") disk.put(dir+"/original."+suffix,original);
            disk.save(); return 0;
        }
        if (op=="trace" && argc>=3) {
            Disk disk;
            uLong checksum = 0;
            for (int i=2; i<argc; ++i) {
                const auto bytes = disk.read(argv[i]);
                checksum = crc32(checksum, bytes.data(), bytes.size());
            }
            const auto stats = disk.cacheStats();
            std::cout << "{\"capacity\":" << stats.capacity << ",\"accesses\":" << stats.accesses
                      << ",\"hits\":" << stats.hits << ",\"data_preads\":" << stats.dataPreads
                      << ",\"metadata_preads\":" << stats.metadataPreads << ",\"crc32\":" << checksum << "}\n";
            return 0;
        }
        if (op=="free" && argc==2) { std::cout << Disk().freeBlocks() << '\n'; return 0; }
        if (argc!=3) throw std::runtime_error("expected one absolute image path");
        std::string path=argv[2];
        if (op=="read" || op=="list") {
            Disk disk;
            if (op=="list") for (auto &name:disk.list(path)) std::cout << name << '\n';
            else {
                Bytes bytes=disk.read(path);
                std::cout.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
            }
            return std::cout ? 0 : 1;
        }
        Disk disk(true);
        if (op=="mkdir") disk.mkdir(path);
        else if (op=="rm") disk.erase(path);
        else if (op=="put") disk.put(path,Bytes(std::istreambuf_iterator<char>(std::cin),{}));
        else throw std::runtime_error("unknown disk operation");
        disk.save();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
