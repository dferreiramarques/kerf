# Kerf for handhelds

Kerf, the sample slicer, on a handheld game console such as the Anbernic RG35XX H (640x480
screen, two analog sticks). **The left stick moves the START of the selected slice, the right
stick moves its END.** The same program also runs on a PC (Windows, Linux) with a keyboard or any
game controller; that is how it is developed and tested.

Status: version 0.5.0. Kerf has two sets of pages, and **Select goes round the pages of the set
you are in**. It starts on *Record* (REC), which takes sound from a USB microphone, a
wireless-microphone receiver or an audio interface, saves it as a WAV file in the samples folder
and loads it into the slicer; the library of samples (LOAD), where a sample is picked from a
folder, and the library of projects (PROJ), where a saved project is picked, are the pages behind
it (REC, LOAD, PROJ, REC ...). With a sample in hand the other pair works on it: the *Slicer* (detect slices,
move their start and end, play them from the pad, the keyboard or a MIDI keyboard, undo) and the
*Sequencer* (a 16-step pattern sequencer for the slices: one row per slice, eight scenes, tempo,
swing and pattern length). **New in 0.5.0: projects are saved and loaded by hand.** Start menu >
**Save project** writes the slices with all their parameters, the patterns of the eight scenes,
the tempo, the swing and the pattern length into a small project file next to the sample
(`demo_beat.wav` gets `demo_beat.kerf`); the PROJ page (Select, twice from REC) or Start menu >
**Load project...** brings a project back with its sample (see "Projects"). The effects of the web app are not ported yet,
and the web app's own project (.zip) files are not opened (see the roadmap). The slicer (0.1.0),
the two pages with the sequencer (0.2.0), the USB MIDI reading of 0.2.1 and **the recording from
USB (0.3.0, with a generic wireless lavalier microphone on the USB-C port)** have been used on a
real Anbernic RG35XX H (muOS 2606.0) by its author, who reports that they work. Not tried on the
device yet: a Tascam Model 12 or another audio interface, the start page of 0.3.1, and the
projects (0.4.0's autosave, which for its author did not bring slices back on the device, and the
saving by hand of 0.5.0); they have only been tested on a PC and in emulators (the USB parts also against a pretend USB kernel; see "What has been tested"). The
Windows package has been tried by its author on a PC. 0.3.0 added Kerf's own reading of USB
microphones and audio interfaces for handhelds whose kernel has no driver for them.

## Folders

