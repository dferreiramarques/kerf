# Kerf Lite (VST3)

Free JUCE 8 instrument plugin — a slimmed-down port of just the **Slicer** tab from the Kerf PWA (`../kerf.html`): no sequencer, no multi-tempo, no effects chain. Informally "Kerf Lite" — a separate product from the web app going forward (own repo/folder structure not decided yet; lives here in `plugin/` for now). See `../` for the web app; this is an independent CMake project sharing no code with it — the DSP is a from-scratch C++ port, and the UI is plain native JUCE widgets, not a reuse of the web app's HTML/CSS.

Full architecture/rationale: see the plan this was built from (slice bank sizing, MIDI mapping, phase breakdown, and the 2026-08-21 update explaining why effects were dropped and the UI stayed native instead of becoming a WebView port of kerf.html).

## Prerequisites (verified working recipe on this machine)

1. **An MSVC C++ toolset + Windows SDK.** This machine already had one via **VS Build Tools 2019** (`C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools`) — that's what the build below actually uses. (A `vs_installer.exe modify` attempt to add the workload to the newer VS 2026 Community install silently no-op'd — didn't error, just didn't add anything — so if you're setting this up fresh, prefer the VS Installer GUI over the CLI `modify` command, or just confirm you already have a working `cl.exe` some other way first.)
2. **CMake** (3.22+): `winget install --id Kitware.CMake -e` — installed at `C:\Program Files\CMake\bin`. Note this only lands on `PATH` in a *new* shell.

That's it — no WebView2 NuGet package needed. There was one earlier in this project's life (see Appendix below), but it went away entirely once the UI moved to plain native JUCE widgets.

## Build (VST3, Debug)

From a shell where `cmake` and `cl` are both reachable (a Developer Command Prompt, or full paths):
```
cmake -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Debug --target Kerf_VST3
```
First configure will `git clone` JUCE 8.0.15 (FetchContent) — takes a while, needs network access. `COPY_PLUGIN_AFTER_BUILD` is on, so a successful build also installs to `C:\Program Files\Common Files\VST3\Kerf Lite (Beta).vst3` automatically. If a host has the plugin loaded, that copy step will fail with a file-lock "Permission denied" — close the host first.

Building VST3 without ever having shipped a VST2 build trips a `#error` about parameter-automation compatibility unless `JUCE_VST3_CAN_REPLACE_VST2=0` is defined (already in `CMakeLists.txt`) — expected/correct for a VST3-only plugin, not a bug.

## Building the installer (for distribution)

Build the **Release** config first (Debug works for local testing/DAW checks, but ship Release — no debug asserts, smaller/faster):
```
cmake --build build --config Release --target Kerf_VST3
```
Then compile the Inno Setup script (installed via `winget install --id JRSoftware.InnoSetup -e`):
```
"C:\Users\<you>\AppData\Local\Programs\Inno Setup 6\ISCC.exe" installer\KerfLite.iss
```
Output lands in `../downloads/KerfLite_<version>_Beta_VST3_x64-setup.exe` (same folder the Tauri desktop-app installers use) — just copies the built `.vst3` bundle into `C:\Program Files\Common Files\VST3\`, nothing fancier. Bump `MyAppVersion` in `installer/KerfLite.iss` alongside the plugin's own version when releasing.

## Tests (no DAW needed)

`KerfTests` is a standalone console app (`juce_add_console_app`) running the engine's `juce::UnitTest` suite — SliceBank, Voice, VoiceEngine, TransientDetector — independent of any host. This is the fast local feedback loop for engine changes:
```
cmake --build build --config Debug --target KerfTests
build/KerfTests_artefacts/Debug/KerfTests.exe
```

## Try it

Load the built VST3 (`build/Kerf_artefacts/Debug/VST3/Kerf.vst3`, also auto-copied to the system VST3 folder) into any VST3 host/DAW as an instrument. Import a WAV, click "Auto Detect" (or "Add Slice (0-1s)" for a quick manual test), click a slice region in the waveform (or double-click to create a new one) to select it, then "Play" or play a note from a MIDI track. Try both MIDI modes, and drag the Start/End sliders while a preview is playing to check the live-retrigger crossfade sounds right (not clicky/glitchy).

## Status

**Shipped as a public beta** (2026-08-21): "Kerf Lite (Beta)" — installer at `downloads/KerfLite_0.1.0_Beta_VST3_x64-setup.exe`, linked from the `#vst-lite` section of `../index.html`. Confirmed by ear in Studio One: sample import, transient auto-detect, manual slice add/delete, waveform display with click-to-select / drag-to-resize slice edges / double-click-to-create / scroll-to-zoom / middle-drag-to-pan, per-slice start/end/pitch/volume/pan/fades/mode/enabled editing with live retrigger, Trigger and Sampler MIDI modes, master volume, and full state persistence (sample audio + slice list survive closing and reopening the DAW project). An in-plugin info button (top right) surfaces the sample-rate/bit-depth and project-recall notes below directly to users.

