// analysis.h - song analysis for Melt Flip: tempo, beat/bar grid, musical key and hits.
//
// SPDX-License-Identifier: MIT
//
// Works on a mono copy decimated to ~11 kHz (enough for rhythm and pitch, 4x less work).
//  * Onset envelope: log-magnitude spectral flux (512-point frames, 128 hop ~ 11.6 ms).
//  * Tempo: autocorrelation of the onset envelope, weighted toward 70-160 BPM, refined.
//  * Beat phase: comb of the onset envelope at the tempo; bar start: the beat phase with the
//    most low-frequency (kick) energy.
//  * Key: chroma (pitch classes) from 4096-point spectra, correlated with major/minor key
//    profiles (Krumhansl-Kessler) in all 24 keys.
//  * Hits: peaks of the onset envelope above an adaptive threshold, refined to the sample.
// Runs on a background thread; allocates freely; never touches the audio thread.
#pragma once
#include <algorithm>
#include <cmath>
#include <array>
#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace flip {

using cfloat = std::complex<float>;

// Radix-2 FFT with precomputed bit-reversal and twiddles. Plain float arithmetic (std::complex
// multiplication is slow without fast-math because of its NaN/Inf special cases).
struct FFTPlan
{
    size_t n = 0;
    std::vector<uint32_t> rev;
    std::vector<float> cs, sn;   // twiddles for the forward transform
    explicit FFTPlan(size_t size) : n(size), rev(size), cs(size / 2), sn(size / 2)
    {
        int bits = 0; while ((size_t)1 << bits < n) ++bits;
        for (size_t i = 0; i < n; ++i) { uint32_t r = 0; for (int b = 0; b < bits; ++b) if (i & ((size_t)1 << b)) r |= 1u << (bits - 1 - b); rev[i] = r; }
        for (size_t k = 0; k < n / 2; ++k) { const double a = -2.0 * 3.14159265358979323846 * (double)k / (double)n; cs[k] = (float)std::cos(a); sn[k] = (float)std::sin(a); }
    }
    void run(cfloat* data, bool inverse) const
    {
        float* d = reinterpret_cast<float*>(data);   // std::complex<float> is two floats (guaranteed layout)
        for (size_t i = 0; i < n; ++i) if (i < rev[i]) { std::swap(d[2 * i], d[2 * rev[i]]); std::swap(d[2 * i + 1], d[2 * rev[i] + 1]); }
        const float sgn = inverse ? -1.0f : 1.0f;
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const size_t half = len / 2, step = n / len;
            for (size_t i = 0; i < n; i += len)
                for (size_t k = 0; k < half; ++k)
                {
                    const float wr = cs[k * step], wi = sgn * sn[k * step];
                    float* u = d + 2 * (i + k); float* v = d + 2 * (i + k + half);
                    const float tr = v[0] * wr - v[1] * wi, ti = v[0] * wi + v[1] * wr;
                    v[0] = u[0] - tr; v[1] = u[1] - ti;
                    u[0] += tr; u[1] += ti;
                }
        }
        if (inverse) { const float s = 1.0f / (float)n; for (size_t i = 0; i < 2 * n; ++i) d[i] *= s; }
    }
};

// Convenience wrapper (plans are cached per size, per thread).
inline void fft(std::vector<cfloat>& a, bool inverse)
{
    thread_local std::vector<std::unique_ptr<FFTPlan>> plans;
    for (auto& p : plans) if (p->n == a.size()) { p->run(a.data(), inverse); return; }
    plans.push_back(std::make_unique<FFTPlan>(a.size()));
    plans.back()->run(a.data(), inverse);
}

struct Hit { double pos; float strength; };   // pos in samples of the original song

