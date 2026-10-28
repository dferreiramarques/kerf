# Plan for handheld 0.6.0

Written on 2026-10-28 for the next working session. **Nothing here is started.** David starts it
("segue o plano em handheld/PLAN.md"); nothing runs by itself. Usage was tight when this was
written, so the work is split into phases that each end in a green build and a commit.

His request, as he wrote it:

> - compatibilidade de projetos entre web e handheld
> - correcção da cadeia de efeitos para a correcta (o que está no codigo)
> - effeitos por slide ou geral (pode ser um comutador para permitir as 2 opções
> - efeitos no handheld, com as mesmas funcionalidades, mesmo que feitos de uma maneira mais facil
> - avaliação do consumo de energia pela app (parece-me um pouco battery hungry, mas não conheço a performance do device)
> - handheld - file manager de samples (rename e delete) file manager de projetos (copy, rename, delete) e criação de pastas (create, rename e delete com todos os contents lá dentro)
> - é ok a nova versão partir projetos da anterior

## Decisions already taken

- 0.6.0 **may break projects of 0.5.0**: format v1 need not be read any more. Opening one should say so
  ("made by an older Kerf (0.5.0), not supported"), not fail in silence.
- The effect order **in the code** is the right one: **Comp -> Drive (overdrive) -> Bits (decimator) -> Reverb**.
  The web UI (Comp, Bits, Drive, Reverb) and the landing page copy are wrong and get corrected; the DSP order stays.
- Effects can be **global** (as in the web app) or **per slice**, with a switch.
- The handheld gets the same functions as the web app (4 pedals, 8 parameters, 4 on/off, bypass); the
  implementation may be simpler (an algorithmic reverb instead of a convolution, for example).

## How to run the session

1. Read this file, then only the files under "Where things are". Do not re-read the code base.
2. Ask David the "Open questions" with AskUserQuestion (4 questions per call at most, so two calls).
3. Work phase by phase. Each phase ends with: build, `ctest`, a local commit, a short report. If usage
   is tight, finish the phase in progress and stop there.
4. The cloud sandbox cannot push (no GitHub credentials; the proxy refuses). See "Notes for pushing".
5. Be truthful in the README ("What has been tested"): there is no device in the sandbox; say what was
   only run on a PC, under Wine or under qemu.

## Phases (sizes are rough, in fresh tokens, plus or minus 50%)

| Phase | Topic | Work | Done when | Size |
|--|--|--|--|--|
| 0 | 2 | Web UI pedal order and landing page copy | Pedals read Comp, Drive, Bits, Reverb in `kerf.html` and `index.html` | 15-25k |
| 1 | 3, 4 | Effects DSP in `core/`, engine integration, unit tests | Chain runs headless; tests green; sanitizers clean | 100-150k |
| 2 | 3, 4 | Effects page (Slicer > Sequencer > Effects), help, scripts, stale tests fixed | Page works with the pad and the keyboard; scripts green | 80-120k |
| 3 | 1 | Project format v2 (effects, scope) and web compatibility | A web project opens on the handheld and back | 60-120k |
| 4 | 6 | File management and an on-screen keyboard | Rename, delete, copy and folders work and are tested on temp dirs | 120-200k |
| 5 | 5 | Power: diagnostics, idle redraw, measurements | Before/after numbers from the device | 60-100k |
| R | all | Release 0.6.0 | Zips, checksums, docs, landing page, roadmap | 30-50k |
| K | 7 | Research on a port to the Behringer KICK (read-only, independent of 0.6.0) | Findings and a go/no-go written into section 7 | 30-60k |

Total about 450-750k tokens (5 or 6 sessions). The effects alone (phases 0-2) are about 250-400k.
Suggested first session: phases 0, 1 and 2.
Phase K (30-60k) comes on top of that total. It is read-only and independent, so it also fits a short session of its own.

## 1. Project compatibility, web and handheld

**Web** (`kerf.html`, `saveProject()` ~5614 and `loadProjectFromBlob()` ~5692): a `.zip` made with JSZip
containing `audio.wav` (stereo) and `project.json`:
`{ version: 2, audioFileName, bpm, swingAmount, slices[{start,end,pitch,volume,pan,enabled,mode,fadeIn,fadeOut}],
scenes[8]{pattern,mute,solo,stepCount,bpm}, currentScene, sequencerPattern (old alias), effects{compressor{ratio,mix,enabled},
overdrive{drive,mix,enabled}, decimator{bits,mix,enabled}, reverb{mode,mix,enabled}, bypass}, multiTempoSlices[{sliceIdx,bpm,volume,isPlaying}] }`.
The loader ignores unknown keys, so extra keys are safe. `generateAsync({type:'blob'})` has no
`compression` option, so JSZip stores the entries uncompressed (STORE); check this on a real file.

**Handheld 0.5.0**: a text `.kerf` (format v1: `kerf_project = 1`, strict parser, newer versions refused;
`core/src/project.cpp`), next to the sample, naming the sample; `.kerf.bak`, `.kerf.tmp` and `.old` files around it.

Options (decide at the start):

- **A (recommended to evaluate).** The handheld reads and writes the web `.zip` (`project.json` + `audio.wav`).
  Reading STORE zips needs no library (about 100 lines); a small JSON reader and writer (about 250 lines);
  zips deflated by other tools would need `tinfl` from miniz (public domain), leave it out unless needed.
  One self-contained file travels both ways, and renaming or deleting a sample can no longer break a project
  (this also removes a difficulty of topic 6). Files get bigger (the audio is inside).
- **B.** The web app reads and writes the handheld `.kerf` text (JavaScript only). The sample has to travel with it.
- **C.** Keep both and write a converter. Least work now, worst for the user.

To settle: per-scene `bpm` and `stepCount` (web) against one tempo and pattern length (handheld; see
`Project` in `core/src/project.h`); `multiTempoSlices` (web only: pass through untouched or ignore);
units of `start`/`end` (seconds or samples); the `enabled` flag; mono or stereo. The `effects` object maps 1 to 1
onto the handheld's effects. Per-slice effects (topic 3) exist only on the handheld: store them under a new
key in each slice (for example `fx`) that the web ignores, unless the web gets them too (open question).
Tests: round trip web -> handheld -> web on a fixture. Ask David for one real web project zip.

## 2. Effect order (Phase 0)

The code order is Comp -> Drive -> Bits -> Reverb: `applyEffects()` in `kerf.html` (~3870) and the slice
path just above it (~3841). The pedal blocks in the HTML are Comp (~2213), Bits (between), Drive (~2261,
commented "agora em 3o lugar") and Reverb (~2285). Move the Drive block before the Bits block, check the
CSS colours and ids, and fix the in-app tour text if it names the order (~6500). Landing page
`index.html`: "Signal path" section ~862-864 ("Compression, bit-crushing, overdrive and reverb - chained in that
order") and the pedal cards under it must read Compression, overdrive, bit-crushing, reverb.
Note that `kerf.html` is the live app: test it in a browser before committing.

## 3 and 4. Effects on the handheld

Web behaviour to match (`kerf.html`, line numbers at commit 78bee44): `state.effects` ~2714; DSP
`applyCompressor` ~3899, `applyOverdrive` ~3926, `applyDecimator` ~3936, `applyReverb` ~3946;
live reverb `initReverbNodes()` ~2561 and `updateReverbMix()` ~2600. All effects are global to the project.

- **Compressor**: not a dynamics compressor. Per slice: RMS of the whole slice; `amount = (ratio-1)/7`;
  `gain = 0.3/rms`; out = x * (1 + mix * amount * (gain - 1)). Ratio 1..8, mix 0..100. Reproducible exactly
  with a cached RMS per slice (recompute when the slice bounds change) and one constant gain per voice.
- **Overdrive**: out = x*(1-mix) + tanh(x * (1 + drive/100*10)) * mix. Drive 0..100.
- **Decimator**: levels = 2^bits; out = x*(1-mix) + round(x*levels)/levels * mix. Bits 2..16 (amplitude only).
- **Reverb**: live bus = ConvolverNode with a 2 s decaying-noise impulse (exp(-3t/T)), wet = mix/100,
  dry = 1 - mix/100; plus a multi-tap on the slice buffer in the render path (short: 3 taps, 25 ms, 0.25^n;
  long: 12 taps, 80 ms, 0.65^n). Mode short/long. On the handheld: an algorithmic stereo reverb
  (Freeverb style: 8 combs and 4 all-passes per side; short and long change size and feedback), buffers
  allocated in `Engine::prepare()`. A direct 2 s convolution is far too heavy for the device. Not
  bit-identical by design.

Design:

- Data: `EffectsSettings` with the same fields and ranges as the web JSON, plus `bypass`.
- Where: compressor, overdrive and decimator **per voice**, before the envelope, in `Voice::renderNextBlock`
  (to match the web, which processes the slice buffer before pitch; applying them to the neighbours before
  interpolation is closest, after interpolation is simpler). Reverb **on the bus** with a per-voice send.
  Both scopes use the same code: in global scope every voice reads one settings object, in per-slice scope
  it reads its slice's. The reverb mode is global (one reverb instance); a per-slice reverb is a send level.
- Scope switch (Global / Per slice): switching to per-slice copies the global settings into every slice;
  switching back keeps the per-slice ones stored. In per-slice scope the page edits the slice selected in the slicer.
- Real time: no allocation in `render()`; parameters through atomics or the existing slice snapshot
  (`SliceBank::Snapshot`); smooth parameter changes (no zipper noise, no clicks on bypass); flush denormals on ARM.
- Page (`Screen::effects`): 4 pedals x 2 controls as in the web; D-pad left/right = pedal, up/down = control,
  L/R or the sticks = value (fine/coarse), A = pedal on/off, Y = bypass, X = scope, B = back; Select goes
  round Slicer > Sequencer > Effects. Match the existing mapping first (README "Controls", `help_text.h`). Update
  the header (third page lit), help pages, Start menu, input test and README.
- Tests: mix 0 and bypass are bit-exact pass-through; drive, bits and compressor formulas against fixed
  vectors computed from the web code; reverb output finite, decaying, no DC; global and per-slice with the same
  settings render identically; project round trip; a UI script for the page; ASan/UBSan/TSan as for 0.4.0.
- Test debt from 0.5.0: some end-to-end scripts (3 of the first 6) and parts of the suites expect 0.4.0
  behaviour. Run `ctest --test-dir build/handheld --output-on-failure` first, then fix them in phase 2, because
  the new page changes Select and the menus.

## 5. Power use

Known from the code (`handheld/src/main.cpp` ~500-570): the main loop runs at a fixed 60 fps all the time.
Every frame does `app.update`, a full 640x480 software redraw (`app.draw`), `SDL_UpdateTexture`,
`SDL_RenderCopy` and `SDL_RenderPresent`, even when nothing changed; the pacing is `SDL_Delay`
(vsync is tried first); `HostStatus` strings are built every frame; the audio callback (512 frames at
44100 Hz, `config.h`) never stops. The backlight and the CPU governor belong to muOS, but the load is Kerf's.

Measure first, then change (cheapest first):

1. A diagnostics line in `kerf-log.txt` every 10 s: frames drawn and skipped, process CPU time as a % of a core
   (`CLOCK_PROCESS_CPUTIME_ID`), audio callback load (render time / buffer time, average and worst), underruns.
2. Redraw only when the state changes or something is animating (playheads, meters, blinking); otherwise
   `SDL_WaitEventTimeout` (50-100 ms) and skip the upload and the present. 30 fps while playing is probably enough.
3. No string formatting per frame. 4. Audio callback early-out when nothing sounds (no voices, no reverb tail).
5. A larger audio buffer option (1024) in Settings as a battery/latency trade.

Needs David on the device: battery % over 15 minutes in three states (slicer idle, sequencer playing,
recording), brightness fixed and Wi-Fi off, compared with a baseline (muOS menu idle, another PortMaster
port), plus the log lines. Done when there is a before/after table in the README.

## 6. File management on the handheld

Today: `FileBrowser` (`file_browser.h/.cpp`: `FileEntry`, `open`, `activate`, `goUp`, `Kind {samples, projects}`)
feeds the LOAD and PROJ pages; projects are written by `ProjectStore` and watched by `ProjectKeeper`
(autosave, `.bak`, `.tmp`, `.old`). The comment in `file_browser.h` still says `name.wav.kerf`, while 0.5.0
writes `demo_beat.kerf`: check which is true.

Design:

- A new `FileOps` (`handheld/src/file_ops.h/.cpp`, `std::filesystem`, no SDL): rename, delete file, copy file,
  create folder, rename folder, delete folder with everything inside. Name validation for FAT/exFAT (no
  `\ / : * ? " < > |`, no trailing dot or space, not empty, 255 bytes of UTF-8, case-insensitive clashes,
  case-only rename), collision policy "name 2" as the projects already do, path guard (inside the roots the
  browser shows; never `..`; never follow a symlink out; never the root), clear error messages.
- UI: an actions menu on the LOAD and PROJ pages (rename, delete, copy for projects, new folder). Confirmation
  dialogs with No as the default ("Delete folder 'x' and 12 items inside?"). There is no trash on muOS,
  so every delete is permanent. Progress or "working" for big folders.
- An **on-screen keyboard** for names (character grid with the D-pad, A types, B backspace, Start confirms,
  L/R for shift, space); the PC keyboard keeps working (`SDL_TEXTINPUT`); UTF-8 aware (`utf8.h`). It is the
  largest single piece of this topic. The script runner needs a text-entry command for tests.
- Samples: rename and delete. With v1 projects (free-standing `.kerf` naming their sample) scan the folder
  and warn; moot if topic 1 option A is chosen.
- Projects: copy, rename, delete, carrying `.bak` along (decide) and ignoring `.tmp`/`.old`. The keeper must
  not write while one is renamed or deleted, and the open project's path must follow a rename.
- Tests on temporary folders (Linux): collisions, non-empty delete, read-only error, long and Unicode names,
  case-only rename, path-guard escapes.

## 7. Research: a port to the Behringer KICK (Music Tribe 0700-AAM)

Added later on 2026-10-28. **Not part of 0.6.0**: a separate, read-only research track (phase K). It can run
before, between or after the 0.6.0 phases, and it writes no Kerf code.

His words:

> ler este git (MusicTribeOpenSource/0700-AAM) e ver como se faria um port. vê isto para perceber como
> podemos fazer o contribute... vou tentar ter o hardware para testar (CONTRIBUTING.md)

Links (without the `fbclid` tracking part):

- https://github.com/MusicTribeOpenSource/0700-AAM
- https://github.com/MusicTribeOpenSource/.github/blob/main/CONTRIBUTING.md

**What the repo page says** (read on 2026-10-28; it is a young repo with 14 commits, so read it again):

- The product is the Behringer KICK (model BE 0700-AAM), a standalone music production center: 7" touch
  screen, 16 RGB pads in 8 banks, a sampler with editing, step sequencer and looper, 300+ effects, 4 knobs,
  a jog wheel, SD slot, USB-C and USB-A, MIDI DIN, CV/Gate, and the Music Tribe JAM DAW.
- Folders: `Kick/` (the application), `AudIO/` (the audio engine library), `ComProtocol/`, `Linux/` (a
  Buildroot image, so the unit runs Linux), `docs/images/`, `.github/`. Two open pull requests and a Discord.
- Licences: AGPL-3.0-or-later for `Kick/`, `AudIO/` and `ComProtocol/`; GPL-2.0 for the Buildroot image;
  the trademarks (names, logos) are excluded.
- **Not on the page**, so the research has to find it: the SoC and CPU architecture, the RAM, the
  languages and UI toolkit, the build instructions, any SDK, plugin or third-party sampler format, any
  simulator or developer hardware, and how the unit is flashed.

**How to contribute** (CONTRIBUTING.md, read on 2026-10-28; re-read it before opening anything):

- Fork, branch, pull request. For a big change, open an issue or ask on Discord first. A new sampler and
  sequencer app is a big change, so ask before writing code. They say they answer in about 48 hours.
- Every commit is signed off with `git commit -s` (DCO; there is no CLA and the contributor keeps the
  copyright). The sign-off is David's own statement, so he makes or amends the commits himself
  (`git rebase --signoff` adds it to a whole series); the sandbox cannot push anyway.
- Anything about hardware compatibility needs a `specs/` document first. Whoever has looked at decompiled
  or proprietary material may only write specs, and someone else writes the code. So: **never look at
  decompiled firmware**, and say so in the PR.
- The PR must say where the work comes from. AI help is allowed, but large AI-written parts must be
  disclosed and understood by the contributor. Kerf's handheld port was written with Claude, so the PR says
  so, and David has to be able to explain the code he sends.
- Not accepted: proprietary firmware, leaked or decompiled source, keys, cracks, binaries, misused logos.
  A change to the flashing procedure needs an own-risk warning. Ports are not mentioned in the guide.

**Licence fit.** Kerf is MIT (`LICENSE`). MIT code can go into an AGPL project (the combination is AGPL, the
original stays MIT). Code copied from `Kick/` or `AudIO/` into Kerf would make that part AGPL, so do not do it:
keep `core/` independent and put any glue code in the KICK repo. This is not legal advice; David decides.

**Three shapes a port could take** (guesses to check against the code, not findings):

1. A page or app inside `Kick/` (slicer, sequencer, effects) with Kerf's `core/` added as a library and the
   sound going out through `AudIO/`. The most integrated, and the only one that uses the unit's own audio
   path and pads. The UI would be new: touch plus 16 pads, 4 knobs and a jog wheel, instead of a 640x480
   d-pad screen. The 16 pads map naturally to the 16 steps and to slice triggers.
2. A standalone SDL2 program on the Buildroot image, the same shape as the muOS port. The cheapest, if the
   image has SDL2 and ALSA and lets third-party programs run, but it would not share the unit's own sampler,
   effects or storage.
3. Only parts of `core/` (the slicer, the 0.6.0 effects) as a module for `AudIO/`.

**Questions the research has to answer**

1. How do apps and samplers plug into `Kick/` and `AudIO/` (an SDK, a plugin format, or editing the app)?
2. CPU architecture, audio API, UI toolkit, C++ standard and build system (CMake? a Buildroot package?).
   Can `core/` (C++20, no allocation while rendering, a snapshot of the slice bank) build in their toolchain?
3. **Is there a simulator or a PC build?** That decides how much can be tested without the unit.
4. Does the KICK already have a sample, kit or project format that Kerf should read or write (like topic 1)?
5. How would the touch screen, pads, knobs and jog wheel map onto Kerf's pages?
6. Where should the code live (their repo, or an own AGPL repo), and what would the maintainers accept?
7. What can be checked without hardware (cross-build, headless tests) and what needs the unit?

**Method (phase K, about 30-60k tokens; nothing is written to the repo except the findings in this section)**

1. Shallow-clone `0700-AAM` in the sandbox (reading GitHub works from there) or use WebFetch. Download no
   binaries or firmware images.
2. Read: the README files, the build files of `Kick/`, `AudIO/` and `ComProtocol/`, the public headers of
   `AudIO/`, the Buildroot configuration under `Linux/` (architecture, SDL2 or ALSA, Qt?), `docs/`, and the two
   open pull requests (they show what the maintainers ask for).
3. Replace the list above with findings and give a go/no-go, with the smallest useful first step (maybe a
   `specs/` note, or Kerf's `core/` built headless inside their toolchain).
4. Posting an issue, a Discord message or a PR is publishing under David's name. The sandbox prepares the
   text; David sends it, or says yes in the chat for that exact text.

**Hardware.** David will try to get a unit to test. Until then it is research and, at most, a cross-build. This
is the same limit as the muOS port: the sandbox builds and runs headless tests, David tries it on the device
and reports back. Do not write UI code for the KICK without a simulator or a unit.

## Where things are (identifiers, not line numbers, are the stable part)

- Engine: `core/src/engine.cpp` `Engine::renderChunk` (voices mixed, then `masterVolume_`, then the peak),
  `core/src/voice.cpp` `Voice::renderNextBlock` (read with pitch interpolation, envelope, pan),
  `core/src/slice.h`, `core/src/slice_bank.h`, `core/src/engine.h` (threading contract: UI writes commands and
  atomics, audio thread applies them; `Engine::prepare()`). New: `core/src/effects.h/.cpp`,
  `core/tests/test_effects.cpp`, entries in `core/CMakeLists.txt`.
- Project: `core/src/project.h/.cpp` (`Project::kFormatVersion`, `serializeProject`, `parseProject`),
  `handheld/src/project_store.*`, `handheld/src/project_keeper.*`.
- UI: `handheld/src/app.h` (`enum class Screen`), `app.cpp` (page switching, Select, Start menu),
  `app_sequencer.cpp` (the template for a page), `app_draw.cpp`, `canvas.h`, `theme.h`, `input.h`,
  `help_text.h`, `script_runner.cpp` and `handheld/tests/scripts/*.txt`.
- Loop and settings: `handheld/src/main.cpp`, `handheld/src/config.h`.
- Build and release: `handheld/CMakeLists.txt`, `handheld/tools/build-release.sh`, `kVersion` in `handheld/src/main.cpp`.

## Open questions for David (ask at the start)

1. Project compatibility: option A, B or C?
2. Per-slice effects: also in the web app, or handheld only?
3. Per-slice reverb: a send level with one global mode (recommended), or a separate reverb per slice?
4. On-screen keyboard: ASCII only or with Portuguese accents?
5. A real web project `.zip` for the tests, and which other port to use as the battery baseline.
6. Release tag (not starting with `v`): reuse `muOS` or a new `handheld-0.6.0`?
7. KICK (phase K): when does he expect to have a unit? Until then it stays research only (recommended).
8. KICK: ask upstream first (an issue or Discord post that David sends) or keep it private until it runs?

## Release checklist (R)

Bump `kVersion` (`handheld/src/main.cpp`, 0.6.0); `CHANGELOG.md`; README (status, "What has been tested",
roadmap); `handheld/portmaster/` (`port.json`, `gameinfo.xml`, `README.md`, `LEIA-ME.txt`) and
`handheld/windows/LEIA-ME.txt`; the landing page Handheld section (it says "There's no effects chain yet"),
screenshots, and the roadmap on roadmaps.monco.io. Build with `handheld/tools/build-release.sh` (Linux or WSL,
python3, Zig via `pip install ziglang`, the SDL2 MinGW package). Attach the two zips, the source zip and
`SHA256SUMS.txt`. A tag that starts with `v` would run the Windows release workflow and fail.

## Notes for pushing (from 2026-10-28)

- The cloud sandbox cannot push. On David's PC the working clone is the connected folder `D:\Documents\kerf`;
  GitHub Desktop lists it (next to an older clone that is behind). Pushing worked through computer use:
  grant "GitHub Desktop" **and** the real process `githubdesktop.exe`, open the repo, click "Push origin".
  Terminals are click-only, so typing `git push` into one is not possible; David can run it himself.
- Git in the device shell: `git -c safe.directory='*' -c core.fileMode=false -c core.autocrlf=true`, author
  from the repo history. It leaves `tmp_obj_*` and `.lock` files in `.git` unless deleting is approved
  (`device_request_delete_permission`); remove them after each commit, or the next git command on Windows fails.
