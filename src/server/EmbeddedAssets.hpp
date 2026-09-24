#pragma once

#include <cstddef>
#include <vector>

namespace usc::assets {

struct Asset {
    const char* path; // "/index.html"
    const unsigned char* data;
    size_t size;
};

// Generated at build time from web/ by cmake/EmbedAssets.cmake.
const std::vector<Asset>& all();

} // namespace usc::assets
