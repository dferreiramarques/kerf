#pragma once

// Mirrors the slice object in kerf.html (state.slices[i]): start/end in seconds, pitch as a
// playbackRate-style multiplier (1 = original), volume 0-1, pan -1..1, fades in ms.
struct Slice
{
    enum class Mode { shot, loop };

    float start = 0.0f;
    float end = 0.0f;
    float pitch = 1.0f;
    float volume = 1.0f;
    float pan = 0.0f;
    bool enabled = true;
    Mode mode = Mode::shot;
    float fadeInMs = 1.0f;
    float fadeOutMs = 50.0f;
};
