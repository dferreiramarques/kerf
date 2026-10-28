#pragma once

#include <filesystem>
#include <string>

#include "input.h"

namespace kerf::ui
{
    // User settings, stored as a small human-editable "key = value" text file (kerf.cfg) next to
    // the program. Unknown lines are ignored and every value is range-checked on load, so a
    // damaged or hand-edited file can never break start-up.
    struct Config
    {
        // sound
        float masterVolume = 0.7f;   // 0..1 (the web app's default)
        int midiMode = 0;            // 0 = trigger (one note per slice), 1 = sampler (selected slice, pitched)
        int noteOffset = 60;         // MIDI note of slice 1 in trigger mode
        bool usbMidi = true;         // read USB MIDI devices that the kernel has no driver for (Linux)
        bool usbAudio = true;        // read USB microphones and audio interfaces that the kernel has no driver for (Linux)
        int audioBuffer = 512;       // frames per audio callback; smaller = lower latency
        int audioRate = 44100;

        // controls
        bool swapAB = false;         // flip A/B (a pad that reports Xbox positions vs Nintendo labels)
        bool swapXY = false;
        float stickSpeed = 0.5f;     // how fast a fully pushed stick moves a handle: view-widths per second
        float deadzone = 0.15f;      // 0.05..0.6

        // Raw-joystick fallback, used only when SDL has no controller mapping for the device.
        // Axis numbers and button numbers as reported by the "Input test" screen.
        int joyAxis[4] = { 0, 1, 2, 3 };               // left X, left Y, right X, right Y
        int joyButton[kBtnCount] = { -1, -1, -1, -1,   // Up Down Left Right (a hat is used when -1)
                                      0, 1, 2, 3,      // A B X Y
                                      4, 5, 8, 9,      // L1 R1 L2 R2
                                      7, 6 };          // Start Select

        // recording (the Record screen)
        int recordLimit = 180;       // longest take in seconds: 5..300
        int recordGainDb = 0;        // -12..30
        std::string recordInput;     // the input used last, as the Record screen names it
        std::string recordChannels;  // the channels taken from it, as the Record screen names them ("3+4  stereo")

        // display
        bool fullscreen = false;
        int windowScale = 2;         // desktop window size = 640x480 times this

        // files
        bool projects = false;       // autosave (the "autosave" key): keep the work on each sample in name.wav.kerf next to it, without being asked. Off: projects are saved and loaded by hand
        std::string lastDir;         // UTF-8

        void sanitize();
        std::string serialize() const;
        static Config parse (const std::string& text);

        static Config load (const std::filesystem::path& file);
        bool save (const std::filesystem::path& file) const;
    };
}
