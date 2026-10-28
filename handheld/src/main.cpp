// Kerf for handhelds: the SDL2 host. It opens the window, the sound device, the game pads and the
// MIDI inputs, and runs the application (app.h) at 60 frames per second.
//
//   kerf [options] [sample.wav]
//
// The application itself never touches SDL: it draws into a Canvas and reads a Pad, which is
// what keeps it testable without a screen (see main_headless.cpp).

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
 #include <xmmintrin.h>
#endif

#if defined(_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <shellapi.h>
#endif

#include "app.h"
#include "audio_input.h"
#include "canvas.h"
#include "config.h"
#include "engine.h"
#include "fake_input.h"
#include "format.h"
#include "input.h"
#include "midi_input.h"
#include "path_util.h"
#include "script_runner.h"
#include "sdl_audio_input.h"
#include "sdl_input.h"
#include "usb_audio_input.h"

namespace fs = std::filesystem;
using namespace kerf;
using namespace kerf::ui;

namespace
{
    constexpr const char* kVersion = "0.5.0";

    // ------------------------------------------------------------------------------------ log

    std::FILE* gLog = nullptr;

    void logLine (const char* fmt, ...)
    {
        char line[512];
        va_list args;
        va_start (args, fmt);
        std::vsnprintf (line, sizeof line, fmt, args);
        va_end (args);

        std::fprintf (stderr, "%s\n", line);

        if (gLog != nullptr)
        {
            std::fprintf (gLog, "%s\n", line);
            std::fflush (gLog);
        }
    }

    // A line of any length (logLine cuts at 512 bytes): the USB descriptors of an audio device are
    // longer than that, and are what tells why a microphone was or was not recognised.
    void logText (const std::string& line)
    {
        std::fprintf (stderr, "%s\n", line.c_str());

        if (gLog != nullptr)
        {
            std::fprintf (gLog, "%s\n", line.c_str());
            std::fflush (gLog);
        }
    }

    // A problem the person has to know about even when there is no console (a double-clicked .exe).
    void fatal (const std::string& message)
    {
        logLine ("ERROR: %s", message.c_str());
        SDL_ShowSimpleMessageBox (SDL_MESSAGEBOX_ERROR, "Kerf", message.c_str(), nullptr);
    }

    // ------------------------------------------------------------------------------ arguments

    struct Options
    {
        std::string file;               // sample to open at start (UTF-8)
        std::string configPath;
        std::string scriptPath;
        int fullscreen = -1;            // -1 = as the config says
        int scale = 0;                  // 0 = as the config says
        long maxFrames = 0;             // 0 = run until quit
        bool help = false;
        bool version = false;
        bool noAudio = false;
        bool noMidi = false;
        bool noInput = false;           // no microphones or line inputs for the Record screen
        bool fakeInput = false;         // pretend ones instead (to try the Record screen without any)
    };

    void printUsage()
    {
        std::printf ("Kerf %s - sample slicer for handhelds\n\n"
                     "usage: kerf [options] [sample.wav]\n\n"
                     "  --fullscreen, --windowed   override the setting in kerf.cfg\n"
                     "  --scale N                  window size = 640x480 times N (1-4)\n"
                     "  --config FILE              settings file (default: kerf.cfg next to the program)\n"
                     "  --script FILE              run a test script instead of waiting for input\n"
                     "  --frames-max N             quit after N frames\n"
                     "  --no-audio, --no-midi      do not open the sound device / MIDI inputs\n"
                     "  --no-input                 no microphones or line inputs for the Record screen\n"
                     "  --fake-input               pretend audio inputs (a microphone, a mixer, ...)\n"
                     "  --version, --help\n\n"
                     "Environment: SDL_GAMECONTROLLERCONFIG, SDL_AUDIODRIVER, SDL_VIDEODRIVER and\n"
                     "KERF_MIDI_DEVICE (extra raw MIDI sources, ':'-separated) are understood.\n",
                     kVersion);
    }

