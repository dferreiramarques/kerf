#pragma once

#include <cstdint>

namespace kerf::ui
{
    // The six pages of the controls help: gamepad / slicer, gamepad / sequencer, keyboard /
    // slicer, keyboard / sequencer, then gamepad and keyboard for the pages that get a sample
    // or a project (REC, LOAD and PROJ). They are plain data so a test can check that every line fits the
    // 640-pixel screen (the help draws keys and descriptions in the 2x font, 16 px per
    // character, and the notes in the 1x font, 8 px per character).
    enum class HelpTone : uint8_t
    {
        normal,
        start,   // the left stick / START handle colour
        end,     // the right stick / END handle colour
        dim,     // modifiers: L2 / R2, Ctrl / Shift
        page     // the "next page" line
    };

    struct HelpRow
    {
        const char* keys;
        const char* what;
        HelpTone tone;
    };

    struct HelpPage
    {
        const char* title;
        const HelpRow* rows;
        int rowCount;
        const char* note;
    };

    constexpr int kHelpPages = 6;
    constexpr int kHelpPadHome = 4;        // the pages for REC / LOAD / PROJ: the gamepad's, then the keyboard's
    constexpr int kHelpKeyboardHome = 5;
    constexpr int kHelpKeyChars = 10;    // widest key text (2x font); the description starts one character later
    constexpr int kHelpWhatChars = 27;   // widest description (2x font)
    constexpr int kHelpNoteChars = 77;   // widest note (1x font)