struct Analysis
{
    double bpm = 0.0;           // detected tempo
    double beatPhase = 0.0;     // position (samples) of a beat
    double barPhase = 0.0;      // position (samples) of a bar start (beat 1)
    int keyIndex = -1;          // 0..11 = C..B major, 12..23 = C..B minor
    float keyConfidence = 0.0f;
    std::vector<Hit> hits;      // in time order
    std::string keyName() const
    {
        static const char* n[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
        if (keyIndex < 0) return "?";
        return std::string(n[keyIndex % 12]) + (keyIndex < 12 ? " maj" : " min");
    }
};

inline Analysis analyse(const float* mono, size_t n, double sr)
{
    Analysis A;
    if (n < (size_t)(sr * 2.0)) return A;   // under 2 seconds: nothing reliable to find

    // ---- decimate to ~11 kHz (two-pole low-pass, then pick every d-th sample) ----
    const int d = std::max(1, (int)std::lround(sr / 11025.0));
    const double asr = sr / d;
    std::vector<float> x;
    x.reserve(n / (size_t)d + 1);
    {
        const double fc = 0.45 * asr / 2.0, w = 2.0 * 3.14159265358979 * fc / sr, c = std::cos(w), al = std::sin(w) / (2 * 0.7071);
        const double b0 = (1 - c) / 2 / (1 + al), b1 = (1 - c) / (1 + al), a1 = -2 * c / (1 + al), a2 = (1 - al) / (1 + al);
        double z1 = 0, z2 = 0, z1b = 0, z2b = 0;
        for (size_t i = 0; i < n; ++i)
        {
            double y = b0 * mono[i] + z1; z1 = b1 * mono[i] - a1 * y + z2; z2 = b0 * mono[i] - a2 * y;
            double y2 = b0 * y + z1b; z1b = b1 * y - a1 * y2 + z2b; z2b = b0 * y - a2 * y2;
            if (i % (size_t)d == 0) x.push_back((float)y2);
        }
    }

    // ---- onset envelope (spectral flux) + low-band flux for downbeats ----
    const int N = 512, H = 128;
    const int frames = (int)((x.size() - N) / H);
    std::vector<float> win(N), onset((size_t)frames, 0.0f), lowOn((size_t)frames, 0.0f), prev(N / 2 + 1, 0.0f);
    for (int i = 0; i < N; ++i) win[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * i / N);
    const int lowBin = std::max(1, (int)(150.0 * N / asr));
    std::vector<cfloat> buf(N);
    for (int f = 0; f < frames; ++f)
    {
        for (int i = 0; i < N; ++i) buf[i] = cfloat(x[(size_t)(f * H + i)] * win[i], 0.0f);
        fft(buf, false);
        float flux = 0, low = 0;
        for (int k = 1; k <= N / 2; ++k)
        {
            const float m = std::log1p(100.0f * std::abs(buf[k]));
            const float dlt = m - prev[k];
            if (dlt > 0) { flux += dlt; if (k <= lowBin) low += dlt; }
            prev[k] = m;
        }
        onset[(size_t)f] = flux; lowOn[(size_t)f] = low;
    }
    const double frameRate = asr / H;

    // ---- tempo: autocorrelation of the (mean-removed) onset envelope ----
    double beatFrames = 0, beatPhFrames = 0;
    {
        std::vector<float> o = onset;
        float mean = 0; for (float v : o) mean += v; mean /= (float)o.size();
        for (float& v : o) v = std::max(0.0f, v - mean);
        const int minLag = (int)(frameRate * 60.0 / 200.0), maxLag = (int)(frameRate * 60.0 / 55.0);
        std::vector<double> ac((size_t)maxLag + 2, 0.0);
        for (int lag = minLag; lag <= maxLag + 1; ++lag)
        {
            double s = 0; for (size_t i = (size_t)lag; i < o.size(); ++i) s += (double)o[i] * o[i - (size_t)lag];
            ac[(size_t)lag] = s / (double)(o.size() - (size_t)lag);
        }
        // Score each candidate beat period by how well the music lines up at every metrical level
        // around it (16ths, 8ths, the beat, half bar, bar). A wrong period (e.g. 3/4 of the beat,
        // from dotted-eighth delays) finds support at few levels; the true beat finds it at all.
        auto acAt = [&](double l) -> double {
            if (l < 1 || l > (double)maxLag + 1) {
                if (l > (double)maxLag + 1 && l < (double)o.size() / 2) {   // longer lags: compute on demand
                    const size_t L = (size_t)std::lround(l); double s2 = 0;
                    for (size_t i = L; i < o.size(); ++i) s2 += (double)o[i] * o[i - L];
                    return s2 / (double)(o.size() - L);
                }
                if (l < 1) return 0.0;
            }
            const size_t k = (size_t)l; const double f = l - (double)k;
            if (k + 1 >= ac.size()) return ac[k];
            return ac[k] * (1 - f) + ac[k + 1] * f;
        };
        // short lags (16ths, 8ths) are below minLag: fill them in too
        for (int lag = 1; lag < minLag; ++lag)
        {
            double s2 = 0; for (size_t i = (size_t)lag; i < o.size(); ++i) s2 += (double)o[i] * o[i - (size_t)lag];
            ac[(size_t)lag] = s2 / (double)(o.size() - (size_t)lag);
        }
        int best = minLag; double bestScore = -1;
        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            const double l = lag;
            double s = acAt(l) + 0.5 * acAt(l / 2) + 0.35 * acAt(l / 4) + 0.6 * acAt(l * 2) + 0.5 * acAt(l * 4);
            const double bpm = 60.0 * frameRate / lag;
            const double w = std::exp(-0.5 * std::pow(std::log2(bpm / 100.0) / 0.75, 2));   // preference for ~70-150 BPM
            s *= w;
            if (s > bestScore) { bestScore = s; best = lag; }
        }
        double lag = best;
        if (best > minLag && best < maxLag)
        {
            const double a = ac[(size_t)best - 1], b = ac[(size_t)best], c = ac[(size_t)best + 1], den = a - 2 * b + c;
            if (std::fabs(den) > 1e-12) lag += 0.5 * (a - c) / den;
        }
        // fine search: the comb whose teeth hit the most onset energy over the whole song
        double bestFine = lag, bestFineScore = -1;
        for (double l = lag * 0.985; l <= lag * 1.015; l += lag * 0.0005)
        {
            double sMax = 0;
            for (int ph = 0; ph < (int)l; ph += 1)
            {
                double s = 0; for (double t = ph; t < (double)o.size(); t += l) s += o[(size_t)t];
                sMax = std::max(sMax, s);
            }
            if (sMax > bestFineScore) { bestFineScore = sMax; bestFine = l; }
        }
        A.bpm = 60.0 * frameRate / bestFine;
        if (A.bpm < 70.0) A.bpm *= 2.0;
        if (A.bpm > 170.0) A.bpm /= 2.0;
        A.bpm = std::round(A.bpm * 10.0) / 10.0;
        const double P = 60.0 * frameRate / A.bpm;   // frames per beat
        // beat phase
        double bestPh = 0, bestPhS = -1;
        for (double ph = 0; ph < P; ph += 0.25)
        {
            double s = 0; for (double t = ph; t < (double)o.size(); t += P) s += o[(size_t)t];
            if (s > bestPhS) { bestPhS = s; bestPh = ph; }
        }
        beatFrames = P; beatPhFrames = bestPh;
        const double toSamples = (double)H * d;
        const double lead = N * 0.5 * d;   // a frame is time-stamped at its centre
        A.beatPhase = bestPh * toSamples + lead;
    }