| Folder | Contents |
|--|--|
| `../core` | The engine: samples, slices, transient detection, voices, the step sequencer, MIDI parsing, and the project (what is saved of a sample's work, and the text file that holds it). No user interface, no audio-device code, no JUCE. |
| `src` | The handheld interface (`App`, drawn into a 640x480 software canvas), the input layer (`PadTracker`, `SdlInput`), MIDI input, audio input and recording (the Record page, the system's microphone through SDL, Kerf's own USB audio reader), the project files (`ProjectStore` reads and writes them, `ProjectKeeper` decides when) and the SDL2 program (`main.cpp`). |
| `tests` | Unit tests, a test of the SDL input with virtual joysticks, and end-to-end scripts (`tests/scripts`). |
| `portmaster` | Launcher and metadata for PortMaster. |
| `windows` | Notes that go into the Windows zip. |
| `tools` | `build-release.sh`, which builds both packages. |
| `../third_party` | The audio decoders and the font (see its README). |

The web app (`kerf.html`) and Kerf Lite (`plugin/`) are not touched.

## Build and test on a PC

You need a C++20 compiler, CMake 3.16 or newer and, for the program itself, the SDL2 development
files (Debian/Ubuntu: `libsdl2-dev`; MSYS2: `mingw-w64-ucrt-x86_64-SDL2`).

```
cmake -S handheld -B build/handheld
cmake --build build/handheld
ctest --test-dir build/handheld --output-on-failure
build/handheld/kerf          # kerf.exe on Windows
```

Without SDL2, configure with `-DKERF_BUILD_HOST=OFF`: you still get `kerf_headless` and most of
the tests. `kerf_headless --script file.txt` runs a script that presses buttons, pushes sticks,
checks the result and can save screenshots; the scripts in `tests/scripts` show the commands.

Command line of `kerf`: `--fullscreen`, `--windowed`, `--scale N` (window = 640x480 times N),
`--config FILE`, `--no-audio`, `--no-midi`, `--no-input` (no audio input for the Record page),
`--fake-input` (pretend inputs, to try the Record page on a machine without a microphone) and a
sample to open (Kerf then starts on the slicer instead of on the Record page). `kerf_headless` also
has `--fake-input`, `--start-dir DIR` for the folder that the library opens and the takes are
saved in, and `--projects`: without it the scripts do not save projects, so that a test leaves no
file next to its sample. Scripts can `reload` the open sample, `expect project
none|saved|edited|off|problem` (the tag in the header), `expect file <name> yes|no` (a file next to
the open sample) and `print project`.

## Controls

Select (a tap) goes round the pages of a set: REC, LOAD and PROJ (Kerf starts on REC; REC > LOAD >
PROJ > REC), or SLICER and SEQ. The header shows the set with the page you are on lit. The
sequencer keeps playing on all of them.

**Slicer page**

| Pad | Keyboard | |
|--|--|--|
| Left stick | Z / X | move the START of the selected slice (stick up/down, C / V: zoom around START) |
| Right stick | N / M | move the END (stick up/down, `,` / `.`: zoom around END) |
| L2 / R2 (held) | Ctrl / Shift | fine (x0.1) / coarse (x4) |
| D-pad | arrows | up/down pick a row, left/right change the value |
| A | Space | play the slice (hold) |
| X | 1 | add a slice at the centre of the view |
| Y | 2 | auto-detect slices |
| L1 / R1 | Q / R | previous / next slice |
| B | Esc | play / stop the sequencer |
| Select (tap) | 3 | the sequencer page |
| Select + X | Delete | delete the slice |
| Select + B | Backspace | undo |
| Start | Tab | menu (**save project, save as new project, load project**, load sample, **record**, detect, add, delete, clear, undo, **zoom**, go to sequencer, settings, help); "Load sample...", "Record sample..." and "Load project..." open LOAD, REC and PROJ |

**Sequencer page**

| Pad | Keyboard | |
|--|--|--|
| D-pad | arrows | move the cursor (row = slice, column = step) |
| A | Space | switch the step on / off (hold A and move the D-pad to paint a run) |
| X | 1 | erase the step (hold X and move to erase a run) |
| Y (held) | 2 (held) | hear the slice of the row |
| B | Esc | play / stop |
| L1 / R1 | Q / R | previous / next scene (it starts with the next loop) |
| Left stick up/down | C / V | tempo, 40 to 300 BPM |
| Right stick up/down | `,` / `.` | swing, 50% to 75% |
| L2 / R2 (held) | Ctrl / Shift | fine (x0.1) / coarse (x4) tempo and swing |
| Select + Left/Right | 3 + Left/Right | pattern length, 1 to 16 steps |
| Select + X | Delete | clear the row |
| Select + B | Backspace | undo |
| Select (tap) | 3 | the slicer page |
| Start | Tab | menu (play / stop, tempo, swing, length, scene, copy scene to next, clear row, clear scene, undo, **save project, save as new project, load project**, go to slicer, settings, help) |

**Record page, REC** (Kerf starts here; see "Recording" below)

| Pad | Keyboard | |
|--|--|--|
| Select (tap) | 3 | the library of samples, LOAD (not while a take is being made or is in hand) |
| D-pad up/down | arrows | pick a row: Input, Channels, Gain |
| D-pad left/right | arrows | change the row (a held button repeats for Channels and Gain, not for Input) |
| L1 / R1 | Q / R | previous / next input |
| A | Space | start the take; stop it |
| Y | 2 | try the input again (after an error) |
| B | Esc | back to the sample being worked on (before there is one there is nowhere to go back to; with a take in hand, B throws it away) |
| Start | Tab | the same as B; before there is a sample, the menu: settings, help, input test, quit |
| After a take: A | Space | save it and load it into the slicer |
| After a take: X | 1 | save it and stay for another take |
| After a take: B | Esc | throw it away |

**Library page, LOAD** (Select from REC, or Start > Load sample...): up/down pick a file or folder,
A opens it, B goes up a folder (and, at the top, back to the sample being worked on), L1 / R1 page
up / down, **Select (key 3) the PROJ page**, Start (Tab) back to the sample being worked on, or
the menu before there is one.

**Project library page, PROJ** (Select from LOAD, or Start > Load project...): the same, but it
lists folders and project files (`.kerf`); A on a project opens it (see "Projects"), and **Select
(key 3) goes on to the Record page**. It starts in the folder of the sample in hand (where the
projects are saved) and stays where you take it.

In every menu: B or Start closes it. Hold Select + Start for 1 s to quit. F1 shows the help,
F11 or Alt+Enter toggles full screen. `A W S E D F T G Y H U J K O L P` play slices 1 to 16, and
so does a USB MIDI keyboard (Settings: trigger or sampler mode).

## The sequencer

- **Rows are slices.** Row N plays slice N, so the grid has as many rows as there are slices (the
  first 128). Adding, deleting, detecting or undoing slices moves the patterns with their slices;
  a new slice starts with an empty row.
- **Steps are sixteenth notes**, 16 of them, from 1 to 16 steps long (Select + Left/Right). The
  tempo is 40 to 300 BPM (120 to start with) and the swing 50% (straight) to 75%.
- **Eight scenes**, each with its own pattern. Changing scene while it plays waits for the end of
  the pattern, so it always lands on the beat; "Copy scene to next" in the menu starts a variation.
- **One-shot slices start again on every lit step. Loop slices are gated**: they start on the first
  lit step of a run and are released on the first unlit step after it, so a run of lit steps is one
  held note. Disabled slices stay silent. Stopping lets go of what the sequencer started, with each
  slice's own fade-out, and leaves the sounds you are playing by hand alone.
- Steps are placed on their exact sample: the same pattern renders to the same audio whatever the
  audio buffer size is, and the tempo does not drift.
- Loading another sample stops the sequencer and clears the patterns. Tempo, swing and length stay,
  unless that sample has a project, which brings back its own (see "Projects"). The patterns are
  part of the project: they are saved with the slices and come back with them.

## Recording

Kerf **starts on the Record page** (REC); **Start > Record sample...** opens it again from the
slicer, and Select flips it with the library (LOAD), which is the page behind it. It takes sound
from a microphone or a line input and turns the take into a sample. The input opens as soon as the
page is up (so the meters move at once) and closes when the page is left.

- **Input.** L1 / R1 (Q / R), or Left / Right on the first row, choose where the sound comes from.
  The inputs that Kerf reads itself over USB come first (on a handheld, whatever was plugged in),
  then the recording devices the system lists (on a PC, its microphones and line inputs). The input
  used last time is opened again. A microphone plugged in while the screen is up is found within a
  couple of seconds; one that is unplugged during a take keeps what was recorded.
- **Channels.** Every channel of the input has its own level meter, with a peak hold in dB and a
  mark when it reaches full scale; the channels that will be recorded are lit. A stereo input
  offers the pair, each side on its own, and the mix of both; a mixer or audio interface offers its
  pairs (1+2, 3+4, ...) and then each channel on its own. A take is stereo or mono, 16-bit, at the
  sample rate the input runs at (usually 48000 or 44100 Hz).
- **Gain.** -12 to +30 dB in steps of 3, applied to the meters and to the take. "Too loud - lower
  the gain" shows while the recorded channels reach full scale (the take is limited, never wrapped
  around), and a take that clipped says so when it stops.
