#include "project_store.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

#include "path_util.h"

#if defined(_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <io.h>
 #include <windows.h>
#else
 #include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace kerf::ui::ProjectStore
{
    namespace
    {
        std::string nameOf (const fs::path& p)
        {
            return pathToUtf8 (p.filename());
        }

        std::FILE* openForWriting (const fs::path& p)
        {
#if defined(_WIN32)
            return _wfopen (p.c_str(), L"wb");
#else
            return std::fopen (p.c_str(), "wb");
#endif
        }

        // The bytes of the file are on the disk, not only in the system's cache.
        bool flushToDisk (std::FILE* f)
        {
#if defined(_WIN32)
            return _commit (_fileno (f)) == 0;
#else
            if (::fsync (::fileno (f)) == 0)
                return true;

            // A file system that cannot be told to flush is not a file system that cannot be saved to.
            return errno == EINVAL || errno == ENOSYS || errno == ENOTSUP;
#endif
        }

        // Renames `from` over `to`, replacing it in one step.
        bool replaceFile (const fs::path& from, const fs::path& to, std::string& why)
        {
#if defined(_WIN32)
            if (MoveFileExW (from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return true;

            why = "error " + std::to_string ((unsigned long) GetLastError());
            return false;
#else
            if (std::rename (from.c_str(), to.c_str()) == 0)
                return true;

            why = std::strerror (errno);
            return false;
#endif
        }

        bool readFile (const fs::path& file, std::string& bytes, std::string& error)
        {
            std::error_code ec;
            const auto size = fs::file_size (file, ec);

            if (ec)
            {
                error = "cannot read " + nameOf (file) + " (" + ec.message() + ")";
                return false;
            }

            if (size > Project::kMaxFileBytes)
            {
                error = nameOf (file) + " is too big";
                return false;
            }

            std::ifstream in (file, std::ios::binary);

            if (! in)
            {
                error = "cannot open " + nameOf (file);
                return false;
            }

            bytes.assign ((std::size_t) size, '\0');
            in.read (bytes.data(), (std::streamsize) size);

            if (in.gcount() != (std::streamsize) size)           // (a file that is shorter than it said it was)
            {
                error = "cannot read all of " + nameOf (file);
                return false;
            }

            return true;
        }
    }

    fs::path projectPath (const fs::path& audioFile)
    {
        fs::path p = audioFile;
        p += ".kerf";
        return p;
    }

    fs::path backupPath (const fs::path& projectFile)
    {
        fs::path p = projectFile;
        p += ".bak";
        return p;
    }

    Read read (const fs::path& file)
    {
        Read r;
        std::error_code ec;
        const auto status = fs::status (file, ec);

        if (status.type() == fs::file_type::not_found)
            return r;

        if (ec)
        {
            r.kind = Read::Kind::damaged;
            r.error = "cannot look at " + nameOf (file) + " (" + ec.message() + ")";
            return r;
        }

        std::string bytes, error;

        if (status.type() != fs::file_type::regular || ! readFile (file, bytes, error))
        {
            r.kind = Read::Kind::damaged;
            r.error = error.empty() ? nameOf (file) + " is not a file" : error;
            return r;
        }

        auto parsed = parseProject (bytes);
        r.error = parsed.error;

        switch (parsed.status)
        {
            case ProjectParse::Status::ok:      r.kind = Read::Kind::ok; r.project = std::move (parsed.project); break;
            case ProjectParse::Status::newer:   r.kind = Read::Kind::newer; break;
            case ProjectParse::Status::damaged: r.kind = Read::Kind::damaged; break;
        }

        return r;
    }

    bool write (const fs::path& file, const std::string& text, std::string& error)
    {
        const std::string name = nameOf (file);
        fs::path part = file;
        part += ".tmp";

        std::FILE* out = openForWriting (part);

        if (out == nullptr)
        {
            error = "cannot create " + name + ".tmp (" + std::strerror (errno) + ")";
            return false;
        }

        bool ok = std::fwrite (text.data(), 1, text.size(), out) == text.size();
        ok = std::fflush (out) == 0 && ok;
        ok = flushToDisk (out) && ok;
        ok = std::fclose (out) == 0 && ok;

        std::error_code ec;

        if (! ok)
        {
            fs::remove (part, ec);
            error = "cannot write " + name + " (is the disk full?)";
            return false;
        }

        std::string why;

        if (! replaceFile (part, file, why))
        {
            fs::remove (part, ec);
            error = "cannot replace " + name + " (" + why + ")";
            return false;
        }

        return true;
    }

    bool backUp (const fs::path& file, std::string& error)
    {
        std::string bytes;

        if (! readFile (file, bytes, error))
            return false;

        return write (backupPath (file), bytes, error);
    }

    fs::path setAside (const fs::path& file)
    {
        for (int n = 1; n <= 99; ++n)
        {
            fs::path to = file;
            to += n == 1 ? std::string (".old") : ".old" + std::to_string (n);

            std::error_code ec;

            if (fs::exists (to, ec))
                continue;

            fs::rename (file, to, ec);

            if (ec)
                return {};

            return to;
        }

        return {};
    }
}
