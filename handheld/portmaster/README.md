## Notes

Kerf is a sample slicer and step sequencer for handhelds. Record a sample from a USB microphone
(or load a loop from a file), cut it into slices, play the slices from the pad, from a USB keyboard
or from a USB MIDI keyboard, and put them in a 16-step pattern on the sequencer page. It opens on
the Record page; Select goes round the pages of the set you are on (record / sample library /
project library, and slicer / sequencer). It is the handheld version of the Kerf web app (https://kerf.monco.io).

**Version 0.5.0.** The slicer (0.1.0), the two pages with the sequencer (0.2.0), the USB MIDI
reading of 0.2.1 and the recording from USB (0.3.0, with a generic wireless lavalier microphone on
the USB-C port) have been confirmed on a real Anbernic RG35XX H with muOS 2606.0 by its author.
Not tried on the device yet: a Tascam Model 12 (or another audio interface), the start page of
0.3.1 (Kerf opens on the Record page) and the projects: in 0.4.0 the autosave did not bring the
slices back on the device (the cause was not found), and 0.5.0, which saves and loads projects
by hand (Start > Save project / Load project), has only been tried on a PC. All of this has only
been tested on a PC and in emulators (the USB parts also against a pretend USB kernel), and the
automatic tests were not updated for 0.5.0. If something does not work, the file `kerf/log.txt`
(what the launcher printed) and `kerf/kerf-log.txt` (what Kerf printed) say what happened.

What works: loading WAV, MP3 and FLAC files; recording a sample from a USB microphone, receiver or
audio interface; automatic slice detection; moving the start and the end of each slice with the
two sticks; playing the slices from the pad and from MIDI; undo; a 16-step sequencer with one row
per slice, eight scenes, tempo, swing and pattern length; and, new in 0.5.0, projects that you
save and load yourself: the slices, the patterns, the tempo, the swing and the length go into a
file next to the sample (Start > Save project) and come back when you open it on the PROJ page.
What is still missing from the web app: the effects, and opening its project files.

**USB MIDI keyboards.** The kernel of muOS 2606.0 has no USB audio / MIDI driver, so a keyboard
that works on a PC is on the USB bus of the device but nothing can read it. Since 0.2.1 Kerf reads
the keyboard itself (class-compliant ones, which is almost every keyboard that needs no driver on a
PC); nothing is installed and muOS is not touched. Plug the keyboard into the USB-C port for
peripherals, before or after starting Kerf. The status line shows "MIDI 1" and Start > Input test
shows the last message received. If it shows "MIDI -", `kerf/kerf-log.txt` has lines starting with
`midi:` that say what Kerf saw and why it did not open the keyboard. `usb_midi = 0` in
`kerf/kerf.cfg` turns the direct reading off.

**Recording from USB.** The same kernel has no USB audio driver either, so Kerf reads USB
microphones, wireless-microphone receivers and audio interfaces (class-compliant, USB Audio Class
1 or 2) itself, the way it reads MIDI keyboards. Plug the device into the USB-C port for
peripherals (with the OTG adapter or hub that works for a keyboard), before or after starting
Kerf. Kerf opens on the Record page (REC) and opens the input at once, so the meters already move:
it shows a level meter for every channel, takes a pair of channels or a single one (a 12-channel
interface such as a Tascam Model 12 has to work as an audio interface, not in the STORAGE mode of
its menu) and saves `rec-001.wav`, `rec-002.wav` ... in `kerf/samples`; A saves a take and loads it
into the slicer, X saves it and stays for another, B throws it away. Select (a tap) goes to the
sample library (LOAD) instead; from the slicer, Start > Record sample... brings you back to REC. If a
device is not recognised, the lines starting with `audio:` in `kerf/kerf-log.txt` list what Kerf
saw on the USB bus (its formats and raw descriptors). `usb_audio = 0` in `kerf/kerf.cfg` turns the
direct reading off.

**Projects.** You save and load them yourself. On the slicer or the sequencer, Start > **Save
project** writes `demo_beat.kerf` next to `demo_beat.wav` (the slices with all their parameters,
the patterns of the eight scenes, the tempo, the swing and the length; not the audio): with no
project in hand it makes a new file (`demo_beat 2.kerf` ... if the name is taken, so a new project
never replaces another one), with one in hand it saves into it; **Save as new project** always
makes a new one. The screen says "Saved demo_beat.kerf (12 slices)" or "Could not save: <reason>".
To load one, Select twice from REC (or Start > **Load project...**) opens the PROJ page: pick a
`.kerf` file with A and Kerf finds its sample next to it, loads it and puts the project back
("Opened demo_beat.kerf: 12 slices"). The header shows SAVED, EDITED, UNSAVED or NO SAVE above the
output meter, and loading something else, "save and load" after a take or Quit in the menu say
"Unsaved changes - repeat to discard" the first time when the work is not saved (the same again
within 8 seconds goes through). A save is written to a temporary file and renamed over the old one,
so a power cut or a full card leaves the previous project; each save keeps the file it replaces as
`demo_beat.kerf.bak`. Autosave (the 0.4.0 way: `rec-001.wav.kerf` kept by itself next to each
sample) is still there, switched off: Start > Settings > Autosave, or `autosave = 1` in
`kerf/kerf.cfg`. Copy the `.kerf` file together with its sample if you move the sample.


Put your own samples in `kerf/samples`, or browse to any folder from inside Kerf (Select on the
Record page opens the sample library).

## Controls

Kerf has five pages in two sets: **REC**, **LOAD** and **PROJ** (to get a sample or a project:
record a sample, pick a file, pick a saved project) and **SLICER** and **SEQ** (to work with it).
It opens on REC. The header shows the set you are on, with the page you are on lit, and Select (a
tap) goes round the pages of the set (REC, LOAD, PROJ, REC ...; SLICER and SEQ); the sequencer keeps
playing on every page. From the slicer, Start > Record sample..., Load sample... or Load
project... takes you to the first set; from there, Start goes back to the sample (so does B on REC;
on LOAD and PROJ, B goes up one folder at a time and, at the top, back to the sample).

Record page (REC, the start page):

| Button | Action |
|--|--|
| Select (a tap) | Go to the sample library (LOAD); not while a take is running or waiting to be saved |
| D-Pad up/down | Pick a row: Input, Channels, Gain |
| D-Pad left/right | Change the row |
| L1 / R1 | Previous / next input |
| A | Start the take; stop it |
| Y | Try the input again (after an error) |
| B | Back to the sample you were working on (before the first one there is nothing to go back to) |
| Start | The same as B; before the first sample it opens a small menu (Settings, Controls / help, Input test, Quit) |
| After a take: A / X / B | Save and load it into the slicer / save and stay for another take / throw it away |

Sample library (LOAD):

| Button | Action |
|--|--|
| D-Pad up/down | Pick a file or a folder |
| A | Open the folder / load the file |
| B | Up one folder |
| L1 / R1 | Page up / page down |
| Select (a tap) | Go to the project library (PROJ) |
| Start | Back to the sample you were working on; before the first one, a small menu |

Project library (PROJ; it starts in the folder of the sample in hand):

| Button | Action |
|--|--|
| D-Pad up/down | Pick a project (`.kerf`) or a folder |
| A | Open the folder / open the project (loads its sample and puts the work back) |
| B | Up one folder |
| L1 / R1 | Page up / page down |
| Select (a tap) | Go to the Record page (REC) |
| Start | Back to the sample you were working on; before the first one, a small menu |

Slicer page:

| Button | Action |
|--|--|
| Left stick | Move the START of the selected slice (up/down: zoom around START) |
| Right stick | Move the END of the selected slice (up/down: zoom around END) |
| L2 / R2 (held) | Fine / coarse stick movement |
| D-Pad | Up/down: pick a row. Left/right: change the value |
| A | Play the selected slice (hold) |
| X | Add a slice at the centre of the view |
| Y | Auto-detect slices |
| L1 / R1 | Previous / next slice |
| B | Play / stop the sequencer |
| Select | Next page: the sequencer |
| Select + X | Delete the slice |
| Select + B | Undo |
| Start | Menu (Save project, Save as new project, Load project, Load sample, Record sample, Auto-detect, Add / Delete / Clear slices, Undo, Zoom, Go to sequencer, Settings, Controls / help, Input test, Quit) |

Sequencer page:

| Button | Action |
|--|--|
| D-Pad | Move the cursor (row = slice, column = step) |
| A | Step on / off (hold A and move the D-Pad to paint a run) |
| X | Erase the step (hold X and move to erase a run) |
| Y (held) | Hear the slice of the row |
| B | Play / stop |
| L1 / R1 | Previous / next scene (it starts with the next loop) |
| Left stick up/down | Tempo (40 to 300 BPM) |
| Right stick up/down | Swing (50% to 75%) |
| L2 / R2 (held) | Fine / coarse tempo and swing |
| Select + Left/Right | Pattern length (1 to 16 steps) |
| Select + X | Clear the row |
| Select + B | Undo |
| Select | Next page: the slicer |
| Start | Menu (Play / stop, Tempo, Swing, Length, Scene, Copy scene to next, Clear row, Clear scene, Undo, Save project, Save as new project, Load project, Go to slicer, Settings, Controls / help, Quit) |

Hold Select + Start for 1 s to quit. In a menu, B or Start closes it.

If A/B or X/Y are swapped on your device, change them under Menu > Settings. Menu > Input test
shows what the program receives from every button and stick.

Settings are saved in `kerf/kerf.cfg`. Menu > Settings > Autosave switches the automatic saving of
projects (off to start with; you save by hand with Start > Save project) on and off.

## Credits and licences

Kerf is MIT licensed (David Marques / monco.io). It uses SDL2 from the system (zlib licence), the
dr_wav, dr_mp3 and dr_flac decoders by David Reid (public domain or MIT-0) and the 8x8 bitmap
font by Daniel Hepper (public domain). The licence texts are in the `licenses` folder.

Source code: https://github.com/dferreiramarques/kerf (folder `handheld`).
