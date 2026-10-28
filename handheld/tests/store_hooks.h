#pragma once

// A way for the project store tests to watch the two system calls that make a save safe - fsync (the
// bytes are on the disk) and rename (the new file takes the old one's place) - and to make each of
// the calls that write the file fail, one at a time, the way a full or failing disk would. Only on
// Linux: the test program is linked with -Wl,--wrap=... for fwrite, fflush, fsync, fclose and rename,
// which sends the store's calls to store_hooks.cpp, and that passes them on to the real ones.
// (Where this is not so, KERF_STORE_HOOKS is not defined and the tests that need it are left out.)

#include <string>
#include <vector>

namespace kerf::test::syscalls
{
    enum class Call { fwrite, fflush, fsync, fclose };

    // Clears the log and starts recording. Calls made while not recording are not logged or spoiled.
    void start();

    // Stops recording and returns what was logged, in order:
    //   "fsync <file name> <size of the file at that moment>"
    //   "rename <from file name> <to file name>"
    std::vector<std::string> stop();

    // While recording, the next call of this kind does nothing and fails with `error` (an errno
    // value): fwrite writes nothing, fflush and fsync flush nothing, and fclose closes the file but
    // says it could not.
    void failNext (Call call, int error);
}
