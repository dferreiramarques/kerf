// Tests for the Record screen's input from the system's own audio (sdl_audio_input.cpp). SDL's
// "dummy" audio driver stands in for the sound card: it has one capture device that sends silence at
// the pace of the clock, so the real SDL capture path runs without any hardware.
//
// These tests are linked into the SDL test program (test_sdl_input.cpp has its main()).

// The program has its own main(): keep SDL from renaming it (matters on Windows).
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_input.h"
#include "minitest.h"
#include "sdl_audio_input.h"

using namespace kerf::ui;

namespace
{
    // The audio SDL uses is chosen when it starts: say "dummy" before a backend is made.
    void useDummyDriver() { SDL_setenv ("SDL_AUDIODRIVER", "dummy", 1); }

    template <typename Predicate>
    bool waitUntil (Predicate done, int timeoutMs = 5000)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMs);

        while (std::chrono::steady_clock::now() < end)
        {
            if (done())
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }

        return done();
    }

    struct Log
    {
        std::mutex mutex;
        std::vector<std::string> lines;

        InputLogger logger()
        {
            return [this] (const std::string& line)
            {
                std::lock_guard<std::mutex> lock (mutex);
                lines.push_back (line);
            };
        }

        int count (const std::string& part)
        {
            std::lock_guard<std::mutex> lock (mutex);
            int n = 0;

            for (const auto& l : lines)
                if (l.find (part) != std::string::npos)
                    ++n;

            return n;
        }
    };

    // The first input the system offers, or an empty source when it offers none.
    InputSource firstInput (InputBackend& backend)
    {
        const auto listed = backend.list();
        return listed.empty() ? InputSource {} : listed.front();
    }
}

KTEST (SdlAudioInput, the_systems_capture_devices_are_listed_as_system_audio)
{
    useDummyDriver();
    Log log;
    const auto backend = makeSdlAudioInput();
    backend->setLogger (log.logger());

    const auto listed = backend->list();
    EXPECT (! listed.empty());

    if (listed.empty())
        return;

    for (const auto& s : listed)
    {
        EXPECT (s.id.rfind ("sdl:", 0) == 0 && s.id.size() > 4);
        EXPECT (s.id == "sdl:" + s.name);
        EXPECT (! s.name.empty());
        EXPECT (s.detail == "system audio");
        EXPECT (! s.direct);
    }

    // What the system offers is written down once, whoever asks and however often.
    EXPECT (backend->list().size() == listed.size());
    EXPECT (backend->list().size() == listed.size());
    EXPECT (log.count ("audio: the system's inputs: ") == 1);
    EXPECT (log.count (listed.front().name) == 1);
}

KTEST (SdlAudioInput, an_input_fills_the_ring_until_it_is_stopped)
{
    useDummyDriver();
    Log log;
    const auto backend = makeSdlAudioInput();
    backend->setLogger (log.logger());
    const InputSource source = firstInput (*backend);
    EXPECT (! source.id.empty());

    if (source.id.empty())
        return;

    EXPECT (backend->state() == InputState::idle);

    CaptureRing ring;
    backend->start (source, ring);
    EXPECT (backend->state() == InputState::running);

    const InputStatus status = backend->status();
    EXPECT (status.state == InputState::running);
    EXPECT (status.channels >= 1 && status.sampleRate > 0);
    EXPECT (status.message == "listening");
    EXPECT (ring.channels() == status.channels);
    EXPECT (ring.capacity() == status.sampleRate * 2);                       // two seconds

    // The silence of the pretend sound card comes at the pace of the clock.
    EXPECT (waitUntil ([&] { return ring.written() >= (uint64_t) status.sampleRate / 10; }));
    EXPECT (waitUntil ([&] { return backend->status().frames >= (uint64_t) status.sampleRate / 10; }));
    EXPECT (log.count ("recording, " + std::to_string (status.channels) + " channels at " + std::to_string (status.sampleRate) + " Hz") == 1);

    // Asking now and then whether the device is still there (the screen does it every frame) changes nothing.
    for (int i = 0; i < 5; ++i)
        backend->poll (0.016);

    EXPECT (backend->state() == InputState::running);

    std::vector<float> block ((size_t) ring.channels() * 256, 1.0f);
    const int got = ring.read (block.data(), 256);
    EXPECT (got > 0);

    for (int i = 0; i < got * ring.channels(); ++i)
        EXPECT (block[(size_t) i] == 0.0f);

    backend->stop();
    EXPECT (backend->state() == InputState::idle);

    // Stopped means that nothing is written any more.
    const uint64_t written = ring.written();
    std::this_thread::sleep_for (std::chrono::milliseconds (120));
    EXPECT (ring.written() == written);
    backend->stop();                                                         // again: nothing is open
    backend->poll (0.016);
    EXPECT (backend->state() == InputState::idle);
}

KTEST (SdlAudioInput, starting_another_stream_replaces_the_first)
{
    useDummyDriver();
    const auto backend = makeSdlAudioInput();
    const InputSource source = firstInput (*backend);
    EXPECT (! source.id.empty());

    if (source.id.empty())
        return;

    CaptureRing first, second;
    backend->start (source, first);
    EXPECT (backend->state() == InputState::running);
    EXPECT (waitUntil ([&] { return first.written() > 0; }));

    backend->start (source, second);                                         // no stop() in between
    EXPECT (backend->state() == InputState::running);
    EXPECT (backend->status().frames < 4800);                                // counted from the new stream's start
    EXPECT (waitUntil ([&] { return second.written() > 0; }));

    const uint64_t written = first.written();
    std::this_thread::sleep_for (std::chrono::milliseconds (120));
    EXPECT (first.written() == written);
    EXPECT (second.written() > 0);
    backend->stop();
}

KTEST (SdlAudioInput, an_input_that_is_gone_says_so)
{
    useDummyDriver();
    const auto backend = makeSdlAudioInput();
    InputSource gone;
    gone.id = "sdl:No such microphone";
    gone.name = "No such microphone";

    CaptureRing ring;
    backend->start (gone, ring);
    EXPECT (backend->state() == InputState::failed);
    EXPECT (backend->status().state == InputState::failed);
    EXPECT (backend->status().message == "the input is gone");
    EXPECT (backend->status().channels == 0 && backend->status().frames == 0);
    EXPECT (ring.written() == 0);

    backend->poll (0.016);
    EXPECT (backend->state() == InputState::failed);                         // nothing is open, so nothing to find unplugged
    backend->stop();
    EXPECT (backend->state() == InputState::idle);

    // A good stream after a failed one starts clean.
    const InputSource source = firstInput (*backend);

    if (! source.id.empty())
    {
        backend->start (source, ring);
        EXPECT (backend->state() == InputState::running);
        EXPECT (backend->status().message == "listening");
        backend->stop();
    }
}

KTEST (SdlAudioInput, without_an_audio_system_nothing_is_offered_and_nothing_opens)
{
    SDL_setenv ("SDL_AUDIODRIVER", "no-such-audio-driver", 1);
    const auto backend = makeSdlAudioInput();
    EXPECT (backend->list().empty());

    InputSource source;
    source.id = "sdl:Anything";
    source.name = "Anything";
    CaptureRing ring;
    backend->start (source, ring);
    EXPECT (backend->state() == InputState::failed);
    EXPECT (backend->status().message == "the system's audio is not available");
    backend->stop();
    EXPECT (backend->state() == InputState::idle);

    useDummyDriver();
}