Engine verified independently via `KerfTests`: 86 `juce::UnitTest` cases covering SliceBank snapshot immutability, TransientDetector against the exact ported algorithm (including a "minSliceLength can never actually reject anything" quirk inherited from the original — see the test comments), Voice pitch/fade/loop/shot behavior (including source/output sample-rate mismatch — a 48kHz sample in a 44.1kHz project resamples automatically, no manual conversion needed), VoiceEngine's two note-keying schemes, and StatePersistence's FLAC-embedded round-trip.

Key design notes worth remembering if this needs revisiting:
- Trigger mode does **not** transpose pitch per note — confirmed directly against kerf.html:4101's own comment ("SEM pitch - toca pitch original do slice"). Only Sampler mode transposes, relative to MIDI note 60.
- Retriggering a slice (drag-resize, clicking Play again, or double-clicking to create one under the cursor) crossfades the old voice out rather than cutting it — matches kerf.html's `stopSlice()`+`playSlice()` pattern, which always schedules a fade release, never an instant stop. Waveform-drag retrigger only fires audibly if the slice is already sounding, matching the original's own gate.
- Per-slice fields (start/end/pitch/volume/pan/mode/fades/enabled) are plain state in `SliceBank`, not APVTS parameters — see `ParameterLayout.h`'s header comment for why.
- `selectedSliceForEditing` (which slice the editor's controls operate on) doubles as Sampler mode's target slice, matching kerf.html's own `state.selectedSliceForEditing` doing the same double duty.
- Bit depth is a non-issue (JUCE's `AudioFormatReader` always normalises to float on read); sample-rate mismatch is compensated in `Voice::start()`'s `pitchRatioPerOutputSample` calculation, not bolted on separately.
- The embedded sample in saved state is FLAC-compressed (`StatePersistence.cpp`), not raw PCM — keeps DAW project files from ballooning.

Not started / explicitly out of scope: effects (dropped entirely, this is a pure slicer/sampler now), sequencer/multi-tempo (never in scope), WAV export, macOS/AU build (Windows-only dev machine so far).

## Appendix: the WebView2 detour (no longer relevant, kept for the war story)

Phase 1 originally built the UI as a WebView2-hosted copy of kerf.html's slicer tab. It got working end-to-end (JS↔C++ native bridge round-tripping), but cost several rounds of confusing debugging to get there, and was dropped once effects were cut and a plain native GUI turned out to be simpler for what was left. In case anything like this resurfaces on a future JUCE WebView project:

1. `withResourceProvider`/`withNativeFunction` on Windows only compile when the WebView2 backend is enabled at compile time (`NEEDS_WEBVIEW2 TRUE` + `JUCE_USE_WIN_WEBVIEW2=1`), which itself needs the WebView2 NuGet package present for `WebView2.h`.
2. **The real bug that cost the most time**: `NEEDS_WEBVIEW2 TRUE` links `WebView2LoaderStatic.lib`, but JUCE's runtime code only calls that statically-linked symbol directly when `JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1` is *also* defined. Without it, JUCE instead tries `LoadLibraryA("WebView2Loader.dll")` at runtime — a DLL that's never deployed anywhere — fails silently, and **falls back to the legacy IE-based WebBrowser control with no error reported**. This showed up as an IE "Script Error" dialog (Line/Char/Code fields — a dead giveaway it's IE's Trident engine, not WebView2/Chromium) once the page used ES module syntax IE can't parse.
3. That silent fallback was also almost certainly the cause of an earlier, more confusing symptom: WebView2's virtual resource-provider origin (`https://juce.backend/`) failed to navigate at all ("Navigation to the webpage was canceled") — because it was actually IE trying and failing to resolve a fake host, not a resource-provider bug.
4. A `file://` temp-file workaround was tried in between and abandoned: JUCE's own frontend bundle (`index.js`) does `import "./check_native_interop.js"`, and Chromium treats every `file://` document as a unique/opaque origin, so that relative import gets blocked by CORS even from the same folder. The resource-provider scheme (serving everything from one shared virtual origin) is the only approach that works with JUCE's native integration.
5. `NativeFunctionCompletion` is a member of `juce::WebBrowserComponent` itself, not `juce::WebBrowserComponent::Options` — use `auto` for the lambda's completion parameter (as JUCE's own example does) rather than spelling out the type.
