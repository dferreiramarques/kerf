#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace kerf
{
    struct MidiMessage
    {
        uint8_t status = 0; // includes the channel nibble
        uint8_t data1 = 0;
        uint8_t data2 = 0;

        int command() const noexcept { return status & 0xF0; }
        int channel() const noexcept { return status & 0x0F; }
        bool isNoteOn() const noexcept { return command() == 0x90 && data2 > 0; }
        // Note-on with velocity 0 counts as note-off, as the original does.
        bool isNoteOff() const noexcept { return command() == 0x80 || (command() == 0x90 && data2 == 0); }
    };

    // Turns a raw MIDI byte stream (USB-MIDI raw device, WinMM, ...) into messages. Handles
    // running status, ignores real-time bytes (they may appear mid-message) and skips SysEx.
    class MidiParser
    {
    public:
        // Feeds one byte; returns true when `out` holds a completed channel message.
        bool feed (uint8_t byte, MidiMessage& out) noexcept
        {
            if (byte >= 0xF8)
                return false; // real-time: ignore, must not disturb running status

            if (byte >= 0x80)
            {
                if (byte == 0xF0) { inSysex_ = true; runningStatus_ = 0; have_ = 0; return false; }
                if (byte == 0xF7) { inSysex_ = false; return false; }

                inSysex_ = false;
                have_ = 0;

                if (byte >= 0xF0) { runningStatus_ = 0; return false; } // system common: unsupported

                runningStatus_ = byte;
                return false;
            }

            if (inSysex_ || runningStatus_ == 0)
                return false;

            data_[have_++] = byte;

            if (have_ >= expectedData (runningStatus_))
            {
                out.status = runningStatus_;
                out.data1 = data_[0];
                out.data2 = expectedData (runningStatus_) > 1 ? data_[1] : 0;
                have_ = 0;
                return true;
            }

            return false;
        }

        void reset() noexcept { runningStatus_ = 0; have_ = 0; inSysex_ = false; }

    private:
        static int expectedData (uint8_t status) noexcept
        {
            const auto c = status & 0xF0;
            return (c == 0xC0 || c == 0xD0) ? 1 : 2;
        }

        uint8_t runningStatus_ = 0;
        uint8_t data_[2] = { 0, 0 };
        int have_ = 0;
        bool inSysex_ = false;
    };

    // Multi-producer inbox for MIDI messages coming from driver threads. The audio thread drains
    // it with try_lock, so it can never block: if a producer holds the lock at that instant the
    // drain is simply skipped and the events are picked up on the next block.
    class MidiInbox
    {
    public:
        void push (const MidiMessage& m)
        {
            std::lock_guard<std::mutex> lock (mutex_);

            if (pending_.size() < 1024)
                pending_.push_back (m);
        }

        // Moves pending messages into `out` (cleared first). Returns false if the lock was busy.
        bool tryDrain (std::vector<MidiMessage>& out)
        {
            out.clear();

            if (! mutex_.try_lock())
                return false;

            out.swap (pending_);
            mutex_.unlock();
            return true;
        }

    private:
        std::mutex mutex_;
        std::vector<MidiMessage> pending_;
    };
}
