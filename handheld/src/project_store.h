#pragma once

// The project files on disk: where a project lives, how it is read, and how it is written without
// ever putting a good file at risk. No screen, no engine - the logic of when to save is the
// ProjectKeeper's (project_keeper.h).
//
//   rec-001.wav            the sample
//   rec-001.wav.kerf       its project (the sample's whole file name plus .kerf, so that kick.wav and
//                          kick.mp3 in one folder do not share one)
//   rec-001.wav.kerf.bak   the project as it was when the sample was opened (made before the first save)
//   rec-001.wav.kerf.old   a project that could not be used, set aside rather than overwritten
//   rec-001.wav.kerf.tmp   exists only while a save is being written

#include <filesystem>
#include <string>

#include "project.h"

namespace kerf::ui::ProjectStore
{
    // rec-001.wav -> rec-001.wav.kerf
    std::filesystem::path projectPath (const std::filesystem::path& audioFile);

    // rec-001.wav.kerf -> rec-001.wav.kerf.bak
    std::filesystem::path backupPath (const std::filesystem::path& projectFile);

    struct Read
    {
        enum class Kind
        {
            missing,   // there is no such file
            ok,        // `project` is what it holds
            newer,     // written by a newer Kerf: not to be read, nor touched
            damaged    // there, but not a project this Kerf can read (or cannot be read at all); `error` says why
        };

        Kind kind = Kind::missing;
        Project project;
        std::string error;
    };

    Read read (const std::filesystem::path& file);

    // Replaces `file` with `text`, all or nothing: the text is written to file.tmp, flushed to the
    // disk, and only then renamed over the file, so that a power cut or a full disk leaves the old
    // file as it was. Returns false (and says why in `error`) when it could not be done; file.tmp
    // does not stay behind.
    bool write (const std::filesystem::path& file, const std::string& text, std::string& error);

    // Copies `file` to file.bak (the same way: the old backup is replaced only by a complete one).
    bool backUp (const std::filesystem::path& file, std::string& error);

    // Renames a project that cannot be used to file.old, or file.old2 ... if that is taken, and
    // returns the new path; empty when it could not be done.
    std::filesystem::path setAside (const std::filesystem::path& file);
}
