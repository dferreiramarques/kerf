#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kerf::ui
{
    struct FileEntry
    {
        std::string name;       // UTF-8, as shown on screen
        bool isDir = false;
        bool isParent = false;  // the ".." entry
        uintmax_t size = 0;     // bytes (files only)
        bool hasProject = false;   // an audio file with a project (name.wav.kerf) next to it
    };

    // The model behind the "Load sample" and "Load project" screens: lists one folder at a time,
    // folders first and then audio files (WAV / MP3 / FLAC) - or, for the project library, project
    // files (.kerf) - hiding everything else. No SDL and no drawing, so it is unit-tested.
    class FileBrowser
    {
    public:
        enum class Result { none, navigated, chosenFile };
        enum class Kind { samples, projects };

        explicit FileBrowser (Kind kind = Kind::samples) : kind_ (kind) {}

        // Lists `dir`. On failure the previous listing is kept and error() says why.
        bool open (const std::filesystem::path& dir);

        // Windows: the list of drive letters. Elsewhere this is the same as open("/").
        bool openRoots();

        // Folder -> navigates into it. File -> returns chosenFile with its full path in `file`.
        Result activate (int index, std::filesystem::path& file);

        // Goes to the parent folder (or, on Windows, from a drive's root to the list of drives).
        // Returns false when already at the very top.
        bool goUp();

        const std::vector<FileEntry>& entries() const noexcept { return entries_; }
        const std::filesystem::path& directory() const noexcept { return dir_; }
        bool atRootsList() const noexcept { return inRoots_; }
        std::string directoryUtf8() const;
        const std::string& error() const noexcept { return error_; }

        // Index of the entry called `name` (UTF-8), or -1. Used to re-select the folder we came from.
        int indexOfName (const std::string& name) const;

        static bool isAudioFile (const std::string& name);
        static bool isProjectFile (const std::string& name);   // name.kerf (the project files this Kerf saves)

    private:
        Kind kind_ = Kind::samples;
        std::vector<FileEntry> entries_;
        std::filesystem::path dir_;
        bool inRoots_ = false;
        std::string error_;
    };
}