    std::vector<std::array<double, 12>> chromaFrames;   // per long frame, also used for bar detection (chord changes)
    // ---- key: chroma from long frames ----
    {
        const int K = 4096, KH = 2048;
        std::vector<float> kw(K);
        for (int i = 0; i < K; ++i) kw[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * i / K);
        std::vector<cfloat> kb(K);
        std::vector<float> mag((size_t)K / 2, 0.0f);
        double chroma[12] = {0};
        for (size_t s = 0; s + K < x.size(); s += KH)
        {
            for (int i = 0; i < K; ++i) kb[i] = cfloat(x[s + (size_t)i] * kw[i], 0.0f);
            fft(kb, false);
            // note salience: a note is strong when its fundamental AND its overtones are present;
            // an overtone on its own (e.g. the 5th harmonic of A = C#) scores low.
            double frameC[12] = {0}, tot = 0;
            for (int k = 0; k < K / 2; ++k) mag[(size_t)k] = std::log1p(10.0f * std::abs(kb[k]));
            for (int m = 33; m <= 84; ++m)   // A1 .. C6
            {
                const double f0 = 440.0 * std::pow(2.0, (m - 69) / 12.0);
                double sal = 0;
                for (int h = 1; h <= 5; ++h)
                {
                    const double bin = h * f0 * K / asr;
                    if (bin >= K / 2 - 2) break;
                    const size_t b = (size_t)std::lround(bin);
                    const float pk = std::max(mag[b], std::max(mag[b - 1], mag[b + 1]));
                    sal += pk / h;
                }
                const double fund = mag[(size_t)std::lround(f0 * K / asr)];
                sal *= std::min(1.0, fund / (0.25 + 1e-9 + fund));   // must have some energy at the fundamental itself
                frameC[m % 12] += sal; tot += sal;
            }
            std::array<double, 12> cf{};
            if (tot > 0) for (int c = 0; c < 12; ++c) { chroma[c] += frameC[c] / tot; cf[(size_t)c] = frameC[c] / tot; }
            chromaFrames.push_back(cf);
        }
        static const double maj[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
        static const double mnr[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
        auto corr = [&](const double* prof, int root)
        {
            double mx = 0, mp = 0; for (int i = 0; i < 12; ++i) { mx += chroma[i]; mp += prof[i]; } mx /= 12; mp /= 12;
            double sxy = 0, sxx = 0, syy = 0;
            for (int i = 0; i < 12; ++i) { const double a = chroma[(i + root) % 12] - mx, b = prof[i] - mp; sxy += a * b; sxx += a * a; syy += b * b; }
            return sxy / std::sqrt(sxx * syy + 1e-12);
        };
        double best = -2, second = -2;
        for (int k = 0; k < 24; ++k)
        {
            const double c = corr(k < 12 ? maj : mnr, k % 12);
            if (c > best) { second = best; best = c; A.keyIndex = k; }
            else if (c > second) second = c;
        }
        A.keyConfidence = (float)std::max(0.0, best - second);
    }

    // ---- bar start: which of the 4 beat positions starts the bar ----
    // Evidence: kick (low-band onsets) on the 1, and chord changes (chroma novelty) on the 1.
    {
        const double toSec = (double)H * d / sr, leadSec = N * 0.5 * d / sr;
        const double beatSec = beatFrames * toSec;
        const double chromaHopSec = 2048.0 / asr, chromaLeadSec = 2048.0 / asr;   // 4096 frame centre
        auto chromaMean = [&](double t0, double t1) {
            std::array<double, 12> m{}; int cnt = 0;
            for (size_t j = 0; j < chromaFrames.size(); ++j)
            {
                const double c = j * chromaHopSec + chromaLeadSec;
                if (c >= t0 && c < t1) { for (int k = 0; k < 12; ++k) m[(size_t)k] += chromaFrames[j][(size_t)k]; ++cnt; }
            }
            return m;
        };
        auto novelty = [&](double t) {
            const auto a = chromaMean(t - 2 * beatSec, t), b = chromaMean(t, t + 2 * beatSec);
            double ab = 0, aa = 0, bb = 0; for (int k = 0; k < 12; ++k) { ab += a[(size_t)k] * b[(size_t)k]; aa += a[(size_t)k] * a[(size_t)k]; bb += b[(size_t)k] * b[(size_t)k]; }
            return (aa > 0 && bb > 0) ? 1.0 - ab / std::sqrt(aa * bb) : 0.0;
        };
        double kick[4] = {0}, nov[4] = {0};
        const double songSec = (double)n / sr;
        for (int b = 0; b < 4; ++b)
        {
            const double ph = beatPhFrames + b * beatFrames;
            for (double t = ph; t < (double)lowOn.size(); t += 4 * beatFrames) { const size_t k = (size_t)t; kick[b] += lowOn[k] + (k + 1 < lowOn.size() ? lowOn[k + 1] : 0); }
            for (double t = ph * toSec + leadSec; t < songSec - 2 * beatSec; t += 4 * beatSec) if (t > 2 * beatSec) nov[b] += novelty(t);
        }
        const double kmax = std::max(std::max(kick[0], kick[1]), std::max(kick[2], kick[3])) + 1e-9;
        const double nmax = std::max(std::max(nov[0], nov[1]), std::max(nov[2], nov[3])) + 1e-9;
        int best = 0; double bestS = -1;
        for (int b = 0; b < 4; ++b) { const double sc = kick[b] / kmax + 1.2 * nov[b] / nmax; if (sc > bestS) { bestS = sc; best = b; } }
        const double barSec = 4 * beatSec;
        A.barPhase = std::fmod((beatPhFrames + best * beatFrames) * toSec + leadSec, barSec) * sr;
    }

    // ---- hits: onset peaks above an adaptive threshold ----
    {
        const int w = 8;
        for (int f = w; f + w < frames; ++f)
        {
            const float v = onset[(size_t)f];
            bool peak = true;
            for (int j = -3; j <= 3 && peak; ++j) if (j && onset[(size_t)(f + j)] > v) peak = false;
            if (!peak) continue;
            float mean = 0; for (int j = -w; j <= w; ++j) mean += onset[(size_t)(f + j)]; mean /= (float)(2 * w + 1);
            if (v < mean * 1.5f + 0.5f) continue;
            // refine to the sample: biggest rise in short-term energy around the frame (original rate)
            const double centre = ((double)f * H + N * 0.5) * d;
            const double from = std::max(0.0, centre - 0.03 * sr), to = std::min((double)n - 64, centre + 0.02 * sr);
            double bestPos = centre, bestRise = -1, prevE = 0;
            const int seg = std::max(16, (int)(0.002 * sr));
            for (double p = from; p < to; p += seg / 2)
            {
                double e = 0; for (int i = 0; i < seg && (size_t)p + (size_t)i < n; ++i) { const float s = mono[(size_t)p + (size_t)i]; e += s * s; }
                if (p > from && e - prevE > bestRise) { bestRise = e - prevE; bestPos = p; }
                prevE = e;
            }
            A.hits.push_back({bestPos, v - mean});
        }
    }
    return A;
}

} // namespace flip
