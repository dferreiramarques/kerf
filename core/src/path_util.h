#pragma once

#include <filesystem>
#include <string>

namespace kerf
{
    // The UI and the file-system layer talk to each other in UTF-8 strings. On Windows a plain
    // std::string path would be read as the ANSI code page and fail for names such as
    // "Gravação.wav", so every string that names a file goes through these two helpers.
    inline std::filesystem::path pathFromUtf8 (const std::string& utf8)
    {
        return std::filesystem::path (std::u8string (reinterpret_cast<const char8_t*> (utf8.data()), utf8.size()));
    }

    inline std::string pathToUtf8 (const std::filesystem::path& p)
    {
        const auto u8 = p.u8string();
        return std::string (reinterpret_cast<const char*> (u8.data()), u8.size());
    }
}
