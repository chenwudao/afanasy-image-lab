#include "../afanasy/src/image_lab/tile_store.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const std::string dir = argv[1];
    std::ofstream(dir + "/manifest") << "3 2 2 invert\n";
    auto tile = image_lab::descriptor(dir, 1);
    if (tile.x != 2 || tile.w != 1 || tile.h != 2) return 1;
    std::ofstream(dir + "/tile-1.attempt-1") << 'x';
    try { image_lab::commit(dir,1,1); return 1; }
    catch (const std::runtime_error &) {}
    if (std::ifstream(dir + "/tile-1.rgb").good()) return 1;
    const std::vector<unsigned char> pixels{9,8,7,6,5,4};
    image_lab::stage(dir,1,2,pixels);
    image_lab::commit(dir,1,2);
    image_lab::commit(dir,1,2); // successful commit whose reply was lost
    image_lab::stage(dir,1,1,{1,1,1,1,1,1});
    image_lab::commit(dir,1,1); // existing pixels survive a late duplicate
    std::ifstream result(dir + "/tile-1.rgb",std::ios::binary);
    std::vector<unsigned char> actual((std::istreambuf_iterator<char>(result)), {});
    if (actual != pixels) return 1;
    std::cout << "PASS partial write, confirmation replay and late duplicate\n";
}
