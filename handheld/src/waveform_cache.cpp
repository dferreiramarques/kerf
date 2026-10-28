#include "waveform_cache.h"

#include <algorithm>
#include <cmath>

namespace kerf::ui
{
    void WaveformCache::clear()
    {
        sample_.reset();
        levels_.clear();
    }

    void WaveformCache::build (kerf::SamplePtr sample)
    {
        clear();

        if (sample == nullptr || sample->data.empty())
            return;

        sample_ = std::move (sample);
        const auto& data = sample_->data;
        const long long frames = data.numFrames();
        const int channels = data.numChannels();

        // Level 0: straight from the audio.
        Level base;
        base.blockFrames = baseBlock;
        const long long blocks0 = (frames + baseBlock - 1) / baseBlock;
        base.lo.resize ((size_t) blocks0);
        base.hi.resize ((size_t) blocks0);

        for (long long b = 0; b < blocks0; ++b)
        {
            const long long a = b * baseBlock;
            const long long e = std::min (frames, a + baseBlock);
            float lo = 1e9f, hi = -1e9f;

            for (int ch = 0; ch < channels; ++ch)
            {
                const float* p = data.channel (ch);

                for (long long i = a; i < e; ++i)
                {
                    lo = std::min (lo, p[i]);
                    hi = std::max (hi, p[i]);
                }
            }

            base.lo[(size_t) b] = lo;
            base.hi[(size_t) b] = hi;
        }

        levels_.push_back (std::move (base));

        // Coarser levels until a single block covers the whole file.
        while (levels_.back().lo.size() > 1)
        {
            const auto& prev = levels_.back();
            Level next;
            next.blockFrames = prev.blockFrames * fanOut;
            const size_t n = (prev.lo.size() + fanOut - 1) / fanOut;
            next.lo.resize (n);
            next.hi.resize (n);

            for (size_t b = 0; b < n; ++b)
            {
                float lo = 1e9f, hi = -1e9f;
                const size_t from = b * fanOut, to = std::min (prev.lo.size(), from + fanOut);

                for (size_t i = from; i < to; ++i)
                {
                    lo = std::min (lo, prev.lo[i]);
                    hi = std::max (hi, prev.hi[i]);
                }

                next.lo[b] = lo;
                next.hi[b] = hi;
            }

            levels_.push_back (std::move (next));
        }
    }

    void WaveformCache::rawRange (long long a, long long b, float& lo, float& hi) const noexcept
    {
        const auto& data = sample_->data;
        lo = 1e9f;
        hi = -1e9f;

        for (int ch = 0; ch < data.numChannels(); ++ch)
        {
            const float* p = data.channel (ch);

            for (long long i = a; i < b; ++i)
            {
                lo = std::min (lo, p[i]);
                hi = std::max (hi, p[i]);
            }
        }
    }

    void WaveformCache::columns (double t0, double t1, int width, Column* out) const
    {
        for (int i = 0; i < width; ++i)
            out[i] = Column {};

        if (sample_ == nullptr || width <= 0 || t1 <= t0)
            return;

        const double rate = sample_->sampleRate;
        const long long total = sample_->data.numFrames();
        const double f0 = t0 * rate;
        const double framesPerPixel = (t1 - t0) * rate / (double) width;

        // The coarsest level whose blocks are no bigger than one pixel's worth of audio.
        int level = -1;

        for (int l = 0; l < (int) levels_.size(); ++l)
            if ((double) levels_[(size_t) l].blockFrames <= framesPerPixel)
                level = l;

        for (int x = 0; x < width; ++x)
        {
            const long long a = (long long) std::floor (f0 + framesPerPixel * x);
            const long long b = std::max (a + 1, (long long) std::ceil (f0 + framesPerPixel * (x + 1)));

            if (b <= 0 || a >= total)
                continue;

            const long long ca = std::max<long long> (a, 0);
            const long long cb = std::min<long long> (b, total);
            float lo, hi;

            if (level < 0)
            {
                rawRange (ca, cb, lo, hi);
            }
            else
            {
                const auto& L = levels_[(size_t) level];
                const long long first = ca / L.blockFrames;
                const long long last = std::min<long long> ((cb - 1) / L.blockFrames, (long long) L.lo.size() - 1);
                lo = 1e9f;
                hi = -1e9f;

                for (long long k = first; k <= last; ++k)
                {
                    lo = std::min (lo, L.lo[(size_t) k]);
                    hi = std::max (hi, L.hi[(size_t) k]);
                }
            }

            out[x] = { lo, hi, true };
        }
    }
}
