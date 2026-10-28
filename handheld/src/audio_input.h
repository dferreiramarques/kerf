#pragma once

// Sound coming in, for the Record screen.
//
// Where the sound comes from is hidden behind InputBackend:
//   - the system's own audio (SDL capture devices): what Windows, macOS and Linux desktops use;
//   - Kerf's own USB audio driver (usb_audio_input.h, Linux only): for the handheld, whose kernel
//     has no USB audio driver;
//   - a pretend microphone (fake_input.h) for the tests and for `kerf_headless --fake-input`.
//
// A backend fills a CaptureRing from its own thread (or from poll(), for the pretend one); the
// InputHub owns the ring and the user interface reads from it. Only the UI thread calls the hub.
//
// The rule that makes the ring safe without locks: a backend sets the ring up (channels, size)
// before it publishes the state `running`, and the reader looks at the ring only in the states
// `running` and `ended` (after the last write). `stop()` returns when the backend writes no more.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "capture_ring.h"

namespace kerf::ui
{
    struct InputSource
    {
        std::string id;             // names the input for as long as it stays plugged in; the same next time
        std::string name;           // what the screen shows
        std::string detail;         // "USB, read by Kerf", "system", ...
        bool direct = false;        // Kerf's own USB driver reads it (the system has no driver for it)
        int backend = -1;           // which backend lists it: set by the hub
    };

    enum class InputState { idle, starting, running, failed, ended };

    struct InputStatus
    {
        InputState state = InputState::idle;
        std::string message;        // why it failed, or what ended it; for the screen
        int channels = 0;           // of the stream, once it runs
        int sampleRate = 0;
        uint64_t dropped = 0;       // frames lost between the device and the screen
        uint64_t frames = 0;        // frames the device has sent since the stream started
        double measuredRate = 0.0;  // what the device really delivers (0 = cannot tell yet)
    };

    using InputLogger = std::function<void (const std::string&)>;

    class InputBackend
    {
    public:
        virtual ~InputBackend() = default;

        // What can be opened now. Cheap enough to call every second or two.
        virtual std::vector<InputSource> list() = 0;

        // Starts the stream and returns at once; state() tells how it went. A stream that is
        // already open has been stopped before this is called.
        virtual void start (const InputSource& source, CaptureRing& ring) = 0;

        // Returns when the backend writes to the ring no more. Does nothing when nothing is open.
        virtual void stop() = 0;

        virtual InputState state() const = 0;                    // cheap: read every frame
        virtual InputStatus status() const = 0;

        // Called from the UI thread once a frame (a pretend input makes its sound here).
        virtual void poll (double /*seconds*/) {}

        void setLogger (InputLogger logger) { logger_ = std::move (logger); }

    protected:
        void log (const std::string& line) const
        {
            if (logger_)
                logger_ (line);
        }

    private:
        InputLogger logger_;
    };

    class InputHub
    {
    public:
        InputHub() = default;
        ~InputHub() { close(); }
        InputHub (const InputHub&) = delete;
        InputHub& operator= (const InputHub&) = delete;

        void setLogger (InputLogger logger);
        void add (std::unique_ptr<InputBackend> backend);
        bool empty() const noexcept { return backends_.empty(); }

        // Everything that can be opened now: the inputs that Kerf reads itself first, then the system's.
        std::vector<InputSource> sources();

        // Closes what is open and opens `source`. False if no backend lists it.
        bool open (const InputSource& source);
        void close();

        bool isOpen() const noexcept { return active_ != nullptr; }
        const InputSource& current() const noexcept { return current_; }
        InputStatus status() const;
        InputState state() const;

        // Moves up to `maxFrames` frames that have arrived to `out` (channels() wide). 0 when
        // nothing is running. Frames can still be read after the stream has ended.
        int read (float* out, int maxFrames);

        void poll (double seconds);

    private:
        std::vector<std::unique_ptr<InputBackend>> backends_;
        InputBackend* active_ = nullptr;
        InputSource current_;
        CaptureRing ring_;
        InputLogger logger_;
    };
}