- **A** starts the take, **A** or **B** stops it. It stops by itself at the limit (Settings >
  Record limit: 30 s, 1, 2, 3 or 5 minutes; 3 minutes to start with). A take shorter than a quarter
  of a second is thrown away.
- **After a take**: **A** saves it and loads it into the slicer, **X** saves it and stays for
  another take, **B** throws it away. The file is the next `rec-001.wav`, `rec-002.wav` ... in the
  samples folder (the `samples` folder next to the program, or the program's folder when there is
  none; the top of the screen says which). A file is never overwritten, and it is written under a
  temporary name and renamed when it is complete. A recorded sample is a sample like any other: it
  is loaded again from the library.
- **Select** goes to the library of samples, except while a take is being made or is in hand (a take is never
  lost by accident: the screen says "Stop the take first" or "Save or discard the take first").
  **B** (or Start) goes back to the sample being worked on; before there is one, there is nothing
  to go back to, and Start opens a small menu with the settings (the longest take is there), the
  help, the input test and the way out.
- If there is no input, the screen says so and Kerf looks again by itself. Not monitored: the sound
  of the input is not played through the speakers while recording.

What the screen remembers (the input, the channels, the gain) is in `kerf.cfg`.

## Projects

**A project is a file you save and load yourself.** On the slicer or the sequencer, open the Start
menu (Tab on the keyboard) and choose **Save project**. Kerf writes a small text file next to the
sample: `demo_beat.wav` gets `demo_beat.kerf`. It holds the slices with all their parameters
(start, end, pitch, volume, pan, one-shot or loop, fade in and out, on or off), the patterns of
the eight scenes, the tempo, the swing, the pattern length, the scene being edited and the
selected slice. It does not hold the undo history, the zoom or the settings, and it does not hold
the audio: the project names its sample and Kerf finds it again next to the project.

