// See store_hooks.h. Linux only.

#include "store_hooks.h"

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <system_error>

extern "C"
{
    std::size_t __real_fwrite (const void* data, std::size_t size, std::size_t count, std::FILE* file);
    int __real_fflush (std::FILE* file);
    int __real_fsync (int fd);
    int __real_fclose (std::FILE* file);
    int __real_rename (const char* from, const char* to);
}

namespace
{
    using kerf::test::syscalls::Call;

    std::mutex lock;
    bool recording = false;
    int failWith[4] = {};
    std::vector<std::string> events;

    // Under the lock: is a failure armed for this call? It is used up by being asked for.
    int takeFailure (Call call)
    {
        if (! recording)
            return 0;

        int& armed = failWith[(int) call];
        const int error = armed;
        armed = 0;
        return error;
    }

    std::string link (int fd)
    {
        return "/proc/self/fd/" + std::to_string (fd);
    }

    std::string nameOfFd (int fd)
    {
        std::error_code ec;
        const auto target = std::filesystem::read_symlink (link (fd), ec);
        return ec ? std::string ("?") : target.filename().string();
    }
}

namespace kerf::test::syscalls
{
    void start()
    {
        std::lock_guard<std::mutex> g (lock);
        events.clear();

        for (int& armed : failWith)
            armed = 0;

        recording = true;
    }

    std::vector<std::string> stop()
    {
        std::lock_guard<std::mutex> g (lock);
        recording = false;

        for (int& armed : failWith)
            armed = 0;

        return std::move (events);
    }

    void failNext (Call call, int error)
    {
        std::lock_guard<std::mutex> g (lock);
        failWith[(int) call] = error;
    }
}

extern "C" std::size_t __wrap_fwrite (const void* data, std::size_t size, std::size_t count, std::FILE* file)
{
    {
        std::lock_guard<std::mutex> g (lock);

        if (const int error = takeFailure (Call::fwrite))
        {
            errno = error;
            return 0;
        }
    }

    return __real_fwrite (data, size, count, file);
}

extern "C" int __wrap_fflush (std::FILE* file)
{
    if (file != nullptr)
    {
        std::lock_guard<std::mutex> g (lock);

        if (const int error = takeFailure (Call::fflush))
        {
            errno = error;
            return EOF;
        }
    }

    return __real_fflush (file);
}

extern "C" int __wrap_fsync (int fd)
{
    {
        std::lock_guard<std::mutex> g (lock);

        if (recording)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size (link (fd), ec);
            events.push_back ("fsync " + nameOfFd (fd) + " " + std::to_string (ec ? -1 : (long long) size));

            if (const int error = takeFailure (Call::fsync))
            {
                errno = error;
                return -1;
            }
        }
    }

    return __real_fsync (fd);
}

extern "C" int __wrap_fclose (std::FILE* file)
{
    int error = 0;

    {
        std::lock_guard<std::mutex> g (lock);
        error = takeFailure (Call::fclose);
    }

    const int result = __real_fclose (file);      // (the file is closed either way: only the answer is spoiled)

    if (error != 0)
    {
        errno = error;
        return EOF;
    }

    return result;
}

extern "C" int __wrap_rename (const char* from, const char* to)
{
    {
        std::lock_guard<std::mutex> g (lock);

        if (recording)
            events.push_back ("rename " + std::filesystem::path (from).filename().string() + " " + std::filesystem::path (to).filename().string());
    }

    return __real_rename (from, to);
}
