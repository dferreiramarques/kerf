// kerf_headless: runs the whole application without a window or sound card, driven by a script.
// It renders the screen into a Canvas (screenshots) and the audio into a buffer (optionally
// written as a WAV), at a fixed 60 frames per second, so end-to-end behaviour can be tested and
// looked at on any machine.
//
//   kerf_headless --script test.txt [--audio-out out.wav] [--rate 44100] [--frames-max 36000]
//                 [--fake-pad] [--fake-midi] [--fake-input] [--projects] [--start-dir DIR]
//
// Projects (the .kerf file next to a sample) are off unless --projects is given, so that the scripts
// that load the demo sample from the repository do not write into it.
//
// --fake-input pretends four audio inputs (a USB microphone, a receiver that sends nothing, a
// 12-channel mixer and one that cannot be opened), so the Record screen can be tried. --start-dir
// is the folder the file browser opens first and the Record screen saves its takes in (default:
// the current folder).

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "app.h"
#include "audio_input.h"
#include "canvas.h"
#include "config.h"
#include "engine.h"
#include "fake_input.h"
#include "input.h"
#include "path_util.h"
#include "script_runner.h"

using namespace kerf;
using namespace kerf::ui;

int main (int argc, char** argv)
{
    std::string scriptPath, audioOut;
    bool fakePad = false;
    bool fakeMidi = false;
    bool fakeInput = false;
    bool projects = false;
    std::string startDir;
    int rate = 44100;
    long maxFrames = 60 * 600;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&] () -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };

        if (a == "--script") scriptPath = next();
        else if (a == "--audio-out") audioOut = next();
        else if (a == "--fake-pad") fakePad = true;
        else if (a == "--fake-midi") fakeMidi = true;
        else if (a == "--fake-input") fakeInput = true;
        else if (a == "--projects") projects = true;
        else if (a == "--start-dir") startDir = next();
        else if (a == "--rate") rate = std::atoi (next().c_str());
        else if (a == "--frames-max") maxFrames = std::atol (next().c_str());
        else { std::fprintf (stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    if (scriptPath.empty())
    {
        std::fprintf (stderr, "usage: kerf_headless --script file [--audio-out out.wav] [--rate 44100] [--fake-pad] [--fake-midi]"
                              " [--fake-input] [--projects] [--start-dir DIR]\n");
        return 2;
    }

    std::ifstream in (pathFromUtf8 (scriptPath));
    if (! in)
    {
        std::fprintf (stderr, "cannot read %s\n", scriptPath.c_str());
        return 2;
    }

    std::stringstream ss;
    ss << in.rdbuf();

    Config cfg; // defaults: tests never touch the user's settings file
    cfg.projects = projects;   // and a test does not leave a project file next to its sample unless it asks to
    Engine engine;
    engine.prepare ((double) rate);

    // The hub is made before the app, so that it is still there when the app closes its input.
    InputHub inputs;

    if (fakeInput) // two pretend inputs: a USB microphone (read by Kerf) and a mixer with twelve channels
    {
        inputs.add (std::make_unique<FakeInput> (demoInputs()));
    }

    App app (engine, cfg, {}, startDir.empty() ? std::filesystem::current_path() : pathFromUtf8 (startDir));

    if (! inputs.empty())
        app.setInputHub (&inputs);

    HostStatus status;
    status.audioInfo = std::to_string (rate) + "/512";
    status.audioOk = true;

    if (fakeMidi) // pretend a USB keyboard is attached that Kerf reads itself, and has just sent a note
    {
        status.midiDevices = 1;
        status.midiUsbDirect = 1;
        status.midiMessages = 1;
        status.midiLast = 0x903C64;
    }

    app.setHostStatus (status);

    ScriptRunner runner (app, engine);
    std::string error;

    if (! runner.parse (ss.str(), error))
    {
        std::fprintf (stderr, "script error: %s\n", error.c_str());
        return 2;
    }

    Canvas canvas (kScreenW, kScreenH);
    PadTracker tracker;
    const float dt = 1.0f / 60.0f;
    double frameAccum = 0.0;
    std::vector<float> block, captured;

    for (long frame = 0; frame < maxFrames; ++frame)
    {
        runner.beforeFrame (tracker);
        const Pad pad = tracker.frame (dt);

        if (fakePad) // pretend a controller is attached, so the "Input test" screen can be looked at
        {
            InputDebug d;
            d.deviceName = "Fake test pad";
            d.isController = true;
            d.axes = 6; d.buttons = 15; d.hats = 0;
            d.axis[0] = pad.lx; d.axis[1] = pad.ly; d.axis[2] = pad.rx; d.axis[3] = pad.ry;
            d.lastButton = 1; d.lastControllerButton = "b";
            app.setInputDebug (d);
        }

        app.update (pad, dt);

        // The audio thread's share of this frame: 1/60 s of sound.
        frameAccum += (double) rate / 60.0;
        const int n = (int) frameAccum;
        frameAccum -= n;
        block.assign ((size_t) n * 2, 0.0f);
        engine.render (block.data(), n);

        if (! audioOut.empty())
            captured.insert (captured.end(), block.begin(), block.end());

        app.draw (canvas);
        runner.afterFrame (canvas);

        if (runner.finished() || app.wantsQuit())
            break;
    }

    if (! audioOut.empty())
    {
        AudioBuffer buf (2, (int) (captured.size() / 2));

        for (int i = 0; i < buf.numFrames(); ++i)
        {
            buf.set (0, i, captured[(size_t) i * 2]);
            buf.set (1, i, captured[(size_t) i * 2 + 1]);
        }

        std::ofstream out (pathFromUtf8 (audioOut), std::ios::binary);
        const auto wav = encodeWav16 (buf, (double) rate);
        out.write (wav.data(), (std::streamsize) wav.size());
    }

    if (runner.failures() > 0)
    {
        std::fprintf (stderr, "%d script check(s) failed\n", runner.failures());
        return 1;
    }

    return 0;
}
