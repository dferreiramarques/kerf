#include "file_browser.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "path_util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace kerf::ui
{
    namespace
    {
        std::string lowerAscii (std::string s)
        {
            for (auto& c : s)
                if (c >= 'A' && c <= 'Z')
                    c = (char) (c - 'A' + 'a');

            return s;
        }

        std::vector<std::string> listRoots()
        {
            std::vector<std::string> roots;

#ifdef _WIN32
            const DWORD mask = GetLogicalDrives();

            for (int i = 0; i < 26; ++i)
                if (mask & (1u << i))
                    roots.push_back (std::string (1, (char) ('A' + i)) + ":\\");
#else
            roots.push_back ("/");
#endif

            return roots;
        }
    }

    bool FileBrowser::isAudioFile (const std::string& name)
    {
        const auto dot = name.find_last_of ('.');
        if (dot == std::string::npos)
            return false;

        const auto ext = lowerAscii (name.substr (dot));
        return ext == ".wav" || ext == ".mp3" || ext == ".flac";
    }

    bool FileBrowser::isProjectFile (const std::string& name)
    {
        return name.size() > 5 && lowerAscii (name.substr (name.size() - 5)) == ".kerf";
    }

    std::string FileBrowser::directoryUtf8() const
    {
        return inRoots_ ? std::string ("Computer") : pathToUtf8 (dir_);
    }

    bool FileBrowser::open (const fs::path& dir)
    {
        std::error_code ec;
        fs::path target = dir.empty() ? fs::path (".") : dir;

        if (! fs::is_directory (target, ec))
        {
            error_ = "Not a folder";
            return false;
        }

        // Resolve to an absolute, tidy path so that goUp() always has somewhere to go.
        const auto abs = fs::weakly_canonical (fs::absolute (target, ec), ec);
        if (! ec)
            target = abs;

        fs::directory_iterator it (target, fs::directory_options::skip_permission_denied, ec);
        if (ec)
        {
            error_ = "Cannot open this folder";
            return false;
        }

        std::vector<FileEntry> dirs, files;
        std::set<std::string> projects;      // the names of the .kerf files met on the way: they mark the audio files they belong to

        for (const fs::directory_iterator end; it != end; it.increment (ec))
        {
            if (ec)
                break;

            const auto name = pathToUtf8 (it->path().filename());
            if (name.empty() || name[0] == '.')
                continue;

            std::error_code e2;

            if (it->is_directory (e2))
            {
                FileEntry fe;
                fe.name = name;
                fe.isDir = true;
                dirs.push_back (std::move (fe));
            }
            else if (kind_ == Kind::samples ? isAudioFile (name) : isProjectFile (name))
            {
                FileEntry fe;
                fe.name = name;
                fe.size = (uintmax_t) it->file_size (e2);
                if (e2) fe.size = 0;
                files.push_back (std::move (fe));
            }
            else if (kind_ == Kind::samples && isProjectFile (name))
            {
                projects.insert (name);
            }
        }

        if (kind_ == Kind::samples)
            for (auto& f : files)
                f.hasProject = projects.count (f.name + ".kerf") != 0;

        auto byName = [] (const FileEntry& a, const FileEntry& b)
        {
            const auto la = lowerAscii (a.name), lb = lowerAscii (b.name);
            return la != lb ? la < lb : a.name < b.name;
        };

        std::sort (dirs.begin(), dirs.end(), byName);
        std::sort (files.begin(), files.end(), byName);

        entries_.clear();

        const bool atPosixRoot = target.has_root_directory() && target == target.root_path() && target.root_name().empty();

        if (! atPosixRoot)
        {
            FileEntry up;
            up.name = "..";
            up.isDir = true;
            up.isParent = true;
            entries_.push_back (std::move (up));
        }

        entries_.insert (entries_.end(), dirs.begin(), dirs.end());
        entries_.insert (entries_.end(), files.begin(), files.end());

        dir_ = target;
        inRoots_ = false;
        error_.clear();
        return true;
    }

    bool FileBrowser::openRoots()
    {
        const auto roots = listRoots();

#ifdef _WIN32
        entries_.clear();

        for (const auto& r : roots)
        {
            FileEntry fe;
            fe.name = r;
            fe.isDir = true;
            entries_.push_back (std::move (fe));
        }

        dir_.clear();
        inRoots_ = true;
        error_.clear();
        return true;
#else
        return open (roots.empty() ? fs::path ("/") : fs::path (roots.front()));
#endif
    }

    FileBrowser::Result FileBrowser::activate (int index, fs::path& file)
    {
        if (index < 0 || index >= (int) entries_.size())
            return Result::none;

        const auto& e = entries_[(size_t) index];

        if (e.isParent)
            return goUp() ? Result::navigated : Result::none;

        if (inRoots_)
            return open (pathFromUtf8 (e.name)) ? Result::navigated : Result::none;

        if (e.isDir)
            return open (dir_ / pathFromUtf8 (e.name)) ? Result::navigated : Result::none;

        file = dir_ / pathFromUtf8 (e.name);
        return Result::chosenFile;
    }

    bool FileBrowser::goUp()
    {
        if (inRoots_)
            return false;

        const auto parent = dir_.parent_path();

        if (parent.empty() || parent == dir_)
        {
#ifdef _WIN32
            return openRoots();
#else
            return false;
#endif
        }

        return open (parent);
    }

    int FileBrowser::indexOfName (const std::string& name) const
    {
        for (size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].name == name)
                return (int) i;

        return -1;
    }
}
