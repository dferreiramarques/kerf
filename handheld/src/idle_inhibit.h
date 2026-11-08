#pragma once

// Keeps the handheld awake while Kerf is making sound.
//
// muOS (2606.0) dims the screen after "Idle Input Display Timeout", mutes the sound with it ("Mute on
// Display Timeout") and later sleeps ("Idle Input Sleep Timeout"), counting only the time since the
// last button press. A song on the sequencer plays with no buttons pressed, so it was cut off.
//
// muOS's hotkey daemon (muhotkey) skips both timers while /run/muos/idle_game_inhibit (screen) and
// /run/muos/idle_sleep_inhibit (sleep) hold the PID of a running process; muOS's own video player
// writes its PID there while it plays. Kerf does the same while it sounds, and takes its PID out again
// when it stops, so the screen still dims and the handheld still sleeps when nothing is playing.
// The timers start counting again from that moment.
//
// Elsewhere (Windows, other firmwares without /run/muos) this does nothing.

#include <string>

#if defined(__linux__)
 #include <cstdio>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

namespace kerf::ui
{
    class IdleInhibit
    {
    public:
        IdleInhibit()
        {
#if defined(__linux__)
            struct stat st {};
            supported_ = ::stat (kRunDir, &st) == 0 && S_ISDIR (st.st_mode);
            pid_ = (long) ::getpid();
#endif
        }

        ~IdleInhibit() { set (false); }

        IdleInhibit (const IdleInhibit&) = delete;
        IdleInhibit& operator= (const IdleInhibit&) = delete;

        bool supported() const noexcept { return supported_; }
        bool active() const noexcept { return active_; }

        // Call whenever the answer may have changed (once a frame is fine: it only touches the files
        // when the answer changes). Returns true when it changed.
        bool set (bool on)
        {
            if (! supported_ || on == active_)
                return false;

            active_ = on;

#if defined(__linux__)
            for (const char* file : kFiles)
            {
                if (on)
                    writePid (file);
                else
                    clearIfOurs (file);
            }
#endif
            return true;
        }

    private:
#if defined(__linux__)
        static constexpr const char* kRunDir = "/run/muos";
        static constexpr const char* kFiles[2] = { "/run/muos/idle_game_inhibit", "/run/muos/idle_sleep_inhibit" };

        void writePid (const char* file) const
        {
            if (std::FILE* f = std::fopen (file, "w"))
            {
                std::fprintf (f, "%ld\n", pid_);
                std::fclose (f);
            }
        }

        // Only our own PID is taken out: if something else wrote its own since, it is left alone.
        void clearIfOurs (const char* file) const
        {
            long pid = 0;

            if (std::FILE* f = std::fopen (file, "r"))
            {
                if (std::fscanf (f, "%ld", &pid) != 1)
                    pid = 0;

                std::fclose (f);
            }

            if (pid == pid_)
                std::remove (file);
        }

        long pid_ = 0;
#endif
        bool supported_ = false;
        bool active_ = false;
    };
}
