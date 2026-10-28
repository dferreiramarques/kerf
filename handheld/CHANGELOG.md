# Changes

## 0.5.0

- **Projects are saved and loaded by hand now.** Start menu (Tab) on the slicer or the sequencer:
  **Save project**, **Save as new project** and **Load project...**. A project is a small text
  file next to the sample (`demo_beat.wav` gets `demo_beat.kerf`) with the slices and all their
  parameters, the patterns of the eight scenes, the tempo, the swing, the pattern length, the scene
  and the selected slice. It is the same file format as in 0.4.0. Why: in 0.4.0 the work was saved
  by itself, and for the author, on the device, the slices of the demo beat were gone after loading
  it again; the cause was not found (the saving by itself could not be made to fail on a PC). Now
  you save when you want to, and a save that does not work says so on the screen ("Could not save:
  <reason>", also in `kerf-log.txt`).
- **A third start page, PROJ.** Select goes round REC, LOAD and PROJ (REC > LOAD > PROJ > REC);
  Kerf still starts on REC. PROJ is the library of projects: folders and `.kerf` files, in the folder
  of the sample in hand (where projects are saved). A opens a project: Kerf finds the sample it
  names (beside the project, then in the samples folder), loads it and puts the project in ("Opened
  demo_beat.kerf: 12 slices"); "Sample not found" and "Project does not fit" say when it cannot.
  The header of those pages shows REC, LOAD and PROJ with the one you are on lit. Select on the
  work pages still flips SLICER and SEQ.
- **Save project** writes the project in hand (opened or saved before); with none yet it makes
  `demo_beat.kerf` next to the sample, or `demo_beat 2.kerf` ... when that name is taken, so a new
  project never replaces another one. Each save is written to a `.tmp` file, flushed and renamed
  over the old one, and keeps the old file as `demo_beat.kerf.bak`.
- **The header** (above the output meter) says SAVED, EDITED (a project in hand has changed),
  UNSAVED (a sample with no project has been worked on) or NO SAVE (the last save failed).
- **Unsaved work is not dropped by accident**: loading another sample or project, "save and
  load" after a take, dropping a file on the window and Quit in the menu say "Unsaved changes -
  repeat to discard" the first time and do nothing; the same again within 8 seconds goes through.
- **Autosave is optional now, and off to start with** (Settings > Autosave; `autosave` in
  `kerf.cfg`, which replaces 0.4.0's `projects` key: that old line is ignored, so autosave is off
  after the update even for those who had it on). With it on it works as in 0.4.0 (a
  `name.wav.kerf` next to each sample). Those files are projects too and appear on the PROJ page.
- The help pages and the menus were updated (the REC / LOAD pages of the help are now REC / LOAD /
  PROJ); the empty library says "Press Select twice to record a sample instead.".
- `kerf_headless`: the script `expect screen` knows `projects`.
- **Tests were not updated.** The test suites are those of 0.4.0. The ones that expect the 0.4.0
  behaviour (autosave on to start with, the `projects` key, Select from LOAD going to REC, menu
  entries by position, the pages of the help) fail or no longer apply, and the new code has no
  tests yet. Checked by hand: one scripted session on Linux x86-64 (save a project, change the
  work, the unsaved-changes warning, open the project from the PROJ page, Select round REC, LOAD
  and PROJ) with the screenshots looked at, and everything compiles. The same session also ran
  on the shipped Windows build (under Wine) and the aarch64 build (under qemu) and wrote the same
  project file as the Linux build; nothing else of 0.5.0 was run on them, and nothing was tried on
  the RG35XX H.
- **The roadmap is shown as Now / Next / Later** (README), with four new items in Next: step
  velocity on the sequencer (L2 / R2), a flam toggled with Y on an active step, starting and
  stopping the sequencer from a MIDI CC, and MIDI clock sync. The audio render, the effects and, as
  a "maybe", the web app's project files are in Later.

## 0.4.0

- **New: projects. Kerf saves your work by itself and brings it back when you load the same sample
  again.** Next to a sample (`rec-001.wav`) Kerf keeps a small text file, its project
  (`rec-001.wav.kerf`: the whole file name plus `.kerf`, so that `kick.wav` and `kick.mp3` in one
  folder do not share one). It holds the slices with all their parameters (start, end, pitch,
  volume, pan, one-shot or loop, fades, on or off), the patterns of the eight scenes, the tempo,
  the swing, the pattern length, the scene being edited and the selected slice. Until now all of
  that was gone as soon as Kerf was closed or another sample loaded. When the sample is loaded
  again the project is put back ("Project restored: 12 slices"). The undo history, the zoom and the
  settings are not part of it.
- **When it saves**: the first time you change something (looking at a sample, or moving the
  cursor, makes no file), then 1.5 seconds after the last change (so that a stick that is still
  moving is not written a hundred times), and at the latest every 10 seconds of steady work. Also
  before another sample is loaded and when the program ends, and never while a take is being
  recorded or waits to be saved.
- **The header shows how things stand**, above the output meter: SAVED, EDITED, SAVE OFF (projects
  are switched off) or NO SAVE (a disk that cannot be written, or a project from a newer Kerf). The
  library (LOAD) marks the samples that have a project with a small KERF label next to their size
  (and updates it as soon as a project is made).
- **Careful with your files.** A save is written to `name.kerf.tmp`, flushed to the disk and then
  renamed over the old file, so that a power cut or a full disk leaves the previous project as it
  was; a save that fails is reported once ("Cannot save the project") and tried again every 5
  seconds. The first save of a session keeps the project that was there as `name.kerf.bak`. A
  project that cannot be used - damaged, or made for audio of another length or sample rate - is
  not overwritten: it is renamed `name.kerf.old` (`.old2`, ...) when your first change is saved,
  and until then it is not touched. A project written by a newer Kerf is never read, changed or
  replaced. Numbers in a file that are out of range are brought into range, so a file that was
  edited by hand cannot make the engine play something it cannot.
- **Settings > Projects** switches it off and on (`projects` in `kerf.cfg`, on to start with).
  Off, nothing more is saved from that moment and the project files are not read. Switched on
  again, the sample in hand is saved again at once if it was opened with projects on; a sample
  that was opened while they were off waits until it is opened again.
- Loading another sample still stops the sequencer and clears the patterns, and keeps the tempo,
  swing and length - unless that sample has a project, which brings back its own.
- **Not in this version**: opening the web app's project (.zip) files, and the effects.
- **Roadmap**: added the audio render (a WAV file of a sequence, or of a set of sequences such as
  the eight scenes one after the other) and, as a "maybe", compatibility with the web app's project
  files; the project files of the web app are not needed now that Kerf has its own.
- `kerf_headless` has `--projects` (the scripts do not save projects unless asked to) and the
  script commands `reload`, `expect project none|saved|edited|off|problem`, `expect file <name>
  yes|no` and `print project`. Four new scripts: a pair that runs Kerf twice in one folder (the
  first makes a project, the second starts afresh and finds everything back), one with projects
  off and one on a disk that refuses the file. Three more are wrong on purpose and must fail, to
  show that the `expect` lines can fail.
- Tests: 119 engine tests (67 before; the 52 new ones are about the project file, and one of them
  damages two good files in about 20,000 places), 31 interface tests (the library's marks and the
  setting), 32 tests of the files on the disk (they also run as an ordinary user), 48 of the part
  that decides when to save and 27 of the application, and 12 end-to-end scripts (3 more fail on
  purpose). Bugs were planted one at a time in all of this code (480 of them); about 70 went
  unnoticed by the first tests, each got a test, and all are caught now but two that change
  nothing anyone could see. One real flaw was found that way, and fixed: a message, or the KERF
  mark of the library, could be lost if another sample was loaded at that very moment. A test of
  the Record page that could fail on a busy machine (it expected a small file still to be loading
  a frame after it was dropped on the window) no longer depends on speed. Everything passes
  natively, under AddressSanitizer + UndefinedBehaviorSanitizer and under ThreadSanitizer, with
  Zig's clang, under Wine (the Windows build) and under qemu (aarch64, with screenshots, audio,
  recorded files and project files identical to the x86-64 build).
- The version of this release has not been tried on the RG35XX H yet (saving to the SD card in
  particular), nor have the Tascam Model 12 and the start page of 0.3.1.

## 0.3.1

- **Kerf now starts on the Record page, and Select flips it with the library.** The first screen
  used to be the file browser, with a "Y record a sample" button next to it. Now the first page is
  Record (REC), the page that gets a sample from a microphone or a line input; the library (LOAD),
  where a sample is picked from a folder, is the page behind it. Select (a tap) flips between the
  two, the way it flips between SLICER and SEQ, and the header shows the pair of pages with the one
  you are on lit (KERF, then REC above LOAD). Y (key 2) no longer opens the Record page from the
  library, and an empty folder says "Press Select to record a sample instead." In the slicer's
  menu, "Load sample..." and "Record sample..." are where they were and open the pair at the page
  they name. A sample given on the command line still opens straight in the slicer.
- **Select does nothing while a take is being made or is in hand**, so that a take is never lost by
  accident; the page says "Stop the take first" or "Save or discard the take first". Select does
  not flip either when another button was pressed together with it (Select + Start is the way out
  of the program).
- **B and Start on the Record page lead back to the sample being worked on.** Before there is a
  sample there is nothing to go back to: B does nothing, and Start opens a small menu with
  Settings (the longest take of the Record page is there), Controls / help, Input test and Quit.
  The library's Start does the same. Until now the settings and the input test could not be
  reached before a sample had been loaded. Menus, the settings and the help lead back to the page
  they were opened from.
- The input of the Record page opens as soon as Kerf starts, so the meters move at once, and closes
  when the page is left (as before).
- The help has two new pages, for the gamepad and for the keyboard, about REC and LOAD (six pages
  now). The library's hint bar names Select and Start instead of Y, and says where Start leads.
- **Confirmed on a real RG35XX H (muOS 2606.0): the recording from USB of 0.3.0**, with a generic
  wireless lavalier microphone on the USB-C port. A Tascam Model 12 (or any other audio interface)
  has not been tried yet; the new start page has only been tested on a PC, in emulators and
  against a pretend USB kernel.
- **A file dropped on the window (Windows) while a take is being made or is in hand is left alone
  too.** Loading it would have left the Record page and thrown the take away; the page says
  "Stop the take first" or "Save or discard the take first" instead. This was already so in 0.3.0.
- Tests: the Record page now has 42 test cases (27 in 0.3.0) and the four scripts that pressed Y
  press Select. Bugs were planted one at a time in the logic of the pages (59 of them); 8 went
  unnoticed at first, each got a test, and all are caught now. Everything passes natively, under
  AddressSanitizer + UndefinedBehaviorSanitizer and under ThreadSanitizer, under Wine (the Windows
  build) and under qemu (aarch64, with screenshots and recorded files identical to the x86-64 build).

## 0.3.0

- **New: the Record screen. Record a sample from a microphone or a line input and use it at once.**
  Press **Y** (key 2) in the file browser, or choose **Start > Record sample...**. Pick the input
  (L1 / R1), the channels to take from it and the gain (-12 to +30 dB), watch the level meters,
  press **A** to record and **A** again (or B) to stop. Then **A** saves the take and loads it into
  the slicer, **X** saves it and stays for another take, and **B** throws it away. Takes are saved as
  `rec-001.wav`, `rec-002.wav` ... in the samples folder (16-bit WAV at the sample rate of the
  input; a file is never overwritten, and it is written under a temporary name and renamed when it
  is complete). The take stops by itself at the limit (Settings > Record limit: 30 s to 5 minutes;
  3 minutes to start with); a take shorter than a quarter of a second is thrown away; if the input
  is unplugged during a take, what was recorded is kept.
- **Every channel has its own meter**, with a peak hold and a clip warning, so that a mixer or audio
  interface can be used: a 12-channel input offers its six pairs and each channel on its own, a
  stereo input offers the pair, each side and the mix of both. The input, the channels and the gain
  used last are remembered.
- **Kerf reads USB microphones, wireless-microphone receivers and audio interfaces itself, for
  handhelds whose kernel has no USB audio driver** (the RG35XX H with muOS 2606.0 has none), the way
  0.2.1 does for MIDI keyboards. It speaks USB Audio Class 1 and 2 through usbfs: it finds the
  AudioStreaming interface with a recording endpoint, chooses the format (48000 Hz, else 44100 Hz;
  the most channels, then 24 bit, then 16 bit), sets the sample rate (on the endpoint for Audio
  Class 1, on the clock source for Audio Class 2), keeps isochronous transfers queued on the input
  endpoint and turns the 8, 16, 24 and 32 bit PCM or 32 bit float samples into frames. It does this
  only for devices the kernel has no driver for; where the driver exists (a PC, Knulli, ...) the
  system's own recording devices are used through SDL, and they are listed after the ones Kerf reads
  itself. A device that is plugged in or out while the screen is up is noticed within a couple of
  seconds. Nothing is installed on the device and the firmware is not touched.
- `kerf-log.txt` tells what Kerf saw: lines starting with `audio:` give, for every USB device that
  has an audio stream, its name, a summary of the formats it offers (or that it has no recording
  stream Kerf can use) and its raw descriptors in hex, then each step taken with a device it reads
  (the interface, the sample rate, the setting chosen, the first transfers, how it ended, and the
  reason when something went wrong). If a microphone or receiver is not recognised, that file is
  what the fix is made from.
- New settings in `kerf.cfg`: `usb_audio = 1` (0 turns the direct USB reading off), `record_limit`,
  `record_gain`, `record_input` and `record_channels` (the last three are written by the Record
  screen). The Settings screen has a new line, **Record limit**.
- New command line options of `kerf`: `--no-input` (no audio input at all) and `--fake-input`
  (pretend inputs: a USB microphone, a receiver that sends nothing, a 12-channel mixer and one that
  cannot be opened - to try the Record screen on a machine without a microphone). `kerf_headless`
  has `--fake-input` and `--start-dir DIR` too. Test scripts can say `expect phase / take / input /
  inputs / channels / gain / status / saved / duration / sample`, `print record` and `wait load`.
- The slicer menu has a new entry, "Record sample...", right after "Load sample...", so the entries
  after it moved down by one. The file browser shows "Y  record a sample", and an empty folder says
  "Press Y to record a sample instead."
- Fixed: a time just below a whole minute was shown as "60.0s" (or "1:60.0"), in the information line
  of a sample and now in the Record screen too. It is rounded to a tenth first and reads "1:00.0".
- Tests for 0.3.0 (all on a Linux x86-64 PC, then under sanitizers, Wine and qemu; see the README,
  "What has been tested"): 31 tests for the audio descriptors, the choice of format and rate and the
  decoding of samples; 46 for the reading of isochronous transfers against a pretend usbfs (stalls,
  errors, unplugging, bandwidth, the clock of Audio Class 2); 26 for the USB input as the Record
  screen sees it; 78 for recording itself (the queue between the threads, the choice of channels,
  meters and clipping, the WAV file down to the bytes of its header, the inputs, the session); 27 for
  the Record screen with buttons and a pretend microphone (28 on Windows, which has a list of drives
  to be on); the SDL tests now also run the system's
  capture path against SDL's dummy audio driver; and 3 end-to-end scripts that record, save and load.
  Bugs were put into this code on purpose, one at a time (several hundred), and tests were added
  until every bug that had gone unnoticed was either caught or shown to change nothing.
  **Not tested: a real USB microphone, receiver or Tascam Model 12 on a real handheld.** If one is
  not recognised, `kerf-log.txt` has what is needed (see above).

## 0.2.1

- **Kerf reads USB MIDI keyboards itself, for the RG35XX H with muOS 2606.0.** The kernel of that firmware
  (Linux 4.9.170) is built without the USB audio / MIDI driver (`# CONFIG_SND_USB_AUDIO is not
  set`), so a class-compliant USB MIDI keyboard showed up on the USB bus but nothing read it: there
  was no `/dev/snd/midi*` and the status line said "MIDI -". Kerf now reads such a keyboard itself,
  through the kernel's generic USB interface (usbfs, the way libusb does it): it claims the MIDI
  Streaming interface, keeps two transfers queued on the input endpoint and turns the 4-byte
  USB-MIDI event packets into MIDI messages. It does this only for devices the kernel has no
  driver for; where the driver exists (a PC, Knulli, ...) the raw MIDI node is used as before.
  Nothing is installed on the device and the firmware is not touched.
- The keyboard can be plugged in and out at any time (the USB bus is looked at once a second). A
  new device is looked at twice before it is opened, so that a kernel driver, if there is one, can
  bind first. A keyboard that Kerf cannot open, or that stops answering, is opened again, up to
  four times in all, and is then left alone until it is plugged in again.
- The status line counts these keyboards in "MIDI n". **Input test** has a MIDI block: how many
  inputs, how many of them Kerf reads itself, and the last message received, for example
  `90 3C 64  note on C4 vel 100  ch 1`.
- `kerf-log.txt` tells what happened: one line for every USB device on the bus (vendor:product and
  the class.subclass of its interfaces, or "unknown" when its descriptors cannot be read), then
  "reading it directly", "ready" or the reason it could not be opened, and the bytes of the first
  transfers.
- New setting `usb_midi = 1` in `kerf.cfg`; `0` turns the direct reading off.
- 53 new tests. `test_usb_midi.cpp` (27): the USB descriptors, the 4-byte packets and the search in
  a fake `/sys/bus/usb/devices`. `test_usb_midi_io.cpp` (26, Linux): the reading itself, run
  against a pretend kernel that behaves as the kernel's usbfs does (claiming the interface, queued
  transfers, a finished one announced by POLLOUT, stalls, errors, unplugging, a missing device
  node, a device that is tried again or left alone, two keyboards at once). It has not been run
  with a real USB device yet: confirming it on the handheld is the first thing to do with this
  version.

