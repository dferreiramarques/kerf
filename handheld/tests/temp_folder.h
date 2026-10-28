#pragma once

// A folder of its own for a test, removed when the test is over, and a few helpers to look at what
// is in it. For the tests of the project files (test_project_store.cpp, test_project_keeper.cpp,
// test_app_project.cpp).

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
 #include <process.h>
#else
 #include <unistd.h>
#endif

namespace kerf::test
{
    inline int processId()
    {
#if defined(_WIN32)
        return (int) _getpid();
#else
        return (int) ::getpid();
#endif
    }

    struct TempFolder
    {
        std::filesystem::path path;

        explicit TempFolder (const std::string& prefix = "kerf-test")
        {
            static int counter = 0;
            path = std::filesystem::temp_directory_path() / (prefix + "-" + std::to_string (processId()) + "-" + std::to_string (++counter));
            std::error_code ec;
            std::filesystem::remove_all (path, ec);
            std::filesystem::create_directories (path, ec);
        }

        TempFolder (const TempFolder&) = delete;
        TempFolder& operator= (const TempFolder&) = delete;

        ~TempFolder()
        {
            std::error_code ec;
            std::filesystem::remove_all (path, ec);
        }

        std::filesystem::path operator/ (const std::string& name) const { return path / name; }

        bool has (const std::string& name) const
        {
            std::error_code ec;
            return std::filesystem::exists (path / name, ec);
        }

        bool isFolder (const std::string& name) const
        {
            std::error_code ec;
            return std::filesystem::is_directory (path / name, ec);
        }

        // The bytes of a file ("" when there is none).
        std::string read (const std::string& name) const
        {
            std::ifstream in (path / name, std::ios::binary);
            return std::string (std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>());
        }

        void write (const std::string& name, const std::string& bytes) const
        {
            std::ofstream out (path / name, std::ios::binary | std::ios::trunc);
            out.write (bytes.data(), (std::streamsize) bytes.size());
        }

        void makeFolder (const std::string& name) const
        {
            std::error_code ec;
            std::filesystem::create_directories (path / name, ec);
        }

        // Takes a file (or a folder and what is in it) away.
        void remove (const std::string& name) const
        {
            std::error_code ec;
            std::filesystem::remove_all (path / name, ec);
        }

        // The names of everything in the folder, sorted.
        std::vector<std::string> names() const
        {
            std::vector<std::string> out;
            std::error_code ec;

            for (const auto& e : std::filesystem::directory_iterator (path, ec))
                out.push_back (e.path().filename().string());

            std::sort (out.begin(), out.end());
            return out;
        }

        // How many of the names start with `prefix`.
        int count (const std::string& prefix) const
        {
            int n = 0;

            for (const auto& name : names())
                if (name.compare (0, prefix.size(), prefix) == 0)
                    ++n;

            return n;
        }
    };
}
