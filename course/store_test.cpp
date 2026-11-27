#include "../afanasy/src/image_lab/tile_store.h"
#include "../afanasy/src/image_lab/inode_fs.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <system_error>
using image_lab::Bytes;
using image_lab::Disk;
static void expect(bool ok) { if (!ok) throw std::runtime_error("store assertion failed"); }
int main(int argc, char **argv) {
    if (argc!=2) return 2;
    setenv("IMAGE_LAB_ROOT",argv[1],1);
    const std::string base="/test";
    {
        Disk d(true,true,32);
        d.mkdir(base); d.mkdir(base+"/nested");
        d.put(base+"/nested/item",{1,2,3});
        d.put(base+"/manifest",{'3',' ','2',' ','2',' ','i','n','v','e','r','t'});
        d.put(base+"/tile-1.attempt-1",{'x'});
        d.save();
    }
    try { image_lab::commit(base,1,1); return 1; }
    catch (const std::runtime_error &) {}
    expect(!Disk().exists(base+"/tile-1.rgb"));
    Bytes pixels{9,8,7,6,5,4};
    image_lab::stage(base,1,2,pixels);
    image_lab::commit(base,1,2);
    image_lab::commit(base,1,2);
    image_lab::stage(base,1,1,{1,1,1,1,1,1});
    image_lab::commit(base,1,1);
    expect(Disk().read(base+"/tile-1.rgb")==pixels);
    image_lab::stage(base,0,1,Bytes(12,42));
    { Disk d(true); d.put("/fill",Bytes(size_t(d.freeBlocks())*4092,0)); d.save(); }
    try { image_lab::commit(base,0,1); return 1; }
    catch (const std::system_error &e) { expect(e.code().value()==ENOSPC); }
    {
        Disk d;
        expect(!d.exists(base+"/tile-0.rgb") && !d.exists(base+"/result.png"));
        expect(d.read(base+"/tile-1.rgb")==pixels && d.exists(base+"/tile-0.attempt-1"));
    }
    { Disk d(true); d.erase("/fill"); d.save(); }
    image_lab::commit(base,0,1);
    expect(Disk().size(base+"/result.png")>8);
    uint32_t free=Disk().freeBlocks();
    try {
        Disk d(true);
        d.put(base+"/nested/item", Bytes(size_t(free+1)*4092,7));
        d.save();
        return 1;
    } catch (const std::system_error &e) { expect(e.code().value()==ENOSPC); }
    expect(Disk().read(base+"/nested/item")==Bytes({1,2,3}));
    expect(Disk().freeBlocks()==free);
    { Disk d(true); d.erase(base+"/nested/item"); d.erase(base+"/nested"); d.save(); }
    expect(Disk().freeBlocks()==free+1);
    {
        // Keep one writer handle through multiple checkpoints and recycle the
        // same first free block. A hit must never return the former inode's bytes.
        Disk d(true);
        d.put("/old",{19,20,21}); d.save();
        expect(d.read("/old")==Bytes({19,20,21}));
        d.erase("/old"); d.save();
        d.put("/new",{33,34,35}); d.save();
        expect(d.read("/new")==Bytes({33,34,35}));
        expect(d.readRows("/new",1,3,2,1)==Bytes({34,35}));
    }
    std::cout << "PASS inode reopen, nested paths, block reclamation, partial write, ENOSPC rollback and duplicate commit\n";
}
