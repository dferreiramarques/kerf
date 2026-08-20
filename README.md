# Spliiice — Professional Sample Slicer & Sequencer

A browser-based sample slicer and 16-step sequencer with effects chain, built with ethical AI as a core philosophy.

## Features

- 🎙️ **Audio Import & Recording** - Load samples or record directly from microphone
- 🔪 **Smart Transient Detection** - Auto-detect transients with adjustable sensitivity
- 🎛️ **Live Parameter Control** - Adjust in/out, pitch, volume, pan in real-time with instant retrigger
- 🎹 **Dual MIDI Modes**
  - **Trigger Mode**: Each MIDI note triggers a different slice
  - **Sampler Mode**: Play a selected slice at different pitches (piano layout)
- 🎵 **16-Step Sequencer** - Pattern-based playback with scene management
- 📊 **Multi-Effects Chain**
  - Reverb (wet/dry mix control)
  - Compressor (attack, release, ratio)
  - Overdrive (drive, tone)
  - Decimator (bit depth reduction)
- 💾 **Project Save/Load** - Export and import projects as ZIP files
- 📁 **WAV Export** - Export individual slices or full audio
- 🎨 **Vibe-Coded** - Built from scratch with Claude.ai
- 📱 **PWA Ready** - Install as desktop app, works offline

## Installation

### Desktop (PWA)

1. Open **kerf.html** in a modern browser (Chrome, Edge, Firefox, Safari) — or visit [kerf.monco.io/kerf.html](https://kerf.monco.io/kerf.html)
2. Click the **Install** button (usually in the address bar) or use the browser menu
3. Kerf will install as a native desktop app

### Desktop (Windows installer)

Download the native Windows build from [kerf.monco.io](https://kerf.monco.io) — no browser required.

### Local Development

```bash
# Clone or download this repository
cd kerf

# Serve locally (required for service worker)
# Python 3
python -m http.server 8000

# Node.js
npx http-server

# Then open: http://localhost:8000
```

## Quick Start

1. **Load Audio**: Click the "+" button to import an audio file or "REC" to record
2. **Detect Slices**: Use "Auto Detect" to identify transients automatically
3. **Adjust Slices**: Double-click the waveform to create manual slices, or edit existing ones
4. **Play**: Click the PLAY button on any slice, or use QWERTY keyboard (A-L + W-P)
5. **MIDI**: Connect a MIDI controller (Trigger or Sampler mode)
6. **Sequencer**: Create patterns in the Sequencer tab, manage scenes

## Controls

### Keyboard Shortcuts

- **A-L (white keys)**: Trigger slices 0-14 (Trigger mode) / Piano C-D (Sampler mode)
- **W-P (black keys)**: Trigger slices 1-15 (Trigger mode) / Piano C#-D# (Sampler mode)
- **1-8**: Switch between 8 scenes
- **Spacebar**: Stop sequencer

### Mouse

- **Left Click**: Trigger slice / Click keyboard key
- **Double Click Waveform**: Create new slice at that position
- **Drag In/Out Sliders**: Adjust slice boundaries with live retrigger
- **MIDI Controller**: Play slices with variable pitch in Sampler mode

## Modes

### Trigger Mode
- Each MIDI note or key corresponds to a different slice
- Native keyboard shows only configured slices
- Best for playing multi-sample kits

### Sampler Mode
- Single slice played at different pitches based on MIDI note
- Piano layout: A=C4 (middle C), higher notes = higher pitches
- MIDI external only (virtual keyboard disabled)
- Best for pitch-accurate sample playback

## Effects

### Reverb
- **Mix**: Dry (0%) to Wet (100%) blend
- Default: 0% (off)

### Compressor
- **Attack**: Time to react to signal (ms)
- **Release**: Time to recover (ms)
- **Ratio**: Compression amount

### Overdrive
- **Drive**: Amount of saturation
- **Tone**: Filter character

### Decimator
- **Bit Depth**: 8-16 bits (lower = more degradation)
- **Sample Rate**: Reduction multiplier

## File Export

- **WAV Export**: Export individual slices or full audio at original quality
- **Project Save**: Save all slices, patterns, and settings as ZIP
- **Project Load**: Restore entire projects

## Project Structure

```
kerf/
├── index.html           # Product/landing page (kerf.monco.io)
├── kerf.html             # The app itself (PWA entry point)
├── manifest.json        # PWA configuration
├── service-worker.js    # Offline support
├── downloads/            # Windows installer builds (.exe / .msi)
└── README.md           # This file
```

## Browser Support

- ✅ Chrome/Chromium 90+
- ✅ Firefox 88+
- ✅ Safari 14+
- ✅ Edge 90+

Web Audio API, File API, and Service Workers required.

## Design Philosophy

**Ethical use of AI**: Spliiice is vibe-coded from scratch with Claude.ai as ideation and coding partner. The tool prioritizes:
- User control over automation
- Transparent parameter behavior
- Live feedback on all actions
- Offline-first architecture

## Roadmap - Mobile UI

**Next Version TODO:**
- Responsive UI for mobile/tablet
- Touch-optimized controls
- Vertical layout option
- Smaller screen breakpoints
- Mobile-safe waveform display
- Swipe gestures for navigation

Current version is **desktop-optimized only**.

## License

MIT License - See LICENSE file for details

## Credits

**a David Marques design**  
vibe coded from scratch with Claude.ai as ideation and coding pal  
MIT License  

**a Monco.io product**

[Support on Ko-fi](https://ko-fi.com/dferreiramarques)

---

Made with ❤️ by monco.io studio