- **Saving.** "Save project" writes the project in hand: the one you opened or saved before. With
  none yet, it makes a new file next to the sample, `demo_beat.kerf`, or `demo_beat 2.kerf`,
  `demo_beat 3.kerf` ... if that name is taken: saving a *new* project never replaces another one.
  "Save as new project" does the same even when a project is in hand. The screen says "Saved
  demo_beat.kerf (12 slices)" or "Could not save: <reason>" (the reason is also in
  `kerf-log.txt`), and the Start menu shows the name of the project in hand at its foot.
- **Loading.** The PROJ page (Select, twice from REC; or Start > Load project...) lists the folders
  and the `.kerf` files; A opens one. Kerf finds the sample the project names (next to the project
  first, then in the samples folder), loads it and puts the project in: "Opened demo_beat.kerf:
  12 slices". If the sample is not there the screen says "Sample not found: <name>"; a project
  made for audio of another length or sample rate is not applied ("Project does not fit
  <name>") and the sample opens as it is.
- **The header says how things stand**, in the corner above the output meter: SAVED (the file has
  the work as it is), EDITED (a project in hand has changed since it was saved or opened), UNSAVED
  (a sample with no project yet has been worked on) and NO SAVE (the last save failed). The Start
  menu's "Save project" shows the same word.
- **Unsaved work is not thrown away by accident.** Loading another sample or project, saving a
  take with "save and load", dropping a file on the window and "Quit" in the menu all drop the work
  in hand. When it has not been saved, the first try only says "Unsaved changes - repeat to
  discard" and does nothing; the same again within 8 seconds goes through. (Holding Select + Start
  to quit does not ask.)
- **A save cannot spoil a good file.** The project is written to `demo_beat.kerf.tmp`, flushed to
  the disk, and only then renamed over the old one, so that a power cut or a full disk leaves the
  previous project as it was. Each save keeps the file it replaces as `demo_beat.kerf.bak`.
- **Which sample a project belongs to** is decided by the number of frames and the sample rate of
  the audio, not by its name (the name inside is for finding the sample, for people and for the
  log).
- **Autosave** (optional, off to start with): Settings > Autosave, or `autosave = 1` in `kerf.cfg`.
  It is the 0.4.0 way of working, kept for those who want it: Kerf also keeps the work on each
  sample by itself in `rec-001.wav.kerf` (the whole file name plus `.kerf`) next to the sample,
  saves it a moment after you stop (1.5 s, and at the latest every 10 s of steady work) and puts it
  back when that sample is loaded again ("Project restored: 12 slices"); the library (LOAD)
  marks those samples with a small KERF label, and a project it cannot use (damaged, or for
  other audio) is set aside as `.old` rather than overwritten. Those files are projects like any
  other: the PROJ page lists them. Autosave was on in 0.4.0; it is off now because the saving by
  hand is clearer: the work is where you put it.

The file is plain text, one line per fact, with a few comment lines at the top, so that it can be
read (and mended) in any editor. Kerf reads it strictly: a file that is not exactly what Kerf
writes is "damaged" and is never half-applied, while numbers that are out of range (a slice that
ends after the sample, a tempo of 9000) are brought into range. Numbers are written the shortest
way that reads back as the very same value, and nothing in the file depends on the system, so a
project can travel with its sample between the PC and the handheld. It is Kerf's own format: the
project files of the web app are a different thing (see the roadmap).