    inline const HelpPage& helpPage (int index)
    {
        static const HelpRow padSlicer[] = {
            { "L-STICK",    "move the START handle",       HelpTone::start },
            { "  up/down",  "zoom around START",           HelpTone::start },
            { "R-STICK",    "move the END handle",         HelpTone::end },
            { "  up/down",  "zoom around END",             HelpTone::end },
            { "L2 / R2",    "fine / coarse (held)",        HelpTone::dim },
            { "D-PAD",      "up/dn pick, l/r change",      HelpTone::normal },
            { "A",          "play the slice (hold)",       HelpTone::normal },
            { "X",          "add slice at view centre",    HelpTone::normal },
            { "Y",          "auto-detect slices",          HelpTone::normal },
            { "L1 / R1",    "previous / next slice",       HelpTone::normal },
            { "B",          "play / stop the sequencer",   HelpTone::normal },
            { "SELECT",     "next page: the sequencer",    HelpTone::page },
            { "SEL + X",    "delete slice",                HelpTone::normal },
            { "SEL + B",    "undo",                        HelpTone::normal },
            { "START",      "menu: save/load project",     HelpTone::normal },
        };

        static const HelpRow padSequencer[] = {
            { "D-PAD",      "move the cursor",             HelpTone::normal },
            { "A",          "step on / off",               HelpTone::normal },
            { "A + D-PAD",  "paint steps while you move",  HelpTone::normal },
            { "X",          "erase a step (hold: paint)",  HelpTone::normal },
            { "Y",          "hear the row's slice (hold)", HelpTone::normal },
            { "B",          "play / stop",                 HelpTone::normal },
            { "L1 / R1",    "previous / next scene",       HelpTone::normal },
            { "L-STICK",    "tempo: up = faster",          HelpTone::start },
            { "R-STICK",    "swing: up = more",            HelpTone::end },
            { "L2 / R2",    "fine / coarse (held)",        HelpTone::dim },
            { "SEL + L/R",  "pattern length",              HelpTone::normal },
            { "SEL + X",    "clear the row",               HelpTone::normal },
            { "SEL + B",    "undo",                        HelpTone::normal },
            { "SELECT",     "next page: the slicer",       HelpTone::page },
            { "START",      "menu: save/load, scenes",     HelpTone::normal },
        };

        static const HelpRow keyboardSlicer[] = {
            { "Z / X",      "move the START handle",       HelpTone::start },
            { "C / V",      "zoom in/out around START",    HelpTone::start },
            { "N / M",      "move the END handle",         HelpTone::end },
            { ", / .",      "zoom in/out around END",      HelpTone::end },
            { "Ctrl/Shift", "fine / coarse (held)",        HelpTone::dim },
            { "Arrows",     "up/dn pick, l/r change",      HelpTone::normal },
            { "Space",      "play the slice (hold)",       HelpTone::normal },
            { "1",          "add slice at view centre",    HelpTone::normal },
            { "2",          "auto-detect slices",          HelpTone::normal },
            { "Q / R",      "previous / next slice",       HelpTone::normal },
            { "Esc",        "play / stop the sequencer",   HelpTone::normal },
            { "3",          "next page: the sequencer",    HelpTone::page },
            { "Delete",     "delete slice",                HelpTone::normal },
            { "Backspace",  "undo",                        HelpTone::normal },
            { "Tab",        "menu: save/load (F1 help)",   HelpTone::normal },
        };

        static const HelpRow keyboardSequencer[] = {
            { "Arrows",     "move the cursor",             HelpTone::normal },
            { "Space",      "step on / off (hold: paint)", HelpTone::normal },
            { "1",          "erase a step (hold: paint)",  HelpTone::normal },
            { "2",          "hear the row's slice (hold)", HelpTone::normal },
            { "Esc",        "play / stop",                 HelpTone::normal },
            { "Q / R",      "previous / next scene",       HelpTone::normal },
            { "C / V",      "tempo up / down",             HelpTone::start },
            { ", / .",      "swing more / less",           HelpTone::end },
            { "Ctrl/Shift", "fine / coarse (held)",        HelpTone::dim },
            { "3 + Arrows", "pattern length (left/right)", HelpTone::normal },
            { "Delete",     "clear the row",               HelpTone::normal },
            { "Backspace",  "undo",                        HelpTone::normal },
            { "3",          "next page: the slicer",       HelpTone::page },
            { "Tab",        "menu: save/load, scenes",     HelpTone::normal },
            { "F1",         "this help",                   HelpTone::normal },
        };

        static const HelpRow padHome[] = {
            { "SELECT",     "next: REC > LOAD > PROJ",     HelpTone::page },
            { "REC",        "the page Kerf starts on",     HelpTone::dim },
            { "A",          "start / stop the take",       HelpTone::normal },
            { "L1 / R1",    "previous / next input",       HelpTone::normal },
            { "D-PAD",      "up/dn row, l/r change",       HelpTone::normal },
            { "Y",          "try the input again",         HelpTone::normal },
            { "B",          "back to the sample",          HelpTone::normal },
            { "LOAD",       "the library of samples",      HelpTone::dim },
            { "PROJ",       "the library of projects",     HelpTone::dim },
            { "A",          "open folder / load file",     HelpTone::normal },
            { "B",          "up a folder, then back",      HelpTone::normal },
            { "D-PAD",      "move the selection",          HelpTone::normal },
            { "L1 / R1",    "page up / page down",         HelpTone::normal },
            { "START",      "back (menu: no sample)",      HelpTone::normal },
            { "SEL+START",  "hold 1 s: quit Kerf",         HelpTone::normal },
        };

        static const HelpRow keyboardHome[] = {
            { "3",          "next: REC > LOAD > PROJ",     HelpTone::page },
            { "REC",        "the page Kerf starts on",     HelpTone::dim },
            { "Space",      "start / stop the take",       HelpTone::normal },
            { "Q / R",      "previous / next input",       HelpTone::normal },
            { "Arrows",     "up/dn row, l/r change",       HelpTone::normal },
            { "2",          "try the input again",         HelpTone::normal },
            { "Esc",        "back to the sample",          HelpTone::normal },
            { "LOAD",       "the library of samples",      HelpTone::dim },
            { "PROJ",       "the library of projects",     HelpTone::dim },
            { "Enter",      "open folder / load file",     HelpTone::normal },
            { "Esc",        "up a folder, then back",      HelpTone::normal },
            { "Arrows",     "move the selection",          HelpTone::normal },
            { "PgUp/PgDn",  "page up / page down",         HelpTone::normal },
            { "Tab",        "back (menu: no sample)",      HelpTone::normal },
            { "F11",        "full screen on / off",        HelpTone::normal },
        };

        static const HelpPage pages[kHelpPages] = {
            { "CONTROLS: GAMEPAD - SLICER",       padSlicer,         15,
              "USB MIDI keyboards play the slices.  Hold SELECT + START for 1 s to quit." },
            { "CONTROLS: GAMEPAD - SEQUENCER",    padSequencer,      15,
              "The pattern keeps playing on the slicer page.  Row N plays slice N." },
            { "CONTROLS: KEYBOARD - SLICER",      keyboardSlicer,    15,
              "Piano: A W S E D F T G Y H U J K O L P play slices 1-16" },
            { "CONTROLS: KEYBOARD - SEQUENCER",   keyboardSequencer, 15,
              "Each scene is its own pattern; a new scene starts at the next loop." },
            { "CONTROLS: GAMEPAD - REC/LOAD/PROJ", padHome,          15,
              "After a take: A saves and loads it, X saves and stays, B throws it away." },
            { "CONTROLS: KEYBOARD - REC/LOAD/PROJ", keyboardHome,    15,
              "After a take: Space saves+loads it, 1 saves it, Esc throws it away." },
        };

        return pages[index < 0 ? 0 : index >= kHelpPages ? kHelpPages - 1 : index];
    }
}
