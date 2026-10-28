#include "png_writer.h"

#include <algorithm>
#include <fstream>
#include <vector>

#include "path_util.h"

namespace kerf::ui
{
    namespace
    {
        uint32_t crc32 (const uint8_t* data, size_t n, uint32_t crc = 0)
        {
            static uint32_t table[256];
            static bool ready = false;

            if (! ready)
            {
                for (uint32_t i = 0; i < 256; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                    table[i] = c;
                }
                ready = true;
            }

            crc = ~crc;
            for (size_t i = 0; i < n; ++i)
                crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }

        void put32be (std::vector<uint8_t>& v, uint32_t x)
        {
            v.push_back ((uint8_t) (x >> 24)); v.push_back ((uint8_t) (x >> 16));
            v.push_back ((uint8_t) (x >> 8));  v.push_back ((uint8_t) x);
        }

        void writeChunk (std::ofstream& out, const char type[4], const std::vector<uint8_t>& body)
        {
            std::vector<uint8_t> head;
            put32be (head, (uint32_t) body.size());
            out.write ((const char*) head.data(), 4);

            std::vector<uint8_t> tc (type, type + 4);
            tc.insert (tc.end(), body.begin(), body.end());
            out.write ((const char*) tc.data(), (std::streamsize) tc.size());

            std::vector<uint8_t> tail;
            put32be (tail, crc32 (tc.data(), tc.size()));
            out.write ((const char*) tail.data(), 4);
        }
    }

    bool writePngRgb (const std::string& utf8Path, const uint8_t* rgb, int width, int height)
    {
        if (width <= 0 || height <= 0 || rgb == nullptr)
            return false;

        // Raw scanlines, each prefixed with filter type 0.
        std::vector<uint8_t> raw;
        raw.reserve ((size_t) height * ((size_t) width * 3 + 1));

        for (int y = 0; y < height; ++y)
        {
            raw.push_back (0);
            raw.insert (raw.end(), rgb + (size_t) y * (size_t) width * 3, rgb + (size_t) (y + 1) * (size_t) width * 3);
        }

        // zlib stream made of stored blocks.
        std::vector<uint8_t> z { 0x78, 0x01 };
        uint32_t a = 1, b = 0;

        for (auto byte : raw)
        {
            a = (a + byte) % 65521u;
            b = (b + a) % 65521u;
        }

        for (size_t pos = 0; pos < raw.size();)
        {
            const auto n = std::min<size_t> (65535, raw.size() - pos);
            const bool last = pos + n >= raw.size();
            z.push_back (last ? 1 : 0);
            z.push_back ((uint8_t) (n & 0xFF));
            z.push_back ((uint8_t) (n >> 8));
            z.push_back ((uint8_t) (~n & 0xFF));
            z.push_back ((uint8_t) ((~n >> 8) & 0xFF));
            z.insert (z.end(), raw.begin() + (std::ptrdiff_t) pos, raw.begin() + (std::ptrdiff_t) (pos + n));
            pos += n;
        }

        put32be (z, (b << 16) | a);

        std::ofstream out (kerf::pathFromUtf8 (utf8Path), std::ios::binary);
        if (! out)
            return false;

        static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        out.write ((const char*) signature, 8);

        std::vector<uint8_t> ihdr;
        put32be (ihdr, (uint32_t) width);
        put32be (ihdr, (uint32_t) height);
        ihdr.push_back (8);  // bit depth
        ihdr.push_back (2);  // colour type: truecolour
        ihdr.push_back (0);  // deflate
        ihdr.push_back (0);  // filter method
        ihdr.push_back (0);  // no interlace

        writeChunk (out, "IHDR", ihdr);
        writeChunk (out, "IDAT", z);
        writeChunk (out, "IEND", {});

        return (bool) out;
    }
}
