// rt_stretch.h - real-time key-lock playback for Melt Deck (streaming phase vocoder).
//
// SPDX-License-Identifier: MIT
//
// Plays a stereo int16 track at any speed with the pitch unchanged. Same core as Melt Flip's
// offline renderer (2048-point frames, 75 % overlap, identity phase locking, transient phase
// reset, L+R packed in one FFT, stereo-linked phases), but streaming:
//   * frames are made on demand, 512 output samples at a time;
//   * jump(p) (cue, hot cue, loop, beat jump) re-primes the vocoder: it silently computes the
//     frames before p so the very first sample out is p at full level - hits on a cue point
//     stay sharp - and the caller fades the old sound out (see Deck::ghost).
// Audio-thread safe: all memory is allocated in the constructor.
#pragma once
#include "analysis.h"

namespace deck {

using flip::cfloat;

struct Track
{
    std::vector<int16_t> l, r;
    double bpm = 120, beatPhase = 0;   // samples
    int keyIndex = -1;
    float gain = 1.0f;                 // auto-gain (loudness match)
    size_t frames() const { return l.size(); }
};

class KeyLock
{
public:
    static constexpr int N = 2048, Hs = 512, Bins = N / 2 + 1;

    KeyLock() : plan_(N), z_(N), win_(N), accL_(N, 0.0f), accR_(N, 0.0f), fifoL_(kFifo, 0.0f), fifoR_(kFifo, 0.0f),
                phIn_(Bins), phPrev_(Bins), phOut_(Bins), mag_(Bins), logPrev_(Bins), XL_(Bins), XR_(Bins), peaks_((size_t)Bins)
    {
        for (int i = 0; i < N; ++i) win_[(size_t)i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * i / N);
        norm_ = 0; for (int i = 0; i < N; ++i) norm_ += win_[(size_t)i] * win_[(size_t)i]; norm_ = (float)Hs / norm_;   // OLA gain for Hann^2 at hop Hs
    }

    // Start playing so that the next output sample is source position p (in samples).
    void jump(const Track& t, double p, double rate)
    {
        std::fill(accL_.begin(), accL_.end(), 0.0f); std::fill(accR_.begin(), accR_.end(), 0.0f);
        fifoCount_ = 0; fifoRead_ = 0;
        apos_ = p - (N / 2) * rate;       // first frame centre: output sample N <-> p (see below)
        reset_ = true; prevA_ = (long)std::lround(apos_);
        fluxMed_ = 0.0f;
        // prime: the first N output samples are incomplete (fewer overlapping frames); make and discard them
        for (int k = 0; k < N / Hs; ++k) frame(t, rate);
        fifoCount_ = 0; fifoRead_ = 0;
    }

    // Fill n samples (adds nothing; writes). rate = source samples per output sample.
    void pull(const Track& t, float* L, float* R, int n, double rate)
    {
        int done = 0;
        while (done < n)
        {
            if (fifoCount_ == 0) frame(t, rate);
            const int take = std::min(n - done, fifoCount_);
            for (int i = 0; i < take; ++i)
            {
                const int k = (fifoRead_ + i) & (kFifo - 1);
                L[done + i] = fifoL_[(size_t)k]; R[done + i] = fifoR_[(size_t)k];
            }
            fifoRead_ = (fifoRead_ + take) & (kFifo - 1);
            fifoCount_ -= take; done += take;
        }
    }

private:
    static constexpr int kFifo = 1024;   // power of two, >= Hs

