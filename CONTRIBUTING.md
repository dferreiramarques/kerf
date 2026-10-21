# Contributing to Kerf

Thanks for your interest in Kerf! This document outlines the development workflow and upcoming features.

## Development Setup

```bash
# Clone the repository
git clone https://github.com/dferreiramarques/kerf.git
cd kerf

# Install dependencies (optional, for local serving)
npm install

# Start development server
npm run dev
# Server will run on http://localhost:8000
```

## Current Status

**Desktop optimized (beta)**

Kerf is currently fully optimized for **desktop and larger screens** with a comprehensive feature set including:
- Full audio slicing and sample manipulation
- Real-time effects chain
- Dual MIDI modes (Trigger & Sampler)
- 16-step sequencer with scene management
- PWA installation support
- Service Worker for offline functionality

## Roadmap - Next Version

### Mobile UI Redesign (Priority: HIGH)

**Screens to Optimize:**
- [ ] **Waveform Display**
  - Vertical layout option
  - Touch-friendly drag handles for in/out
  - Responsive canvas sizing
  - Swipe to zoom/pan

- [ ] **Slice Editor Panel**
  - Stack vertically on mobile
  - Larger touch targets (min 44x44px)
  - Collapse/expand sections
  - Horizontal scroll for parameter sliders

- [ ] **MIDI/Keyboard Virtualization**
  - Responsive keyboard layout
  - Swipeable keyboard octaves
  - Touch-optimized button sizes
  - Two-row piano keyboard for narrow screens

- [ ] **Sequencer Tab**
  - Grid-based step display (adjust column count by screen width)
  - Full-width scene selector
  - Large play/stop buttons
  - Horizontal scrolling for long patterns

- [ ] **Effects Chain**
  - Horizontal carousel of effect tiles
  - Knob interaction without mouse drag (tap to edit value)
  - Number input fallback for precise control
  - Collapse effects to save screen space

### Technical Implementation Notes

- Use CSS Media Queries: `@media (max-width: 768px)`
- Implement touch event handlers (no mouse-only interactions)
- Test on: iPhone 12/14, iPad, Android tablets
- Safe area insets for notched devices
- Landscape vs. portrait orientation handling

### File Structure for Mobile Features

```
kerf/
├── kerf.html            # Contains responsive CSS
├── service-worker.js    # No changes needed
├── manifest.json        # Add orientation: "portrait-primary"
├── README.md           # Update mobile section
└── [Future: separate mobile.css if needed]
```

### Breakpoints

```css
/* Desktop */
@media (min-width: 1200px) { /* Current optimized for this */ }

/* Tablet */
@media (min-width: 768px) and (max-width: 1199px) { /* TODO */ }

/* Mobile */
@media (max-width: 767px) { /* TODO */ }

/* Small Mobile */
@media (max-width: 480px) { /* TODO */ }
```

## Code Style

- **Language**: Vanilla JavaScript (no frameworks)
- **Formatting**: 4 spaces for indentation
- **Naming**: camelCase for variables/functions, kebab-case for CSS classes
- **Comments**: Use emoji prefixes for clarity
  - 🎯 Feature explanation
  - ⚠️ Important notes/warnings
  - 🐛 Known issues
  - 📝 TODO items

## Performance Notes

- Audio processing happens in real-time using Web Audio API
- Service Worker caches app shell for offline support
- No external dependencies (pure Web APIs)
- Large file handling (multiple MB audio files) tested

## Testing

Current testing is manual via browser DevTools:
- Open DevTools: F12
- Console tab for error checking
- Performance tab for audio CPU usage
- Network tab to verify Service Worker

## Accessibility

**Current Accessibility:**
- Keyboard shortcuts available (QWERTY mapping)
- Focus-visible states on buttons
- Semantic HTML structure

**TODO for V1.1:**
- ARIA labels for custom controls
- Keyboard navigation for all functions
- High contrast mode option
- Screen reader support for complex UI

## Git Workflow

```bash
# Create feature branch
git checkout -b feature/mobile-ui

# Make changes, test locally
npm run dev

# Commit with clear messages
git commit -m "feat: responsive waveform for mobile"

# Push and create PR
git push origin feature/mobile-ui
```

## Issue Reporting

When reporting issues, include:
- Browser and version
- Device/screen size
- Steps to reproduce
- Screenshots if UI-related
- Console errors (F12)

## Questions?

Open an issue or contact: hello@monco.io

---

**Remember**: Kerf is built with ethical AI as a core philosophy. Keep decisions human-centered and user-focused.
