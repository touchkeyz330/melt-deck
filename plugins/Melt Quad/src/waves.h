// waves.h - Wave Relay's single-cycle wave library: 32 built-in waves made from maths (no
// samples), optional user waves from Content/Waves, and band-limited mip-mapped tables so high
// notes do not alias.
//
// SPDX-License-Identifier: MIT
//
// Every wave is stored as kLevels tables of kTable points; level L keeps harmonics 1..(1024 >> L).
// Pick the level from the playback increment (see levelFor). Tables are built once per process
// (shared by all instances) with a small radix-2 FFT. Construction/prepare only: allocates.

#ifndef WAVERELAY_WAVES_H
#define WAVERELAY_WAVES_H

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace wr {

constexpr int kTable = 2048;
constexpr int kLevels = 11;

struct Wave
{
    std::string name;
    std::vector<float> mips;   // kLevels * (kTable + 1): one guard point per level for interpolation
    const float* level(int l) const { return mips.data() + (size_t)l * (kTable + 1); }
};

// Mip level for a phase increment in cycles per sample.
inline int levelFor(float inc)
{
    const float maxHarm = inc > 1.0e-9f ? 0.5f / inc : 1.0e9f;
    int l = 0;
    while (l < kLevels - 1 && (float)(1024 >> l) > maxHarm) ++l;
    return l;
}

namespace detail {

inline void fft(std::vector<std::complex<double>>& a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = 2.0 * 3.14159265358979323846 / (double)len * (inverse ? 1.0 : -1.0);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse) for (auto& x : a) x /= (double)n;
}

} // namespace detail

// Build a band-limited, level-matched wave from one cycle of kTable points.
inline Wave makeWave(const std::string& name, const std::vector<double>& cycle)
{
    using cd = std::complex<double>;
    std::vector<cd> spec(kTable);
    for (int i = 0; i < kTable; ++i) spec[(size_t)i] = cd(i < (int)cycle.size() ? cycle[(size_t)i] : 0.0, 0.0);
    detail::fft(spec, false);
    spec[0] = 0.0;   // no DC

    Wave w;
    w.name = name;
    w.mips.assign((size_t)kLevels * (kTable + 1), 0.0f);
    double gain = 1.0;
    for (int l = 0; l < kLevels; ++l)
    {
        const int maxH = 1024 >> l;
        std::vector<cd> s = spec;
        for (int k = maxH + 1; k < kTable - maxH; ++k) s[(size_t)k] = 0.0;
        if (maxH >= kTable / 2) s[kTable / 2] = 0.0;
        detail::fft(s, true);
        if (l == 0)
        {
            // even loudness across waves (RMS ~ -9 dBFS), never above full scale
            double rms = 0.0, peak = 0.0;
            for (const auto& x : s) { rms += x.real() * x.real(); peak = std::max(peak, std::fabs(x.real())); }
            rms = std::sqrt(rms / kTable);
            gain = rms > 1.0e-9 ? 0.35 / rms : 1.0;
            if (peak * gain > 0.98) gain = 0.98 / peak;
        }
        float* t = w.mips.data() + (size_t)l * (kTable + 1);
        for (int i = 0; i < kTable; ++i) t[i] = (float)(s[(size_t)i].real() * gain);
        t[kTable] = t[0];
    }
    return w;
}

