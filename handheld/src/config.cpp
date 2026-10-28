#include "config.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace kerf::ui
{
    namespace
    {
        std::string trim (const std::string& s)
        {
            size_t a = 0, b = s.size();
            while (a < b && std::isspace ((unsigned char) s[a])) ++a;
            while (b > a && std::isspace ((unsigned char) s[b - 1])) --b;
            return s.substr (a, b - a);
        }

        bool parseInt (const std::string& v, int& out)
        {
            char* end = nullptr;
            const long n = std::strtol (v.c_str(), &end, 10);
            if (end == v.c_str() || *end != '\0') return false;
            out = (int) std::clamp<long> (n, -1000000, 1000000);
            return true;
        }

        bool parseFloat (const std::string& v, float& out)
        {
            char* end = nullptr;
            const double d = std::strtod (v.c_str(), &end);
            if (end == v.c_str() || *end != '\0' || ! std::isfinite (d)) return false;
            out = (float) d;
            return true;
        }

        bool parseBool (const std::string& v, bool& out)
        {
            if (v == "1" || v == "true" || v == "yes" || v == "on") { out = true; return true; }
            if (v == "0" || v == "false" || v == "no" || v == "off") { out = false; return true; }
            return false;
        }
    }

    void Config::sanitize()
    {
        masterVolume = std::clamp (masterVolume, 0.0f, 1.0f);
        midiMode = midiMode == 1 ? 1 : 0;
        noteOffset = std::clamp (noteOffset, 0, 127);
        audioBuffer = std::clamp (audioBuffer, 64, 4096);
        audioRate = (audioRate == 48000 || audioRate == 22050 || audioRate == 32000) ? audioRate : 44100;
        stickSpeed = std::clamp (stickSpeed, 0.1f, 2.0f);
        deadzone = std::clamp (deadzone, 0.05f, 0.6f);
        windowScale = std::clamp (windowScale, 1, 4);
        recordLimit = std::clamp (recordLimit, 5, 300);
        recordGainDb = std::clamp (recordGainDb, -12, 30);

        // The names are written on one line: no line breaks or other control characters, and not endless.
        for (std::string* name : { &recordInput, &recordChannels })
        {
            name->erase (std::remove_if (name->begin(), name->end(), [] (unsigned char ch) { return ch < 0x20; }), name->end());

            if (name->size() > 200)
                name->resize (200);
        }

        for (auto& a : joyAxis) a = std::clamp (a, 0, 31);
        for (auto& b : joyButton) b = std::clamp (b, -1, 63);
    }

    std::string Config::serialize() const
    {
        std::ostringstream o;
        o << "# Kerf settings. Edit while the program is closed.\n";
        o << "master_volume = " << masterVolume << "\n";
        o << "midi_mode = " << midiMode << "   # 0 trigger, 1 sampler\n";
        o << "note_offset = " << noteOffset << "\n";
        o << "usb_midi = " << (usbMidi ? 1 : 0) << "   # 1 = read USB MIDI keyboards that the system has no driver for\n";
        o << "usb_audio = " << (usbAudio ? 1 : 0) << "   # 1 = read USB microphones and audio interfaces that the system has no driver for\n";
        o << "audio_buffer = " << audioBuffer << "\n";
        o << "audio_rate = " << audioRate << "\n";
        o << "swap_ab = " << (swapAB ? 1 : 0) << "\n";
        o << "swap_xy = " << (swapXY ? 1 : 0) << "\n";
        o << "stick_speed = " << stickSpeed << "\n";
        o << "deadzone = " << deadzone << "\n";
        o << "fullscreen = " << (fullscreen ? 1 : 0) << "\n";
        o << "window_scale = " << windowScale << "\n";
        o << "joy_axis_lx = " << joyAxis[0] << "\n";
        o << "joy_axis_ly = " << joyAxis[1] << "\n";
        o << "joy_axis_rx = " << joyAxis[2] << "\n";
        o << "joy_axis_ry = " << joyAxis[3] << "\n";

        for (int i = 0; i < kBtnCount; ++i)
            o << "joy_btn_" << btnName ((Btn) i) << " = " << joyButton[i] << "\n";

        o << "record_limit = " << recordLimit << "   # longest take in seconds\n";
        o << "record_gain = " << recordGainDb << "   # dB, -12 to 30\n";
        o << "record_input = " << recordInput << "\n";
        o << "record_channels = " << recordChannels << "\n";
        o << "autosave = " << (projects ? 1 : 0) << "   # 1 = also keep the work on each sample in name.wav.kerf next to it, without being asked (projects are saved by hand from the Start menu)\n";
        o << "last_dir = " << lastDir << "\n";
        return o.str();
    }

    Config Config::parse (const std::string& text)
    {
        Config c;
        std::istringstream in (text);
        std::string line;

        while (std::getline (in, line))
        {
            const auto eq = line.find ('=');
            if (eq == std::string::npos || trim (line).rfind ('#', 0) == 0)
                continue;

            auto key = trim (line.substr (0, eq));
            auto value = line.substr (eq + 1);

            // Numbers may carry a trailing "# comment"; a folder name (last_dir) and the name of an
            // input (record_input: "usb:0644:805f@1-1.2#2") may contain '#'.
            if (key != "last_dir" && key != "record_input")
            {
                const auto hash = value.find ('#');
                if (hash != std::string::npos)
                    value.erase (hash);
            }

            value = trim (value);
            int i; float f; bool b;

            if (key == "master_volume" && parseFloat (value, f)) c.masterVolume = f;
            else if (key == "midi_mode" && parseInt (value, i)) c.midiMode = i;
            else if (key == "note_offset" && parseInt (value, i)) c.noteOffset = i;
            else if (key == "usb_midi" && parseBool (value, b)) c.usbMidi = b;
            else if (key == "usb_audio" && parseBool (value, b)) c.usbAudio = b;
            else if (key == "audio_buffer" && parseInt (value, i)) c.audioBuffer = i;
            else if (key == "audio_rate" && parseInt (value, i)) c.audioRate = i;
            else if (key == "swap_ab" && parseBool (value, b)) c.swapAB = b;
            else if (key == "swap_xy" && parseBool (value, b)) c.swapXY = b;
            else if (key == "stick_speed" && parseFloat (value, f)) c.stickSpeed = f;
            else if (key == "deadzone" && parseFloat (value, f)) c.deadzone = f;
            else if (key == "fullscreen" && parseBool (value, b)) c.fullscreen = b;
            else if (key == "window_scale" && parseInt (value, i)) c.windowScale = i;
            else if (key == "joy_axis_lx" && parseInt (value, i)) c.joyAxis[0] = i;
            else if (key == "joy_axis_ly" && parseInt (value, i)) c.joyAxis[1] = i;
            else if (key == "joy_axis_rx" && parseInt (value, i)) c.joyAxis[2] = i;
            else if (key == "joy_axis_ry" && parseInt (value, i)) c.joyAxis[3] = i;
            else if (key == "record_limit" && parseInt (value, i)) c.recordLimit = i;
            else if (key == "record_gain" && parseInt (value, i)) c.recordGainDb = i;
            else if (key == "record_input") c.recordInput = value;
            else if (key == "record_channels") c.recordChannels = value;
            else if (key == "autosave" && parseBool (value, b)) c.projects = b;
            else if (key == "last_dir") c.lastDir = value;
            else if (key.rfind ("joy_btn_", 0) == 0 && parseInt (value, i))
            {
                Btn which;
                if (btnFromName (key.substr (8), which))
                    c.joyButton[(int) which] = i;
            }
        }

        c.sanitize();
        return c;
    }

    Config Config::load (const std::filesystem::path& file)
    {
        std::ifstream in (file, std::ios::binary);
        if (! in)
            return Config {};

        std::ostringstream ss;
        ss << in.rdbuf();
        return parse (ss.str());
    }

    bool Config::save (const std::filesystem::path& file) const
    {
        std::ofstream out (file, std::ios::binary | std::ios::trunc);
        if (! out)
            return false;

        out << serialize();
        return (bool) out;
    }
}
