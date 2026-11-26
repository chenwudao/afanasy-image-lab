#include "tile_store.h"

#include <cstdlib>
#include <chrono>
#include <thread>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

static std::string setting(const char *key) {
    const char *value = std::getenv(key);
    if (!value) throw std::runtime_error(std::string("missing ") + key);
    return value;
}
int main() {
    try {
        auto dir = image_lab::jobDirectory(setting("IMAGE_JOB_KEY"));
        int tile = std::stoi(setting("AF_TASK_ID"));
        int attempt = std::stoi(setting("IMAGE_ATTEMPT"));
        auto t = image_lab::descriptor(dir,tile);
        image_lab::event(dir,tile,attempt,"started",getpid());
        if (const char *delay = std::getenv("IMAGE_DEMO_DELAY_MS")) {
            int ms = std::stoi(delay);
            if (ms < 0 || ms > 60000) throw std::runtime_error("invalid demo delay");
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
        std::ifstream input(dir+"/source.rgb", std::ios::binary);
        std::vector<unsigned char> result(t.bytes());
        for (unsigned row=0; row<t.h; ++row) {
            input.seekg((std::streamoff(t.y+row)*t.width+t.x)*3);
            auto dst = result.data() + size_t(row)*t.w*3;
            if (!input.read(reinterpret_cast<char *>(dst), t.w*3)) throw std::runtime_error("short source row");
            for (unsigned x=0; x<t.w; ++x, dst+=3) {
                if (t.operation == "invert") {
                    for (int c=0; c<3; ++c) dst[c] = 255-dst[c];
                } else {
                    unsigned gray = (77u*dst[0]+150u*dst[1]+29u*dst[2]+128u)/256u;
                    dst[0]=dst[1]=dst[2]=static_cast<unsigned char>(gray);
                }
            }
        }
        image_lab::stage(dir,tile,attempt,result);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