## Settings and files

`kerf.cfg` and `kerf-log.txt` are written next to the program, so a handheld install is one
self-contained folder. `kerf.cfg` is a small `key = value` file; every value is range-checked on
load. The Settings screen covers volume, MIDI mode, the note of slice 1, stick speed and dead
zone, A/B and X/Y swap, the longest take of the Record page, the autosave
(`autosave`, off to start with) and the audio buffer. The Record screen writes `record_input`,
`record_channels` and `record_gain` (what it was left on); `usb_audio` and `usb_midi` turn Kerf's
own USB reading off.

Pads that SDL knows are used as standard game controllers (on a handheld, PortMaster passes the
mapping in `SDL_GAMECONTROLLERCONFIG`). A pad SDL has no mapping for is read as a plain joystick,
with the axis and button numbers `joy_axis_lx` ... and `joy_btn_a` ... of `kerf.cfg`; Menu >
Input test shows the numbers. A `gamecontrollerdb.txt` next to the program is added to SDL's
mappings.

On Linux the program reads raw MIDI devices (`/dev/snd/midiC*D*`, scanned every second, so USB
keyboards can be plugged in at any time); `KERF_MIDI_DEVICE=path[:path]` adds extra sources. On
Windows it uses WinMM.

Whether a handheld's kernel has the USB audio / MIDI driver depends on the firmware, and muOS
2606.0 (Linux 4.9.170) has not: a USB MIDI keyboard is on the bus but has no `/dev/snd/midi*`. So,
**for a class-compliant USB MIDI device that no kernel driver has taken, Kerf reads the device
itself** (`src/usb_midi.h`, `src/usb_midi_io.h`, `src/midi_input.cpp`): it finds the MIDI Streaming interface in
`/sys/bus/usb/devices/*/descriptors`, opens `/dev/bus/usb/<bus>/<device>` (making the node first if
`/dev` lacks it), claims the interface, keeps two bulk (or interrupt) IN transfers queued on its
input endpoint and decodes the 4-byte USB-MIDI packets, one parser per cable. It needs no driver, no
module and no change to the firmware, and it needs the permission to open the device node (a
handheld's launcher runs as root). `usb_midi = 0` in `kerf.cfg` turns it off. `kerf-log.txt` has a
line about every USB device it looked at and about each step it took with a MIDI one. Devices with a
vendor-specific protocol (not class-compliant) are not handled.

**USB audio input works the same way** (`src/usb_audio.h`, `src/usb_audio_io.h`,
`src/usb_audio_input.cpp`): for a USB microphone, a wireless-microphone receiver or an audio
interface that no kernel driver has taken, Kerf reads the descriptors from sysfs, picks the
AudioStreaming interface that records (Audio Class 1 or 2; class 3 is not handled) and the format
(48000 Hz, else 44100 Hz; the most channels, then 24 bit, then 16 bit), claims the interface,
sets the sample rate (on the endpoint for Audio Class 1, on the clock source for Audio Class 2),
selects the alternate setting and keeps isochronous transfers queued on the input endpoint. The
samples (8, 16, 24 and 32 bit PCM or 32 bit float, any number of channels) go through a lock-free
ring to the Record page. It needs the same permission as the MIDI reading, and a device that has
more than one USB mode is only seen in the mode it is in (a Tascam Model 12 has to work as the USB
audio interface, not in the STORAGE mode of its menu, which shows its SD card to the computer; its
manual advises connecting it directly, not through a hub). `usb_audio = 0` in `kerf.cfg` turns it
off, and `kerf-log.txt` has `audio:` lines with every USB audio device it saw (its formats and its
raw descriptors in hex) and every step it took, which is what to send when a device is not
recognised.
On a PC, and wherever the kernel does have the driver, the system's recording devices are used
through SDL instead.

## Packages

`bash tools/build-release.sh` (Linux, or WSL on Windows) builds
`dist/Kerf-handheld-<version>-windows-x64.zip` and
`dist/Kerf-handheld-<version>-portmaster-aarch64.zip`, with Zig as the compiler for both targets
(no cross toolchain needed). The header of the script lists what it needs. The PortMaster package
is `Kerf.sh` plus a `kerf` folder; where they go depends on the firmware (see
`portmaster/README.md`).

## What has been tested

**Version 0.5.0 is the exception to everything below.** It was written quickly and checked only
by hand: one scripted session on Linux x86-64 (save a project, change the work, meet the warning
about unsaved changes, open the project again from the PROJ page, go round REC, LOAD and PROJ
with Select), with the screenshots looked at, and a compile of everything. The test suites, the
sanitizer runs and the Wine and qemu runs described next are those of 0.4.0: **they were not
updated for 0.5.0**, and those that expect the 0.4.0 behaviour (autosave on to start with, the
`projects` key of `kerf.cfg`, Select going from LOAD back to REC, the entries of the menus, the
help pages) fail or no longer apply. There are no tests yet of the new code (the project library,
saving and opening a project, the unsaved-changes warning). The same scripted session also ran on
the shipped Windows build (under Wine) and the shipped aarch64 build (under qemu) and wrote the
same project file as the Linux build; nothing else of 0.5.0 was run on them, and nothing was
tried on the RG35XX H.

Everything else was tested on a Linux x86-64 machine, with Wine and qemu standing in for the other
systems. The slicer (0.1.0), the sequencer page (0.2.0), the USB MIDI reading (0.2.1) and the
recording from USB (0.3.0, with a generic wireless lavalier microphone) have been used on a real
RG35XX H by its author, who reports that they work. A Tascam Model 12, the start page of 0.3.1 and
the projects (the autosave of 0.4.0, which did not bring slices back on the device, and the saving
by hand of 0.5.0) have not been tried successfully on the device. The Windows package has been
tried by its author on a PC, where the MIDI input (WinMM) worked; it has not been built with MSVC.

- Linux x86-64: 119 engine tests, 31 interface tests, 53 USB MIDI tests, 103 USB audio tests, 78
  recording tests, 42 tests of the Record page, 107 tests of the projects (32 of the files on the
  disk, 48 of the saving, 27 of the application), 12 end-to-end scripts (and 3 more that are wrong
  on purpose and must fail) and the 15 SDL tests (SDL's virtual joysticks stand in for a pad, and
  SDL's dummy audio driver for a sound card), built with GCC 13 and CMake 3.28, and also with Zig's
  clang. All of them were also run under AddressSanitizer + UndefinedBehaviorSanitizer and under
  ThreadSanitizer, without findings. The engine tests include one that edits the pattern, the
  tempo, the scene and the slices from a second thread while the audio thread renders, and the
  recording tests include one that writes to the capture queue from one thread while another
  reads it.
- The sequencer's timing: the clock is tested alone (steps land on the exact sample, no drift over
  ten minutes, the same steps for any block size, swing, scene changes at the top of the pattern)
  and through the engine (the audio of a pattern is identical, sample for sample, for audio
  buffers of 64 to 4096 frames). End to end, scripts program a pattern, play it and write the audio
  to a WAV file: every note starts within one sample of where the tempo and swing put it (120 to
  150 BPM, swing 50% to 58%, pattern lengths 12 and 16), and after 30 s at 108.7 BPM the last
  note is 0.8 sample off. The engine and the sequencer were not changed in 0.4.0, and the audio
  that those scripts write is the same, sample for sample, as the one 0.3.1 wrote.
- Windows x64: cross-built with Zig (MinGW-w64). Under Wine 9: all the tests that exist on
  Windows (some are Linux only, such as the ones of the direct USB readers: 431 test cases) and the
  12 scripts, and the `kerf.exe` of the zip (with its `SDL2.dll`), which played a programmed
  pattern through SDL's audio path with the timing above, recorded from a pretend input into a WAV
  file (byte for byte the one the Linux build writes), and made a project of a sample and found it
  again when it ran a second time (the project file is byte for byte the Linux one). The
  screenshots, the audio of the sequencer scripts, the WAV files and the project files that the
  scripts make are the same as the Linux build's, byte for byte, except for the folder path that
  the library writes at the top, which Windows writes its own way. Not built with MSVC.
- aarch64: cross-built with Zig. Under qemu-user: all 533 test cases (119 engine, 31
  interface, 53 USB MIDI, 103 USB audio, 78 recording, 42 Record page and 107 of the projects)
  and the 12 end-to-end scripts. The screenshots (the project tags of the header and the KERF
  labels of the library included) are identical to the x86-64 build, and so is the audio of the
  sequencer scripts (16-bit captures, sample for sample), the WAV files that the recording scripts
  save and the project files that the project scripts make (byte for byte). The `kerf.aarch64` of
  the zip, run with an aarch64 SDL2 built for the purpose, played a programmed pattern through
  SDL's audio path with the same timing, recorded from a pretend input into the same WAV file as
  the x86-64 build, and made a project and found it again, with the same project file as the
  x86-64 build, byte for byte. These runs were not on a real device, so the speed on the
  handheld's CPU and the feel of the sticks for tempo and swing are unconfirmed.
- MIDI input: through a pipe on Linux (the raw MIDI path), and, on Windows, WinMM with a real
  keyboard (by its author). The direct USB reading of 0.2.1 is tested in two layers. 27 tests cover
  the descriptor parser (including truncated and random data), the packet decoder (every code
  index, cables, SysEx, clock) and the search over fake `/sys/bus/usb/devices` trees. 26 more (on
  Linux) run the reading code itself against a pretend kernel that behaves as usbfs is documented
  to (checked against the kernel's `devio.c`): claiming the interface, queued transfers, finished
  transfers announced by POLLOUT, stalls, errors, unplugging, a missing device node, a device that
  is tried again or left alone, two keyboards at once. Readers that were broken on purpose (a
  transfer not queued again, POLLIN instead of POLLOUT, no unplug detection, a device opened twice)
  make those tests fail. What has not been done is running it against a real USB device: the
  machine it was written on has no USB bus.
- USB audio input (0.3.0): the same three layers. 31 tests cover the descriptor parser (Audio
  Class 1 and 2, a mixer's recording stream apart from its playback one and its feedback endpoint,
  high-bandwidth endpoints, truncated and wrong descriptors), the choice of format and rate, the
  clock behind a stream, the class requests, the decoding of 8, 16, 24 and 32 bit samples and 32
  bit float (also when the bytes are cut into packets anywhere) and the search over fake
  `/sys/bus/usb/devices` trees. 46 (on Linux) run the isochronous reading itself against a pretend
  kernel: opening, the rate set on the endpoint or on the clock, the settings that are tried when
  the bandwidth is not there, empty, failed and over-long packets, transfers that are queued again,
  stalls, errors, unplugging, the rate the device really delivers, and closing without leaving
  anything open. 26 more look at the whole input the way the Record page does, with a pretend
  `/sys`, a pretend kernel and a test that plays the device (a receiver, a mixer, two receivers of
  one kind, a driver that has the device, one that fails and is tried again). What has not been
  done is running it against a real microphone, receiver or audio interface.
- Recording (0.3.0): 78 tests for the queue between the capture thread and the screen (also with
  one thread writing while another reads), the choice of channels, the meters (peak, hold, fall
  of 24 dB a second, clipping after the gain), the take (limit, gain, mono and the mix of two) and
  the WAV file (read back by Kerf's own decoder, and its header checked byte by byte), the numbering
  of the files, the inputs (the hub, the pretend microphone) and the session (which input is open,
  an input unplugged and plugged in again, a take that is kept or thrown away). 42 tests and 3
  scripts press the buttons of the Record page and check what is recorded, saved and loaded, in
  an empty folder: the file `rec-001.wav` exists, is as long as the take and is the sample that is
  loaded. The system's capture path (SDL) is run against SDL's dummy audio driver. Bugs were put
  into this code on purpose, one at a time, and the tests were extended until every bug that went
  unnoticed was either caught or shown to change nothing.
- The pages (0.3.1): the same was done for the flow between REC, LOAD, SLICER and SEQ. The tests
  press Select as a tap, with another button in the same frame, with its press on another screen
  and with a release that has no press; they follow B, Start, the menus and the help back to the
  page they came from, with and without a sample, and check that a take that is being made or is
  waiting to be saved cannot be left by accident (Select, or a file dropped on the window). 59 bugs
  were planted in that logic, one at a time (a Select that arms nothing, a chord that is not seen,
  a menu that leads back to the wrong page, a page that is not remembered, an input that stays
  open, ...); 8 went unnoticed at first, each got a test, and all 59 are caught now.
- Projects (0.4.0): the same was done for the saving of work. 52 of the engine tests are about
  the file: every field written and read back as the very same value (the floats and their extremes
  too), the layout the format documents, what the strict reader refuses and why (numbers that are
  not numbers, a slice or a pattern that is damaged, a file that stops anywhere, an `end` line that
  does not fit), a file of a newer Kerf, a project that belongs to other audio, numbers brought into
  range, and what is taken from the engine and put back into it. One of them damages two good
  files in about 20,000 ways (every byte changed into each of a few awkward ones, taken out or
  preceded by one, and every line gone, doubled, blanked or swapped with the next) and checks that
  reading always ends well: a project that can be written and read back as the same work and is
  inside the range of the controls, or a damaged file with its reason. 32 tests look at the file
  on the disk (the temporary file, the flush, the rename, a full disk, a folder that is not there
  or may not be written, a file that is a folder, one that is shorter than it said, the backup,
  the file set aside as `.old`), some of them against system calls that were made to fail, and they
  were run again as an ordinary user, because file permissions mean nothing to root. 48 tests are
  about when it saves (the first change, the quiet moment, the ten seconds, nothing while a take
  is in hand, before another sample is loaded and at quit, a save that fails and is tried again,
  the project of another sample, the switch) and 27 press the buttons of the application and read
  the header, the messages and the library's labels off the screen. Scripts run Kerf twice in one
  folder (the first run makes a project, the second starts afresh and finds the slices, the steps
  and the tempo as they were left), with the projects off, and on a disk that refuses the file;
  three more are wrong on purpose and have to fail, to show that their `expect` lines can fail.
  Bugs were planted in this code, one at a time (a timer a second out, a rename before the flush, a
  project read for the wrong sample, a number that is not brought into range, a label that is not
  updated, ...): 480 of them, of which about 70 went unnoticed by the first tests. Each of those
  got a test, and all are caught now but two, which change nothing anyone could see (a value that
  nothing reads afterwards). The search found one real flaw, which was fixed: a message, or the
  KERF mark of the library, could be lost if another sample was loaded at that very moment. What
  has not been done is saving on the SD card of a real handheld (a slow card, the card's file
  system, power taken away at a bad moment).

## Roadmap

Now, next and later: what is being tried at the moment, what comes after it, and what is further
away or not decided. Inside each part the order is roughly the order of the work. Nothing here is a
promise.

### Now

- **Try Kerf on the real device.** The saving and loading of projects (0.5.0) on the SD card, and
  why the autosave of 0.4.0 brought nothing back there; the start pages (REC, LOAD, PROJ); an
  audio interface (a Tascam Model 12, 12 channels) and other microphones and receivers; the
  sequencer page; the stick feel for tempo and swing; audio latency. What turns up is fixed first.

### Next

- **Step velocity on the sequencer**: L2 / R2 lower and raise the velocity of the step under the
  cursor. (Today L2 / R2 are the fine / coarse keys of the tempo and swing sticks on that page, so
  those need another place.)
- **Flam**: Y on a step that is already on turns a flam (a quick grace hit just before the step)
  on and off. (Today Y, held, plays the slice of the row; on a step that is off it would go on
  doing that. Any other key would do: Y is just the least used.) Velocity and flam both need a
  step to hold more than on / off, so the patterns and the project file change (format version 2,
  which still reads version 1).
- **MIDI: start and stop the sequencer from a MIDI CC** (a control-change message, for example from
  a button of a keyboard or a controller). Which CC and which values: to be decided.
- **MIDI clock sync**: the sequencer follows the tempo and the start / stop of an external MIDI
  clock.
- Compare the sequencer with the web app's (scenes, per-step options) and bring them in line; the
  web app's `kerf.html` was not at hand when it was written.
- Bring the tests up to date with 0.5.0 (the old suites fail in a few places and the new code has
  none), and write the tests of the new things above.

### Later

- **Audio render**: write the sound of a sequence to a WAV file (a bounce), and of a set of
  sequences, such as the eight scenes one after the other or a chain of scenes in an order of
  your choice.
- The effects chain (compressor, decimator, overdrive, reverb).
- Maybe: compatibility with the web app's project files (opening them, and perhaps writing them).
  It is not decided that it is needed: Kerf has projects of its own now.
- Performance work on the ARM core, then the hardware of its own.