    // Command-line arguments as UTF-8. On Windows, main() receives them in the ANSI code page,
    // which would mangle a file name with accents, so the wide command line is used instead.
    std::vector<std::string> utf8Args (int argc, char** argv)
    {
        std::vector<std::string> out;

#if defined(_WIN32)
        (void) argc; (void) argv;
        int count = 0;

        if (LPWSTR* wide = CommandLineToArgvW (GetCommandLineW(), &count))
        {
            for (int i = 0; i < count; ++i)
            {
                const int bytes = WideCharToMultiByte (CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
                std::string s ((size_t) std::max (bytes, 1), '\0');
                WideCharToMultiByte (CP_UTF8, 0, wide[i], -1, s.data(), bytes, nullptr, nullptr);
                s.resize (std::strlen (s.c_str()));
                out.push_back (std::move (s));
            }

            LocalFree (wide);
            return out;
        }
#endif

        for (int i = 0; i < argc; ++i)
            out.emplace_back (argv[i]);

        return out;
    }

    bool parseArgs (const std::vector<std::string>& args, Options& o, std::string& error)
    {
        for (size_t i = 1; i < args.size(); ++i)
        {
            const std::string& a = args[i];
            auto value = [&] (std::string& dst) -> bool
            {
                if (i + 1 >= args.size()) { error = "missing value after " + a; return false; }
                dst = args[++i];
                return true;
            };

            std::string v;

            if (a == "--fullscreen") o.fullscreen = 1;
            else if (a == "--windowed") o.fullscreen = 0;
            else if (a == "--scale") { if (! value (v)) return false; o.scale = std::atoi (v.c_str()); }
            else if (a == "--config") { if (! value (o.configPath)) return false; }
            else if (a == "--script") { if (! value (o.scriptPath)) return false; }
            else if (a == "--frames-max") { if (! value (v)) return false; o.maxFrames = std::atol (v.c_str()); }
            else if (a == "--no-audio") o.noAudio = true;
            else if (a == "--no-midi") o.noMidi = true;
            else if (a == "--no-input") o.noInput = true;
            else if (a == "--fake-input") o.fakeInput = true;
            else if (a == "--help" || a == "-h") o.help = true;
            else if (a == "--version") o.version = true;
            else if (! a.empty() && a[0] == '-') { error = "unknown option " + a; return false; }
            else o.file = a;
        }

        return true;
    }

    // ------------------------------------------------------------------------------------ audio

    // Flush denormal numbers to zero on the audio thread: they can make the arithmetic on small
    // ARM cores many times slower, and nobody can hear them.
    void flushDenormals() noexcept
    {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
        _mm_setcsr (_mm_getcsr() | 0x8040u);
#elif defined(__aarch64__)
        unsigned long long fpcr = 0;
        asm volatile ("mrs %0, fpcr" : "=r" (fpcr));
        fpcr |= (1ull << 24);
        asm volatile ("msr fpcr, %0" : : "r" (fpcr));
#endif
    }

    void SDLCALL audioCallback (void* userdata, Uint8* stream, int length)
    {
        static thread_local bool initialised = false;

        if (! initialised)
        {
            flushDenormals();
            initialised = true;
        }

        const int frames = length / (int) (2 * sizeof (float));
        static_cast<Engine*> (userdata)->render (reinterpret_cast<float*> (stream), frames);
    }

    // Owns the sound device. Declared after the Engine it renders, so it is closed first.
    struct AudioState
    {
        SDL_AudioDeviceID device = 0;
        int rate = 44100;
        int frames = 512;

        AudioState() = default;
        AudioState (const AudioState&) = delete;
        AudioState& operator= (const AudioState&) = delete;

        ~AudioState()
        {
            if (device != 0)
                SDL_CloseAudioDevice (device);
        }
    };

    void openAudio (AudioState& out, Engine& engine, const Config& cfg, bool allowed)
    {
        out.rate = cfg.audioRate;
        out.frames = cfg.audioBuffer;

        if (allowed)
        {
            if (SDL_WasInit (SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem (SDL_INIT_AUDIO) != 0)
            {
                logLine ("audio: cannot start the audio system: %s", SDL_GetError());
            }
            else
            {
                SDL_AudioSpec want, have;
                SDL_zero (want);
                SDL_zero (have);
                want.freq = cfg.audioRate;
                want.format = AUDIO_F32SYS;
                want.channels = 2;
                want.samples = (Uint16) cfg.audioBuffer;
                want.callback = audioCallback;
                want.userdata = &engine;

                // The sample rate and buffer size may change; the format and channel count may not
                // (SDL converts for us), so the callback can always write interleaved stereo floats.
                out.device = SDL_OpenAudioDevice (nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE);

                if (out.device == 0)
                {
                    logLine ("audio: cannot open the sound device: %s", SDL_GetError());
                }
                else
                {
                    out.rate = have.freq;
                    out.frames = have.samples;
                    logLine ("audio: driver %s, %d Hz, %d frames per block", SDL_GetCurrentAudioDriver() != nullptr ? SDL_GetCurrentAudioDriver() : "?",
                             have.freq, (int) have.samples);
                }
            }
        }

        // Prepared before the device starts, so the audio thread never sees a half-set-up engine.
        engine.prepare ((double) out.rate);

        if (out.device != 0)
            SDL_PauseAudioDevice (out.device, 0);
    }

    // ------------------------------------------------------------------------------------ video

    int chooseScale (int wanted)
    {
        wanted = std::clamp (wanted, 1, 4);
        SDL_Rect usable;

        if (SDL_GetDisplayUsableBounds (0, &usable) == 0 && usable.w > 0 && usable.h > 0)
        {
            const int fit = std::max (1, std::min ((usable.w - 40) / kScreenW, (usable.h - 60) / kScreenH)); // room for the title bar and borders
            return std::min (wanted, fit);
        }

        return wanted;
    }

    struct Video
    {
        SDL_Window* window = nullptr;
        SDL_Renderer* renderer = nullptr;
        SDL_Texture* texture = nullptr;

        ~Video()
        {
            if (texture != nullptr) SDL_DestroyTexture (texture);
            if (renderer != nullptr) SDL_DestroyRenderer (renderer);
            if (window != nullptr) SDL_DestroyWindow (window);
        }
    };

    bool openVideo (Video& v, bool fullscreen, int scale)
    {
        const int w = kScreenW * scale, h = kScreenH * scale;
        Uint32 flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;

        if (fullscreen)
            flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

        v.window = SDL_CreateWindow ("Kerf", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);

        if (v.window == nullptr)
        {
            fatal (std::string ("Could not open a window: ") + SDL_GetError());
            return false;
        }

        // Crisp pixels: the picture is only ever enlarged by whole numbers.
        SDL_SetHint (SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

        const Uint32 attempts[] = { SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC, SDL_RENDERER_ACCELERATED, SDL_RENDERER_SOFTWARE };

        for (const Uint32 flagsTry : attempts)
        {
            v.renderer = SDL_CreateRenderer (v.window, -1, flagsTry);

            if (v.renderer != nullptr)
                break;
        }

        if (v.renderer == nullptr)
        {
            fatal (std::string ("Could not create a renderer: ") + SDL_GetError());
            return false;
        }

        SDL_RendererInfo info;
        if (SDL_GetRendererInfo (v.renderer, &info) == 0)
            logLine ("video: driver %s, renderer %s%s", SDL_GetCurrentVideoDriver() != nullptr ? SDL_GetCurrentVideoDriver() : "?", info.name,
                     (info.flags & SDL_RENDERER_PRESENTVSYNC) ? " (vsync)" : "");

        SDL_RenderSetLogicalSize (v.renderer, kScreenW, kScreenH);
        SDL_RenderSetIntegerScale (v.renderer, SDL_TRUE);

        v.texture = SDL_CreateTexture (v.renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, kScreenW, kScreenH);

        if (v.texture == nullptr)
        {
            fatal (std::string ("Could not create the screen texture: ") + SDL_GetError());
            return false;
        }

        SDL_ShowCursor (fullscreen ? SDL_DISABLE : SDL_ENABLE);
        return true;
    }

    fs::path startFolder (const fs::path& baseDir)
    {
        std::error_code ec;

        if (fs::is_directory (baseDir / "samples", ec))
            return baseDir / "samples";

        return baseDir;
    }
}

namespace
{
    // Everything that needs SDL to be initialised. Returns the process exit code; SDL is shut down
    // by the caller only after every object in here (window, sound device, pads) is gone.
    int runHost (const Options& opt, Config& cfg, const fs::path& configFile, const fs::path& baseDir, const std::string& scriptText)
    {
        int exitCode = 0;

        if (SDL_InitSubSystem (SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0)
            logLine ("input: game pads are not available: %s", SDL_GetError());

        // A mapping file next to the program is added to SDL's own: handy for a pad SDL does not know.
        const auto dbFile = baseDir / "gamecontrollerdb.txt";
        std::error_code ec;

        if (fs::exists (dbFile, ec))
        {
            const int added = SDL_GameControllerAddMappingsFromFile (pathToUtf8 (dbFile).c_str());
            logLine ("input: gamecontrollerdb.txt: %d mapping(s) added", added);
        }

        auto engine = std::make_unique<Engine>();
        AudioState audio;
        openAudio (audio, *engine, cfg, ! opt.noAudio);

        Video video;
        const bool fullscreen = cfg.fullscreen;

        if (! openVideo (video, fullscreen, fullscreen ? 1 : chooseScale (cfg.windowScale)))
            return 1;

        PadTracker tracker;
        SdlInput input (tracker, cfg);
        input.openExisting();
        logLine ("input: %d pad(s) found", (int) SDL_NumJoysticks());

        // With a pad attached from the start (always the case on a handheld) the on-screen hints
        // use the pad's button names; the keyboard takes over as soon as a key is pressed.
        if (input.hasPad())
            tracker.preferDevice (PadTracker::Source::gamepad);

        MidiInput midi (engine->midiInbox());
        midi.setUsbDirect (cfg.usbMidi);
        midi.setLogger ([] (const std::string& line) { logText (line); });

        if (! opt.noMidi)
            midi.start();

        // What the Record screen can listen to: first the USB audio that Kerf reads itself (where
        // the system has no driver: the handheld), then the system's own inputs. Made before the app,
        // which closes its input when it goes.
        InputHub inputs;
        inputs.setLogger ([] (const std::string& line) { logText (line); });

        if (opt.fakeInput)
        {
            inputs.add (std::make_unique<FakeInput> (demoInputs()));
        }
        else if (! opt.noInput)
        {
            if (cfg.usbAudio)
                if (auto usb = makeUsbAudioInput())
                    inputs.add (std::move (usb));

            if (! opt.noAudio)
                inputs.add (makeSdlAudioInput());
        }

        App app (*engine, cfg, configFile, startFolder (baseDir));
        app.setInputHub (&inputs);
        app.setLogger ([] (const std::string& line) { logText (line); });

        std::unique_ptr<ScriptRunner> runner;

        if (! scriptText.empty())
        {
            runner = std::make_unique<ScriptRunner> (app, *engine);
            std::string scriptError;

            if (! runner->parse (scriptText, scriptError))
            {
                fatal ("Script error: " + scriptError);
                return 2;
            }
        }

        if (! opt.file.empty())
            app.loadFile (pathFromUtf8 (opt.file));

        Canvas canvas (kScreenW, kScreenH);
        const Uint64 freq = SDL_GetPerformanceFrequency();
        Uint64 last = SDL_GetPerformanceCounter();
        long frame = 0;
        bool isFullscreen = fullscreen;

        auto setFullscreen = [&] (bool on)
        {
            isFullscreen = on;
            cfg.fullscreen = on;
            SDL_SetWindowFullscreen (video.window, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            SDL_ShowCursor (on ? SDL_DISABLE : SDL_ENABLE);
        };

        while (! app.wantsQuit())
        {
            const Uint64 frameStart = SDL_GetPerformanceCounter();

            SDL_Event e;

            while (SDL_PollEvent (&e))
            {
                // Window-level keys belong to the host, not to the application.
                if (e.type == SDL_KEYDOWN && e.key.repeat == 0)
                {
                    const bool altEnter = (e.key.keysym.mod & KMOD_ALT) != 0
                                          && (e.key.keysym.scancode == SDL_SCANCODE_RETURN || e.key.keysym.scancode == SDL_SCANCODE_KP_ENTER);

                    if (e.key.keysym.scancode == SDL_SCANCODE_F11 || altEnter)
                    {
                        setFullscreen (! isFullscreen);
                        continue;
                    }
                }

                input.handleEvent (e);
            }

            if (runner)
                runner->beforeFrame (tracker);

            const Uint64 now = SDL_GetPerformanceCounter();
            // Scripts run on a fixed clock so a run behaves the same every time.
            const float dt = runner ? 1.0f / 60.0f : (float) ((double) (now - last) / (double) freq);
            last = now;

            const Pad pad = tracker.frame (dt);
            input.updateDebug();
            app.setInputDebug (input.debug());

            HostStatus status;
            status.audioInfo = integer (audio.rate) + "/" + integer (audio.frames);
            status.audioOk = audio.device != 0 || opt.noAudio;
            status.midiDevices = midi.deviceCount();
            status.midiUsbDirect = midi.usbDirectCount();
            status.midiMessages = midi.messageCount();
            status.midiLast = midi.lastMessage();
            app.setHostStatus (status);

            app.update (pad, dt);
            app.draw (canvas);

            if (runner)
                runner->afterFrame (canvas);

            SDL_UpdateTexture (video.texture, nullptr, canvas.pixels(), canvas.pitchBytes());
            SDL_RenderClear (video.renderer);
            SDL_RenderCopy (video.renderer, video.texture, nullptr, nullptr);
            SDL_RenderPresent (video.renderer);

            ++frame;

            if (opt.maxFrames > 0 && frame >= opt.maxFrames)
                break;

            if (runner && runner->finished())
                break;

            // Hold 60 frames per second even when the display has no vertical sync.
            const double spent = (double) (SDL_GetPerformanceCounter() - frameStart) / (double) freq;
            const double target = 1.0 / 60.0;

            if (spent < target - 0.0015)
                SDL_Delay ((Uint32) ((target - spent) * 1000.0));
        }

        if (runner && runner->failures() > 0)
        {
            logLine ("%d script check(s) failed", runner->failures());
            exitCode = 1;
        }

        // The settings (including the window mode) are remembered for the next start.
        app.saveSettings();

        // Leaving: `app`, `inputs`, `midi`, `input`, `video`, `audio` and finally `engine` are
        // destroyed in that order, so the sound, MIDI and USB audio threads are stopped before the
        // engine goes away.
        return exitCode;
    }
}

int main (int argc, char** argv)
{
    const auto args = utf8Args (argc, argv);
    Options opt;
    std::string error;

    if (! parseArgs (args, opt, error))
    {
        std::fprintf (stderr, "%s\n\n", error.c_str());
        printUsage();
        return 2;
    }

    if (opt.help) { printUsage(); return 0; }
    if (opt.version) { std::printf ("Kerf %s\n", kVersion); return 0; }

    // Everything the program writes (settings, log) lives next to it, so a handheld install is
    // one self-contained folder.
    fs::path baseDir = fs::current_path();

    if (char* base = SDL_GetBasePath())
    {
        baseDir = pathFromUtf8 (base);
        SDL_free (base);
    }

    // fs::path::c_str() is wide characters on Windows, so a folder name with accents is fine.
#if defined(_WIN32)
    gLog = _wfopen ((baseDir / "kerf-log.txt").c_str(), L"w");
#else
    gLog = std::fopen ((baseDir / "kerf-log.txt").c_str(), "w");
#endif
    SDL_version linked;
    SDL_GetVersion (&linked);
    logLine ("Kerf %s, SDL %d.%d.%d", kVersion, (int) linked.major, (int) linked.minor, (int) linked.patch);

    // Scripts are read before anything else is started so a typo is reported immediately.
    std::string scriptText;

    if (! opt.scriptPath.empty())
    {
        std::ifstream in (pathFromUtf8 (opt.scriptPath));

        if (! in)
        {
            fatal ("Cannot read the script " + opt.scriptPath);
            return 2;
        }

        std::stringstream ss;
        ss << in.rdbuf();
        scriptText = ss.str();
    }

    const fs::path configFile = opt.configPath.empty() ? baseDir / "kerf.cfg" : pathFromUtf8 (opt.configPath);
    Config cfg = Config::load (configFile);

    if (opt.fullscreen >= 0) cfg.fullscreen = opt.fullscreen == 1;
    if (opt.scale > 0) cfg.windowScale = std::clamp (opt.scale, 1, 4);

    // ---- SDL ----
    if (SDL_Init (SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0)
    {
        fatal (std::string ("Could not start SDL: ") + SDL_GetError());
        return 1;
    }

    const int exitCode = runHost (opt, cfg, configFile, baseDir, scriptText);

    SDL_Quit();

    if (gLog != nullptr)
        std::fclose (gLog);

    return exitCode;
}
