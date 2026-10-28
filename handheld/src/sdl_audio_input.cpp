#include "sdl_audio_input.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <mutex>
#include <string>
#include <vector>

namespace kerf::ui
{
    namespace
    {
        constexpr int kWantedRate = 48000;
        constexpr int kWantedChannels = 2;              // the device may give more
        constexpr int kWantedBlock = 1024;              // frames in each callback
        constexpr double kRingSeconds = 2.0;

        struct Listed
        {
            std::string id;
            std::string name;
        };

        class SdlAudioInput : public InputBackend
        {
        public:
            SdlAudioInput() { initialised_ = SDL_InitSubSystem (SDL_INIT_AUDIO) == 0; }

            ~SdlAudioInput() override
            {
                stop();

                if (initialised_)
                    SDL_QuitSubSystem (SDL_INIT_AUDIO);
            }

            std::vector<InputSource> list() override
            {
                std::vector<InputSource> out;
                const auto devices = enumerate();

                if (! told_ && ! devices.empty())
                {
                    told_ = true;
                    std::string names;

                    for (const auto& d : devices)
                        names += (names.empty() ? "" : " | ") + d.name;

                    log ("audio: the system's inputs: " + names);
                }

                for (const auto& d : devices)
                {
                    InputSource s;
                    s.id = d.id;
                    s.name = d.name;
                    s.detail = "system audio";
                    s.direct = false;
                    out.push_back (std::move (s));
                }

                return out;
            }

            void start (const InputSource& source, CaptureRing& ring) override
            {
                stop();

                {
                    std::lock_guard<std::mutex> lock (mutex_);
                    status_ = InputStatus {};
                }

                frames_.store (0);

                if (! initialised_)
                {
                    finish (InputState::failed, "the system's audio is not available");
                    return;
                }

                std::string name;

                for (const auto& d : enumerate())
                    if (d.id == source.id)
                        name = d.name;

                if (name.empty())
                {
                    finish (InputState::failed, "the input is gone");
                    return;
                }

                SDL_AudioSpec want;
                SDL_zero (want);
                want.freq = kWantedRate;
                want.format = AUDIO_F32SYS;
                want.channels = (Uint8) kWantedChannels;
                want.samples = (Uint16) kWantedBlock;
                want.callback = &SdlAudioInput::callback;
                want.userdata = this;

                SDL_AudioSpec have;
                SDL_zero (have);

                // The device's own rate and channels are taken as they are (SDL converts nothing but the format).
                const int allowed = SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE;
                const SDL_AudioDeviceID opened = SDL_OpenAudioDevice (name.c_str(), 1, &want, &have, allowed);

                if (opened == 0)
                {
                    const std::string why = SDL_GetError();
                    log ("audio: " + name + ": cannot open the input: " + why);
                    finish (InputState::failed, why.empty() ? std::string ("the input could not be opened") : why);
                    return;
                }

                if (have.format != AUDIO_F32SYS || have.channels == 0 || have.freq <= 0)
                {
                    SDL_CloseAudioDevice (opened);
                    log ("audio: " + name + ": the input gave a format Kerf cannot use");
                    finish (InputState::failed, "the input gave a format Kerf cannot use");
                    return;
                }

                device_ = opened;
                channels_ = (int) have.channels;
                ring.configure (channels_, std::max ((int) (have.freq * kRingSeconds), 1));
                ring_.store (&ring, std::memory_order_release);

                {
                    std::lock_guard<std::mutex> lock (mutex_);
                    status_.channels = channels_;
                    status_.sampleRate = (int) have.freq;
                    status_.message = "listening";
                }

                log ("audio: " + name + ": recording, " + std::to_string (channels_) + " channels at " + std::to_string (have.freq) + " Hz");
                state_.store (InputState::running, std::memory_order_release);
                SDL_PauseAudioDevice (device_, 0);
            }

            void stop() override
            {
                if (device_ != 0)
                {
                    SDL_CloseAudioDevice (device_);       // returns when the callback is not running any more
                    device_ = 0;
                }

                ring_.store (nullptr, std::memory_order_release);
                state_.store (InputState::idle, std::memory_order_release);
            }

            InputState state() const override { return state_.load (std::memory_order_acquire); }

            InputStatus status() const override
            {
                std::lock_guard<std::mutex> lock (mutex_);
                InputStatus s = status_;
                s.state = state_.load (std::memory_order_acquire);
                s.frames = frames_.load (std::memory_order_relaxed);
                return s;
            }

            // A device that is unplugged stops by itself: SDL then reports it as stopped.
            void poll (double) override
            {
                if (device_ != 0 && state_.load (std::memory_order_acquire) == InputState::running
                    && SDL_GetAudioDeviceStatus (device_) == SDL_AUDIO_STOPPED)
                    finish (InputState::ended, "the input was unplugged");
            }

        private:
            // The capture devices. SDL opens a device by its name (the first one with that name), so a
            // name that comes twice is listed once.
            std::vector<Listed> enumerate() const
            {
                std::vector<Listed> out;

                if (! initialised_)
                    return out;

                const int count = SDL_GetNumAudioDevices (1);
                std::set<std::string> seen;

                for (int i = 0; i < count; ++i)
                {
                    const char* raw = SDL_GetAudioDeviceName (i, 1);

                    if (raw == nullptr || *raw == '\0')
                        continue;

                    const std::string name = raw;

                    if (seen.insert (name).second)
                        out.push_back ({ "sdl:" + name, name });
                }

                return out;
            }

            void finish (InputState state, const std::string& message)
            {
                {
                    std::lock_guard<std::mutex> lock (mutex_);
                    status_.message = message;
                }

                state_.store (state, std::memory_order_release);
            }

            static void SDLCALL callback (void* userdata, Uint8* stream, int length)
            {
                auto* self = static_cast<SdlAudioInput*> (userdata);
                CaptureRing* ring = self->ring_.load (std::memory_order_acquire);

                if (ring == nullptr || self->channels_ <= 0 || length <= 0)
                    return;

                const int frames = length / (int) (sizeof (float) * (size_t) self->channels_);
                ring->write (reinterpret_cast<const float*> (stream), frames);
                self->frames_.fetch_add ((uint64_t) frames, std::memory_order_relaxed);
            }

            bool initialised_ = false;
            bool told_ = false;
            SDL_AudioDeviceID device_ = 0;
            int channels_ = 0;                          // written before the device is unpaused, read by the callback
            std::atomic<CaptureRing*> ring_ { nullptr };
            std::atomic<uint64_t> frames_ { 0 };
            std::atomic<InputState> state_ { InputState::idle };
            mutable std::mutex mutex_;
            InputStatus status_;
        };
    }

    std::unique_ptr<InputBackend> makeSdlAudioInput()
    {
        return std::make_unique<SdlAudioInput>();
    }
}
