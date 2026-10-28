#pragma once

// A Kerf project: everything a person makes from one sample - the slices with all their
// parameters, the eight scenes of step patterns, the tempo, the swing and the pattern length - as
// a plain value that can be compared, written to a small text file and read back.
//
// The file is text on purpose (a person can open it, and a damaged one can be understood), one
// line per fact, and read strictly: a file that is not exactly what Kerf writes is "damaged" and is
// never half-applied. See project.cpp for the format.
//
// This header and project.cpp know nothing about files on disk (the handheld's ProjectStore does
// that) or about the screen; captureProject() and applyProject() are the only two functions that
// touch the Engine, and they are for the UI thread.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "slice.h"
#include "step_sequencer.h"

namespace kerf
{
    class Engine;
    struct Sample;

    struct Project
    {
        static constexpr int kFormatVersion = 1;                         // what this Kerf writes and reads
        static constexpr int kMaxSlices = 50000;                         // far beyond anything a sample of a few minutes holds
        static constexpr std::size_t kMaxFileBytes = 16u * 1024u * 1024u;

        // The sample it was made for. Only the length and the rate are checked when it is loaded
        // again; the name is there for people and for the log.
        std::string audio;
        std::int64_t frames = 0;
        std::int64_t rate = 0;

        // The work. Indices count from 0 here (the file and the screen count from 1).
        std::vector<Slice> slices;
        int selected = 0;
        float tempo = StepSequencer::kDefaultTempo;
        float swing = StepSequencer::kMinSwing;
        int length = StepSequencer::kSteps;
        int scene = 0;
        StepSequencer::Grid grid;

        // Is `sample` the audio this project was made for (same number of frames, same rate)?
        bool belongsTo (const Sample& sample) const noexcept;

        // Brings every number into the range the controls allow, so that whatever a file said,
        // the engine only ever sees values it can play: slices lie inside the sample and run
        // forwards, pitch, volume, pan and fades are in range, a pattern row exists only where
        // a slice does. `durationSeconds` is the length of the sample. Idempotent.
        void sanitize (double durationSeconds) noexcept;

        // Rows with at least one step lit, over all scenes (the lines a file holds for the patterns).
        int patternCount() const noexcept;
    };

    // Is this the same work? The slice cursor and the scene being looked at are left out: moving
    // around is not editing, and must not make a project look "changed".
    bool sameWork (const Project& a, const Project& b) noexcept;

    // The text of a project file. Returns false (and an empty text) for a project that could not be
    // read back: more than Project::kMaxSlices slices, or a number that is not a number.
    bool serializeProject (const Project& project, std::string& text);

    struct ProjectParse
    {
        enum class Status
        {
            ok,        // `project` is complete
            newer,     // written by a Kerf with a newer format: leave the file alone
            damaged    // anything else that is not a project file this Kerf wrote; `error` says where
        };

        Status status = Status::damaged;
        int version = 0;
        std::string error;
        Project project;
    };

    ProjectParse parseProject (const std::string& text);

    // The state of the engine as a project (UI thread). `selected` is the engine's selected slice.
    Project captureProject (Engine& engine);

    // Puts a project into the engine: the slices, the patterns, tempo, swing, length, scene and the
    // selected slice. Returns false, touching nothing, when the loaded sample is not the one the
    // project was made for. The numbers go through Project::sanitize first. Call it with the
    // sequencer stopped (right after a sample was loaded).
    bool applyProject (Engine& engine, const Project& project);
}