## 0.2.0

- **New: the Sequencer page.** A 16-step pattern sequencer for the slices: one row per slice (row N
  plays slice N), steps are sixteenth notes, eight scenes, tempo 40 to 300 BPM (left stick up/down),
  swing 50% to 75% (right stick up/down) and a pattern length of 1 to 16 steps. One-shot slices
  start again on every lit step; loop slices are held for a run of lit steps. A scene change waits
  for the end of the pattern.
- **Select (a tap) flips between the two pages**, Slicer and Sequencer. It used to zoom to the slice
  or to the whole sample; the zoom is now in the Start menu (Zoom: SLICE / ALL).
- **B plays and stops the sequencer** on both pages (on the slicer page B was only used to close
  menus). The sequencer keeps playing when you flip pages.
- The patterns follow their slices: adding, deleting, auto-detecting and clearing slices move the
  rows with them, and Select + B (undo) on the slicer page brings the rows back.
- The page header shows which page you are on (SLICER / SEQ, with a play triangle on SEQ while the
  sequencer runs). The help has four pages: gamepad and keyboard, for each of the two pages.
- The Start menu depends on the page: the slicer menu gained "Zoom" and "Go to sequencer"; the
  sequencer menu has Play / stop, Tempo, Swing, Length, Scene, Copy scene to next, Clear row,
  Clear scene, Undo, Go to slicer.
- Engine (`core/src/step_sequencer.h`, `engine.cpp`): steps are placed on their exact sample on one
  absolute clock, so the audio does not depend on the size of the audio buffer and the tempo does
  not drift. 21 new tests in `core/tests/test_sequencer.cpp`; new end-to-end scripts
  `pages_and_sequencer` and `sequencer_follows_slices`; a test that every line of the help fits the
  screen.

## 0.1.0

First version: load WAV, MP3 and FLAC; detect slices; move the start and the end of each slice with
the two sticks; play the slices from the pad, the keyboard or MIDI; undo; settings; input test;
Windows and PortMaster packages.
