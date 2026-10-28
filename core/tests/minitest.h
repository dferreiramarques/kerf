#pragma once

// A deliberately tiny test harness (no dependencies) so the core's tests build anywhere a C++
// compiler exists, including on the handheld's toolchain.

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace minitest
{
    struct Case
    {
        std::string suite;
        std::string name;
        std::function<void()> fn;
    };

    inline std::vector<Case>& registry()
    {
        static std::vector<Case> r;
        return r;
    }

    inline int& failures()
    {
        static int f = 0;
        return f;
    }

    inline int& checks()
    {
        static int c = 0;
        return c;
    }

    struct Registrar
    {
        Registrar (const char* suite, const char* name, std::function<void()> fn)
        {
            registry().push_back ({ suite, name, std::move (fn) });
        }
    };

    inline int runAll()
    {
        int failedCases = 0;

        for (auto& c : registry())
        {
            const auto before = failures();

            try { c.fn(); }
            catch (const std::exception& e)
            {
                std::printf ("  EXCEPTION in %s / %s: %s\n", c.suite.c_str(), c.name.c_str(), e.what());
                ++failures();
            }

            const bool ok = failures() == before;
            if (! ok) ++failedCases;
            std::printf ("[%s] %s / %s\n", ok ? " ok " : "FAIL", c.suite.c_str(), c.name.c_str());
        }

        std::printf ("\n%zu test cases, %d checks, %d failed checks (%d failed cases)\n",
                     registry().size(), checks(), failures(), failedCases);
        return failures() == 0 ? 0 : 1;
    }
}

#define KTEST(suite, name) \
    static void suite##_##name(); \
    static minitest::Registrar reg_##suite##_##name (#suite, #name, suite##_##name); \
    static void suite##_##name()

#define EXPECT(cond) \
    do { ++minitest::checks(); if (! (cond)) { ++minitest::failures(); \
        std::printf ("    %s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, #cond); } } while (0)

#define EXPECT_EQ(a, b) \
    do { ++minitest::checks(); const auto va_ = (a); const auto vb_ = (b); if (! (va_ == vb_)) { ++minitest::failures(); \
        std::printf ("    %s:%d: EXPECT_EQ(%s, %s) failed: %g vs %g\n", __FILE__, __LINE__, #a, #b, (double) va_, (double) vb_); } } while (0)

// Two pieces of text that should be the same; when they are not, both are shown (the first 300 bytes).
#define EXPECT_TEXT(a, b) \
    do { ++minitest::checks(); const std::string va_ = (a); const std::string vb_ = (b); if (va_ != vb_) { ++minitest::failures(); \
        std::printf ("    %s:%d: EXPECT_TEXT(%s, %s) failed:\n      got:      \"%s\"\n      expected: \"%s\"\n", __FILE__, __LINE__, #a, #b, \
                     va_.substr (0, 300).c_str(), vb_.substr (0, 300).c_str()); } } while (0)

#define EXPECT_NEAR(a, b, eps) \
    do { ++minitest::checks(); const double va_ = (double) (a); const double vb_ = (double) (b); \
        if (! (std::fabs (va_ - vb_) <= (double) (eps))) { ++minitest::failures(); \
        std::printf ("    %s:%d: EXPECT_NEAR(%s, %s, %s) failed: %g vs %g\n", __FILE__, __LINE__, #a, #b, #eps, va_, vb_); } } while (0)
