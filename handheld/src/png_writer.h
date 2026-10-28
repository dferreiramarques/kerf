#pragma once

#include <cstdint>
#include <string>

namespace kerf::ui
{
    // Minimal PNG encoder (8-bit RGB, stored/uncompressed deflate blocks) so screenshots need no
    // zlib. Files are large (about 900 KB for 640x480) but every viewer opens them.
    // `rgb` holds width*height*3 bytes, row-major. `utf8Path` is UTF-8.
    bool writePngRgb (const std::string& utf8Path, const uint8_t* rgb, int width, int height);
}
