// autotest: PNG encoding of screenshots (stb_image_write, third_party/stb). The only translation unit
// with STB_IMAGE_WRITE_IMPLEMENTATION; no stdio (the file is written here with a wide path).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma warning(push, 0)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define STBIW_ASSERT(x) ((void)0)
#include "third_party/stb/stb_image_write.h"
#pragma warning(pop)

namespace sr2::modules::autotest {

// RGBA8 rows (top first) -> PNG file. False + why on failure.
bool WritePng(const std::filesystem::path& file, unsigned w, unsigned h, const unsigned char* rgba, std::string& why) {
    std::vector<unsigned char> out;
    stbi_write_png_compression_level = 5;  // fast enough for 1440p on a pipe thread, still small
    auto sink = [](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<unsigned char>*>(ctx);
        v->insert(v->end(), static_cast<unsigned char*>(data), static_cast<unsigned char*>(data) + size);
    };
    if (!stbi_write_png_to_func(sink, &out, static_cast<int>(w), static_cast<int>(h), 4, rgba, static_cast<int>(w) * 4) ||
        out.empty()) {
        why = "PNG encoding failed";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) {
        why = "cannot open the file for writing";
        return false;
    }
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!f) {
        why = "write failed";
        return false;
    }
    return true;
}

}  // namespace sr2::modules::autotest
