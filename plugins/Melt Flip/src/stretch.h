// stretch.h - Melt Flip's time-stretch / pitch-shift renderer (offline, background thread).
//
// SPDX-License-Identifier: MIT
//
// render(): change the length (tempo) and the pitch (key) of a whole song independently.
//   1. Phase vocoder stretches by timeRatio * pitchRatio:
//        - 2048-point Hann frames, synthesis hop 512 (75 % overlap)
//        - L and R share one complex FFT (L + iR) both ways: half the work
//        - phases advanced from the L+R sum and applied to both channels with their own phase
//          offsets, so the stereo image does not smear
//        - identity phase locking (Laroche & Dolson): bins around each spectral peak keep their
//          phase relationship to the peak, which avoids the "phasey/watery" sound
//        - phase reset on strong transients, so drums stay sharp
//   2. A windowed-sinc resampler (16 taps, anti-aliased) reads that at pitchRatio, which
//      restores the length and moves the pitch.
// Result: output length = input * timeRatio, pitch x pitchRatio.
#pragma once
#include "analysis.h"
#include "decode.h"
#include <atomic>
#include <functional>

namespace flip {

namespace detail {

inline float wrapPi(float a) { a = std::fmod(a + 3.14159265f, 6.28318531f); if (a < 0) a += 6.28318531f; return a - 3.14159265f; }

// Kaiser-windowed sinc resampler: read 'in' at positions advancing by 'step'; cutoff below Nyquist when step > 1.
inline void resample(const std::vector<float>& in, std::vector<float>& out, double step, size_t outLen)
{
    constexpr int kTaps = 16, kPhases = 512;
    std::vector<float> table;   // built per call (cheap) for the given cutoff
    const double cutoff = step > 1.0 ? 0.97 / step : 0.97;
    table.assign((size_t)(kPhases + 1) * kTaps, 0.0f);
    auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 30; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
    const double beta = 7.0;
    for (int p = 0; p <= kPhases; ++p)
    {
        const double frac = (double)p / kPhases;
        double sum = 0;
        for (int t = 0; t < kTaps; ++t)
        {
            const double x = (double)(t - kTaps / 2 + 1) - frac;
            const double sinc = x == 0 ? 1.0 : std::sin(3.14159265358979 * x * cutoff) / (3.14159265358979 * x * cutoff);
            const double r = x / (kTaps / 2.0);
            const double w = std::fabs(r) >= 1 ? 0.0 : i0(beta * std::sqrt(1 - r * r)) / i0(beta);
            table[(size_t)p * kTaps + (size_t)t] = (float)(sinc * w);
            sum += sinc * w;
        }
        for (int t = 0; t < kTaps; ++t) table[(size_t)p * kTaps + (size_t)t] /= (float)sum;   // unity gain
    }
    out.assign(outLen, 0.0f);
    const long n = (long)in.size();
    for (size_t i = 0; i < outLen; ++i)
    {
        const double pos = (double)i * step;
        const long k = (long)pos;
        const int ph = (int)((pos - (double)k) * kPhases);
        const float* c = &table[(size_t)ph * kTaps];
        float acc = 0;
        for (int t = 0; t < kTaps; ++t)
        {
            const long j = k + t - kTaps / 2 + 1;
            if (j >= 0 && j < n) acc += c[t] * in[(size_t)j];
        }
        out[i] = acc;
    }
}

} // namespace detail

// cancel: set to true to abandon (returns false). progress: called with 0..1 now and then.
inline bool render(const Song& in, double timeRatio, double pitchRatio, Song& out,
                   const std::atomic<bool>* cancel = nullptr, const std::function<void(float)>& progress = nullptr)
{
    out.sampleRate = in.sampleRate;
    out.truncated = in.truncated;
    const size_t n = in.l.size();
    if (std::fabs(timeRatio - 1.0) < 1e-6 && std::fabs(pitchRatio - 1.0) < 1e-6) { out.l = in.l; out.r = in.r; return true; }

    const double S = timeRatio * pitchRatio;           // phase-vocoder stretch
    const int N = 2048, Hs = 512, bins = N / 2 + 1;
    const double Ha = Hs / S;
    const size_t midLen = (size_t)((double)n * S) + N;
    std::vector<float> midL(midLen + N, 0.0f), midR(midLen + N, 0.0f), norm(midLen + N, 0.0f);
    std::vector<float> win(N);
    for (int i = 0; i < N; ++i) win[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * i / N);

    std::vector<cfloat> z(N);
    std::vector<float> magS(bins), phIn(bins), phPrevIn(bins, 0.0f), phOut(bins, 0.0f), logMagPrev(bins, 0.0f);
    std::vector<cfloat> XLs((size_t)bins), XRs((size_t)bins);
    std::vector<int> peaks; peaks.reserve(bins);
    std::vector<float> fluxHist;
    long prevA = 0;
    const size_t frames = (size_t)((double)n / Ha) + 2;
    for (size_t m = 0; m < frames; ++m)
    {
        if (cancel && cancel->load()) return false;
        if (progress && (m & 255) == 0) progress(0.9f * (float)m / (float)frames);
        const long a = (long)std::lround((double)m * Ha) - N / 2;   // analysis frame start (centred)
        const long s = (long)m * Hs;                               // synthesis frame start
        if (s + N > (long)midL.size()) break;
        for (int i = 0; i < N; ++i)
        {
            const long j = a + i;
            const float l = (j >= 0 && j < (long)n) ? in.l[(size_t)j] : 0.0f, r = (j >= 0 && j < (long)n) ? in.r[(size_t)j] : 0.0f;
            z[i] = cfloat(l * win[i], r * win[i]);
        }
        fft(z, false);
        float flux = 0;
        for (int k = 0; k < bins; ++k)
        {
            // split the packed transform: XL = (Z[k] + conj Z[N-k]) / 2, XR = (Z[k] - conj Z[N-k]) / 2i
            const cfloat Zk = z[(size_t)k], Zm = z[(size_t)((N - k) % N)];
            const float ar = Zk.real(), ai = Zk.imag(), br = Zm.real(), bi = -Zm.imag();
            const cfloat XL(0.5f * (ar + br), 0.5f * (ai + bi)), XR(0.5f * (ai - bi), -0.5f * (ar - br));
            XLs[(size_t)k] = XL; XRs[(size_t)k] = XR;
            const float sr_ = XL.real() + XR.real(), si = XL.imag() + XR.imag();
            const float p2 = sr_ * sr_ + si * si;
            magS[k] = p2;                                   // power is enough to find peaks
            phIn[k] = std::atan2(si, sr_);
            const float lm = 0.5f * std::log1p(p2);        // ~ log magnitude, for the transient detector
            if (lm > logMagPrev[k]) flux += lm - logMagPrev[k];
            logMagPrev[k] = lm;
        }
        // transient? compare with the recent typical flux
        fluxHist.push_back(flux);
        float med = 0;
        {
            const size_t h0 = fluxHist.size() > 16 ? fluxHist.size() - 16 : 0;
            std::vector<float> w(fluxHist.begin() + (long)h0, fluxHist.end());
            std::nth_element(w.begin(), w.begin() + (long)w.size() / 2, w.end());
            med = w[w.size() / 2];
        }
        const bool reset = m == 0 || (flux > 2.0f * med + 1.0f && fluxHist.size() > 2 && flux > fluxHist[fluxHist.size() - 2] * 1.3f);
        const float hopA = (float)(a + N / 2 - prevA);   // actual analysis hop (integer)
        prevA = a + N / 2;
        if (reset)
        {
            for (int k = 0; k < bins; ++k) phOut[k] = phIn[k];
        }
        else
        {
            // peaks of the magnitude spectrum
            peaks.clear();
            for (int k = 2; k < bins - 2; ++k)
                if (magS[k] > magS[k - 1] && magS[k] >= magS[k + 1] && magS[k] > magS[k - 2] && magS[k] >= magS[k + 2]) peaks.push_back(k);
            // advance each peak by its true (instantaneous) frequency
            for (int p : peaks)
            {
                const float omega = 2.0f * 3.14159265f * p / N;
                const float dev = detail::wrapPi(phIn[p] - phPrevIn[p] - omega * hopA);
                phOut[p] = detail::wrapPi(phOut[p] + (omega + dev / std::max(1.0f, hopA)) * (float)Hs);
            }
            // lock every other bin to its nearest peak (region boundary = midpoint between peaks)
            if (peaks.empty())
                for (int k = 0; k < bins; ++k)
                {
                    const float omega = 2.0f * 3.14159265f * k / N;
                    const float dev = detail::wrapPi(phIn[k] - phPrevIn[k] - omega * hopA);
                    phOut[k] = detail::wrapPi(phOut[k] + (omega + dev / std::max(1.0f, hopA)) * (float)Hs);
                }
            else
            {
                size_t pi = 0;
                for (int k = 0; k < bins; ++k)
                {
                    while (pi + 1 < peaks.size() && k > (peaks[pi] + peaks[pi + 1]) / 2) ++pi;
                    const int p = peaks[pi];
                    if (k != p) phOut[k] = phOut[p] + (phIn[k] - phIn[p]);
                }
            }
        }
        for (int k = 0; k < bins; ++k) phPrevIn[k] = phIn[k];
        // rebuild each channel with the shared phase motion: rotate both by the same angle
        // (one sin/cos per bin), which keeps each channel's own phase offset to the sum
        for (int k = 0; k < bins; ++k)
        {
            const float rot = phOut[k] - phIn[k];
            const float c = std::cos(rot), sn = std::sin(rot);
            const float lr = XLs[(size_t)k].real() * c - XLs[(size_t)k].imag() * sn, li = XLs[(size_t)k].real() * sn + XLs[(size_t)k].imag() * c;
            const float rr = XRs[(size_t)k].real() * c - XRs[(size_t)k].imag() * sn, ri = XRs[(size_t)k].real() * sn + XRs[(size_t)k].imag() * c;
            // pack back: Z[k] = YL + i*YR, Z[N-k] = conj(YL) + i*conj(YR)
            z[(size_t)k] = cfloat(lr - ri, li + rr);
            if (k > 0 && k < N / 2) z[(size_t)(N - k)] = cfloat(lr + ri, -li + rr);
        }
        fft(z, true);
        for (int i = 0; i < N; ++i)
        {
            const size_t j = (size_t)(s + i);
            midL[j] += z[(size_t)i].real() * win[i];
            midR[j] += z[(size_t)i].imag() * win[i];
            norm[j] += win[i] * win[i];
        }
    }
    for (size_t j = 0; j < midL.size(); ++j) if (norm[j] > 1e-3f) { midL[j] /= norm[j]; midR[j] /= norm[j]; }
    // the vocoder output is centred: frame m (input centre m*Ha) sits at m*Hs + N/2
    const size_t shift = N / 2;
    midL.erase(midL.begin(), midL.begin() + (long)shift); midR.erase(midR.begin(), midR.begin() + (long)shift);

    if (cancel && cancel->load()) return false;
    if (progress) progress(0.92f);
    const size_t outLen = (size_t)((double)n * timeRatio);
    if (std::fabs(pitchRatio - 1.0) < 1e-6)
    {
        midL.resize(outLen); midR.resize(outLen);
        out.l = std::move(midL); out.r = std::move(midR);
    }
    else
    {
        detail::resample(midL, out.l, pitchRatio, outLen);
        if (cancel && cancel->load()) return false;
        detail::resample(midR, out.r, pitchRatio, outLen);
    }
    if (progress) progress(1.0f);
    return true;
}

} // namespace flip