// The built-in waves: all original, generated from formulas.
inline std::vector<Wave> builtinWaves()
{
    const double PI = 3.14159265358979323846;
    auto shape = [&](auto f) { std::vector<double> c(kTable); for (int i = 0; i < kTable; ++i) c[(size_t)i] = f((double)i / kTable); return c; };
    auto additive = [&](auto amp) {   // amp(h) for harmonics 1..256, sine phase
        std::vector<double> c(kTable, 0.0);
        for (int h = 1; h <= 256; ++h) { const double a = amp(h); if (a == 0.0) continue; for (int i = 0; i < kTable; ++i) c[(size_t)i] += a * std::sin(2 * PI * h * i / kTable); }
        return c; };
    auto formant = [&](double f1, double f2, double bw) {   // harmonic peaks at h ~ f1, f2
        return additive([=](int h) { const double x = h; return (1.0 / x) * (1.0 + 6.0 * std::exp(-(x - f1) * (x - f1) / (2 * bw * bw)) + 4.0 * std::exp(-(x - f2) * (x - f2) / (2 * bw * bw))); }); };
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (double)(seed >> 8) / 16777216.0 * 2.0 - 1.0; };

    std::vector<std::pair<std::string, std::vector<double>>> list;
    list.push_back({"Sine", shape([&](double t) { return std::sin(2 * PI * t); })});
    list.push_back({"Triangle", shape([&](double t) { return t < 0.25 ? 4 * t : t < 0.75 ? 2 - 4 * t : 4 * t - 4; })});
    list.push_back({"Saw", shape([&](double t) { return 1.0 - 2.0 * t; })});
    list.push_back({"Square", shape([&](double t) { return t < 0.5 ? 1.0 : -1.0; })});
    list.push_back({"Pulse 25", shape([&](double t) { return t < 0.25 ? 1.0 : -1.0; })});
    list.push_back({"Pulse 10", shape([&](double t) { return t < 0.10 ? 1.0 : -1.0; })});
    list.push_back({"Organ", additive([](int h) { return h == 1 ? 1.0 : h == 2 ? 0.8 : h == 3 ? 0.6 : h == 4 ? 0.5 : h == 6 ? 0.3 : h == 8 ? 0.25 : 0.0; })});
    list.push_back({"Soft Organ", additive([](int h) { return h == 1 ? 1.0 : h == 2 ? 0.35 : h == 4 ? 0.12 : 0.0; })});
    list.push_back({"Hollow", additive([](int h) { return (h % 2) ? 1.0 / ((double)h * h) * 3.0 : 0.0; })});
    list.push_back({"Bright Saw", additive([](int h) { return (1.0 / h) * (h >= 5 && h <= 14 ? 2.2 : 1.0); })});
    list.push_back({"Vox Ah", formant(3.0, 5.0, 1.0)});
    list.push_back({"Vox Ee", formant(1.0, 9.0, 1.2)});
    list.push_back({"Vox Oo", formant(1.5, 3.0, 0.7)});
    list.push_back({"Glass", additive([](int h) { return h == 1 ? 1.0 : h == 4 ? 0.6 : h == 9 ? 0.45 : h == 16 ? 0.3 : h == 25 ? 0.2 : 0.0; })});
    list.push_back({"Bell Tone", additive([](int h) { return h == 1 ? 1.0 : h == 3 ? 0.5 : h == 7 ? 0.45 : h == 11 ? 0.3 : h == 17 ? 0.2 : 0.0; })});
    list.push_back({"Choir", additive([](int h) { return std::exp(-0.35 * h) * (1.0 + 0.8 * std::exp(-(h - 4.0) * (h - 4.0) / 2.0)); })});
    list.push_back({"FM Bright", shape([&](double t) { return std::sin(2 * PI * t + 2.5 * std::sin(2 * PI * 2 * t)); })});
    list.push_back({"FM Hollow", shape([&](double t) { return std::sin(2 * PI * t + 1.8 * std::sin(2 * PI * 3 * t)); })});
    list.push_back({"FM Metal", shape([&](double t) { return std::sin(2 * PI * t + 4.0 * std::sin(2 * PI * 7 * t)); })});
    list.push_back({"Sync 2.5", shape([&](double t) { return 1.0 - 2.0 * std::fmod(t * 2.5, 1.0); })});
    list.push_back({"Sync 4.3", shape([&](double t) { return std::sin(2 * PI * std::fmod(t * 4.3, 1.0)) * (1.0 - t); })});
    list.push_back({"Fold", shape([&](double t) { return std::sin(3.0 * std::sin(2 * PI * t)); })});
    list.push_back({"Digital", shape([&](double t) { return std::floor((1.0 - 2.0 * t) * 4.0) / 4.0; })});
    list.push_back({"Clip Sine", shape([&](double t) { return std::max(-0.6, std::min(0.6, std::sin(2 * PI * t))) / 0.6; })});
    list.push_back({"Half Sine", shape([&](double t) { return t < 0.5 ? std::sin(2 * PI * t) * 2.0 - 0.6 : -0.6; })});
    list.push_back({"Ramp Sq", shape([&](double t) { const double r = 1.0 - 2.0 * t; return r * std::fabs(r); })});
    list.push_back({"Comb Saw", additive([](int h) { return (1.0 / h) * (0.5 + 0.5 * std::cos(h * 0.9)); })});
    list.push_back({"Odd Saw", additive([](int h) { return (h % 2) ? 1.0 / h : 0.15 / h; })});
    list.push_back({"Stairs", shape([&](double t) { return t < 0.25 ? 1.0 : t < 0.5 ? 0.33 : t < 0.75 ? -0.33 : -1.0; })});
    list.push_back({"Buzz", ([&]() { std::vector<double> c(kTable); for (auto& x : c) x = rnd(); return c; })()});
    list.push_back({"Pluck", shape([&](double t) { return std::exp(-6.0 * t) * std::sin(2 * PI * t * 3.0) + (1.0 - 2.0 * t) * 0.3; })});
    list.push_back({"Metal", additive([&](int h) { return h >= 6 && h <= 40 && (h * 7919) % 5 == 0 ? 0.6 / std::sqrt((double)h) : (h == 1 ? 0.5 : 0.0); })});

    std::vector<Wave> out;
    for (auto& p : list) out.push_back(makeWave(p.first, p.second));
    return out;
}

// Resample any number of points to one kTable cycle (linear interpolation).
inline std::vector<double> toCycle(const float* x, size_t n)
{
    std::vector<double> c(kTable);
    for (int i = 0; i < kTable; ++i)
    {
        const double p = (double)i * (double)n / kTable;
        const size_t k = (size_t)p;
        const double f = p - (double)k;
        c[(size_t)i] = (double)x[k % n] * (1.0 - f) + (double)x[(k + 1) % n] * f;
    }
    return c;
}

} // namespace wr

#endif // WAVERELAY_WAVES_H