    void frame(const Track& t, double rate)
    {
        const long n = (long)t.frames();
        const long a = (long)std::lround(apos_);
        const long start = a - N / 2;
        const float s = 1.0f / 32768.0f;
        for (int i = 0; i < N; ++i)
        {
            const long j = start + i;
            const float l = (j >= 0 && j < n) ? t.l[(size_t)j] * s : 0.0f, r = (j >= 0 && j < n) ? t.r[(size_t)j] * s : 0.0f;
            z_[(size_t)i] = cfloat(l * win_[(size_t)i], r * win_[(size_t)i]);
        }
        plan_.run(z_.data(), false);
        float flux = 0;
        for (int k = 0; k < Bins; ++k)
        {
            const cfloat Zk = z_[(size_t)k], Zm = z_[(size_t)((N - k) % N)];
            const float ar = Zk.real(), ai = Zk.imag(), br = Zm.real(), bi = -Zm.imag();
            XL_[(size_t)k] = cfloat(0.5f * (ar + br), 0.5f * (ai + bi));
            XR_[(size_t)k] = cfloat(0.5f * (ai - bi), -0.5f * (ar - br));
            const float sr = XL_[(size_t)k].real() + XR_[(size_t)k].real(), si = XL_[(size_t)k].imag() + XR_[(size_t)k].imag();
            const float p2 = sr * sr + si * si;
            mag_[(size_t)k] = p2;
            phIn_[(size_t)k] = std::atan2(si, sr);
            const float lm = 0.5f * std::log1p(p2);
            if (lm > logPrev_[(size_t)k]) flux += lm - logPrev_[(size_t)k];
            logPrev_[(size_t)k] = lm;
        }
        const bool transient = flux > 2.0f * fluxMed_ + 1.0f && flux > lastFlux_ * 1.3f;
        fluxMed_ += 0.1f * (flux - fluxMed_);
        lastFlux_ = flux;
        const float hopA = (float)(a - prevA_);
        prevA_ = a;
        if (reset_ || transient || hopA <= 0.0f || hopA > 4.0f * N)
        {
            for (int k = 0; k < Bins; ++k) phOut_[(size_t)k] = phIn_[(size_t)k];
            reset_ = false;
        }
        else
        {
            int np = 0;
            for (int k = 2; k < Bins - 2; ++k)
                if (mag_[(size_t)k] > mag_[(size_t)k - 1] && mag_[(size_t)k] >= mag_[(size_t)k + 1] && mag_[(size_t)k] > mag_[(size_t)k - 2] && mag_[(size_t)k] >= mag_[(size_t)k + 2]) peaks_[(size_t)np++] = k;
            auto advance = [&](int k) {
                const float omega = 2.0f * 3.14159265f * k / N;
                float dev = phIn_[(size_t)k] - phPrev_[(size_t)k] - omega * hopA;
                dev -= 6.28318531f * std::floor((dev + 3.14159265f) / 6.28318531f);
                float ph = phOut_[(size_t)k] + (omega + dev / hopA) * (float)Hs;
                ph -= 6.28318531f * std::floor((ph + 3.14159265f) / 6.28318531f);
                phOut_[(size_t)k] = ph;
            };
            if (np == 0) for (int k = 0; k < Bins; ++k) advance(k);
            else
            {
                for (int i = 0; i < np; ++i) advance(peaks_[(size_t)i]);
                int pi = 0;
                for (int k = 0; k < Bins; ++k)
                {
                    while (pi + 1 < np && k > (peaks_[(size_t)pi] + peaks_[(size_t)pi + 1]) / 2) ++pi;
                    const int p = peaks_[(size_t)pi];
                    if (k != p) phOut_[(size_t)k] = phOut_[(size_t)p] + (phIn_[(size_t)k] - phIn_[(size_t)p]);
                }
            }
        }
        for (int k = 0; k < Bins; ++k) phPrev_[(size_t)k] = phIn_[(size_t)k];
        for (int k = 0; k < Bins; ++k)
        {
            const float rot = phOut_[(size_t)k] - phIn_[(size_t)k];
            const float c = std::cos(rot), sn = std::sin(rot);
            const cfloat& a1 = XL_[(size_t)k]; const cfloat& b1 = XR_[(size_t)k];
            const float lr = a1.real() * c - a1.imag() * sn, li = a1.real() * sn + a1.imag() * c;
            const float rr = b1.real() * c - b1.imag() * sn, ri = b1.real() * sn + b1.imag() * c;
            z_[(size_t)k] = cfloat(lr - ri, li + rr);
            if (k > 0 && k < N / 2) z_[(size_t)(N - k)] = cfloat(lr + ri, -li + rr);
        }
        plan_.run(z_.data(), true);
        for (int i = 0; i < N; ++i) { accL_[(size_t)i] += z_[(size_t)i].real() * win_[(size_t)i] * norm_; accR_[(size_t)i] += z_[(size_t)i].imag() * win_[(size_t)i] * norm_; }
        // emit the first Hs samples (now complete), shift the accumulator
        int w = (fifoRead_ + fifoCount_) & (kFifo - 1);
        for (int i = 0; i < Hs; ++i) { fifoL_[(size_t)w] = accL_[(size_t)i]; fifoR_[(size_t)w] = accR_[(size_t)i]; w = (w + 1) & (kFifo - 1); }
        fifoCount_ += Hs;
        std::copy(accL_.begin() + Hs, accL_.end(), accL_.begin()); std::fill(accL_.end() - Hs, accL_.end(), 0.0f);
        std::copy(accR_.begin() + Hs, accR_.end(), accR_.begin()); std::fill(accR_.end() - Hs, accR_.end(), 0.0f);
        apos_ += Hs * rate;
    }

    flip::FFTPlan plan_;
    std::vector<cfloat> z_;
    std::vector<float> win_, accL_, accR_, fifoL_, fifoR_;
    std::vector<float> phIn_, phPrev_, phOut_, mag_, logPrev_;
    std::vector<cfloat> XL_, XR_;
    std::vector<int> peaks_;
    float norm_ = 1.0f, fluxMed_ = 0.0f, lastFlux_ = 0.0f;
    double apos_ = 0;
    long prevA_ = 0;
    bool reset_ = true;
    int fifoCount_ = 0, fifoRead_ = 0;
};

} // namespace deck
