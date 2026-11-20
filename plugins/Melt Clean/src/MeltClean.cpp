// MeltClean.cpp - one-fader adaptive noise suppressor for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// Original design. Push one fader up and the steady background noise (hiss, hum, room tone,
// fan, vinyl/tape bed, mic noise) goes down while the voice or instrument stays put. Nothing
// to set up: the plugin learns the noise floor by itself, continuously, from the quietest
// moments of each frequency band (minimum statistics over about 3 seconds), so it follows the
// noise if it changes. A tone guard leaves steady, held notes alone (noise flickers, notes don't),
// so sustained pads, bass and chords are never mistaken for noise.
//
// How: 75 %-overlap STFT (1024 points at 44.1/48 kHz, 2048 at 88.2/96 kHz, sqrt-Hann in and out).
// Per bin: smoothed power vs. tracked noise floor -> spectral-subtraction (Wiener-style) gain with
// a floor, smoothed in time (fast open, slower close) and across neighbouring bins so it does
// not "twinkle" (musical noise). Both channels share one gain, so the stereo image never moves.
// The fader sets over-subtraction and the depth of the floor together: 0 = exact bypass
// (delayed to stay in sync), 100 % = up to 30 dB of noise removed. Latency is reported to the
// host so it stays time-aligned.

#include "mpc_plugin.h"
#include "dsp/math.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

using namespace mpc::dsp;
using cfloat = std::complex<float>;

struct FFTPlan
{
    size_t n = 0;
    std::vector<uint32_t> rev;
    std::vector<float> cs, sn;
    void init(size_t size)
    {
        n = size; rev.assign(size, 0); cs.assign(size / 2, 0.0f); sn.assign(size / 2, 0.0f);
        int bits = 0; while ((size_t)1 << bits < n) ++bits;
        for (size_t i = 0; i < n; ++i) { uint32_t r = 0; for (int b = 0; b < bits; ++b) if (i & ((size_t)1 << b)) r |= 1u << (bits - 1 - b); rev[i] = r; }
        for (size_t k = 0; k < n / 2; ++k) { const double a = -2.0 * 3.14159265358979323846 * (double)k / (double)n; cs[k] = (float)std::cos(a); sn[k] = (float)std::sin(a); }
    }
    void run(cfloat* data, bool inverse) const
    {
        float* d = reinterpret_cast<float*>(data);
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

class MeltClean : public mpc::Plugin
{
public:
    enum Params { kAmount = 0, kReduction, kNumParams };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt Clean";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'c', 'M', 'w');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        return i;
    }

    MeltClean() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("amount", "Clean", 0.0f, 100.0f, 0.0f, "%", 0));
        Param gr = floatParam("gr", "Noise Removed", -30.0f, 0.0f, 0.0f, "dB", 1);
        gr.automatable = false;
        addParam(gr);
        addProgram("Off", {0.0f});
        addProgram("Light (vocals)", {25.0f});
        addProgram("Medium", {50.0f});
        addProgram("Heavy (noisy sample)", {75.0f});
        addProgram("Max", {100.0f});
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        N_ = sampleRate > 60000.0 ? 2048 : 1024;
        H_ = N_ / 4; B_ = N_ / 2 + 1;
        plan_.init((size_t)N_);
        win_.assign((size_t)N_, 0.0f);
        for (int i = 0; i < N_; ++i) win_[(size_t)i] = std::sqrt(0.5f - 0.5f * std::cos(6.2831853f * (float)i / (float)N_));
        for (int c = 0; c < 2; ++c) { in_[c].assign((size_t)N_, 0.0f); out_[c].assign((size_t)N_, 0.0f); spec_[c].assign((size_t)N_, cfloat()); }
        ps_.assign((size_t)B_, 0.0f); noise_.assign((size_t)B_, 0.0f); subMin_.assign((size_t)B_, 0.0f);
        wins_.assign((size_t)B_ * kSubs, 0.0f); prevP_.assign((size_t)B_, 0.0f); tonal_.assign((size_t)B_, 0.0f); dev_.assign((size_t)B_, 0.4f); gain_.assign((size_t)B_, 1.0f); graw_.assign((size_t)B_, 1.0f);
        // frames per second ~172 at 44.1k: sub-window of 64 frames x 8 = ~3 s memory
        subLen_ = std::max(8, (int)std::lround(0.37 * sampleRate / (double)H_));
        meterRel_ = std::exp(-1.0f / (0.3f * (float)sampleRate / (float)H_));
        setLatency(N_);
        reset();
    }

    void reset() override
    {
        for (int c = 0; c < 2; ++c) { std::fill(in_[c].begin(), in_[c].end(), 0.0f); std::fill(out_[c].begin(), out_[c].end(), 0.0f); }
        std::fill(ps_.begin(), ps_.end(), 0.0f);
        std::fill(noise_.begin(), noise_.end(), 0.0f);
        std::fill(subMin_.begin(), subMin_.end(), kBig);
        std::fill(wins_.begin(), wins_.end(), kBig);
        std::fill(gain_.begin(), gain_.end(), 1.0f);
        std::fill(prevP_.begin(), prevP_.end(), 0.0f);
        std::fill(dev_.begin(), dev_.end(), 0.4f);
        idx_ = 0; frames_ = 0; subPos_ = 0; subIdx_ = 0; meter_ = 0.0f;
        meterHold_ = (int)(0.5 * sr_);   // leave a restored meter value alone right after a load
    }

    void process(mpc::ProcessContext& ctx) override
    {
        amount_ = get(kAmount) * 0.01f;
        const int nf = ctx.numFrames;
        float* io[2] = {ctx.outputs[0], ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr};
        const float* ins[2] = {ctx.numInputs > 0 ? ctx.inputs[0] : nullptr, ctx.numInputs > 1 ? ctx.inputs[1] : nullptr};
        if (!ins[1]) ins[1] = ins[0];
        for (int i = 0; i < nf; ++i)
        {
            const float xl = ins[0] ? ins[0][i] : 0.0f, xr = ins[1] ? ins[1][i] : xl;   // read both before writing (in place)
            const int w = N_ - H_ + idx_;
            in_[0][(size_t)w] = xl; in_[1][(size_t)w] = xr;
            float yl = out_[0][(size_t)idx_], yr = out_[1][(size_t)idx_];
            if (std::fabs(yl) < 1.0e-20f) yl = 0.0f;
            if (std::fabs(yr) < 1.0e-20f) yr = 0.0f;
            io[0][i] = sanitize(yl);
            if (io[1]) io[1][i] = sanitize(yr);
            if (++idx_ == H_) { idx_ = 0; frame(); }
        }
        if (meterHold_ > 0) meterHold_ -= nf;
        else set(kReduction, std::max(-30.0f, meter_));
    }

private:
    static constexpr int kSubs = 8;
    static constexpr float kBig = 1.0e30f;

    void frame()
    {
        // analysis
        for (int c = 0; c < 2; ++c)
        {
            for (int i = 0; i < N_; ++i) spec_[c][(size_t)i] = cfloat(in_[c][(size_t)i] * win_[(size_t)i], 0.0f);
            plan_.run(spec_[c].data(), false);
        }
        // noise tracking + gains (linked stereo)
        const float a = amount_;
        const float over = 1.0f + 2.0f * a;                 // over-subtraction
        const float floorG = dbToGain(-30.0f * a);          // deepest cut
        double eIn = 0.0, eOut = 0.0;
        const bool subDone = ++subPos_ >= subLen_;
        for (int k = 0; k < B_; ++k)
        {
            const float p = std::norm(spec_[0][(size_t)k]) + std::norm(spec_[1][(size_t)k]) + 1.0e-24f;
            float& s = ps_[(size_t)k];
            s = frames_ == 0 ? p : 0.7f * s + 0.3f * p;
            subMin_[(size_t)k] = std::min(subMin_[(size_t)k], s);
            if (subDone)
            {
                wins_[(size_t)k * kSubs + (size_t)subIdx_] = subMin_[(size_t)k];
                subMin_[(size_t)k] = kBig;
            }
            float m = subMin_[(size_t)k];
            for (int j = 0; j < kSubs; ++j) m = std::min(m, wins_[(size_t)k * kSubs + (size_t)j]);
            noise_[(size_t)k] = m * ((k == 0 || k == B_ - 1) ? 4.0f * kBias : kBias);   // DC and Nyquist bins are real-only: their minima sit much lower
            float g = std::max(floorG, 1.0f - over * noise_[(size_t)k] / s);
            // tone guard: noise power jumps around from frame to frame, a held note does not.
            // Steady (tonal) bins are left alone, so sustained pads, bass and chords never get eaten.
            float& pp = prevP_[(size_t)k];
            const float d = std::fabs(p - pp) / (p + pp);
            pp = p;
            float& dv = dev_[(size_t)k];
            dv = frames_ == 0 ? 0.4f : dv + (d - dv) * kDevRate;
            const float tonal = mpc::clamp((0.25f - dv) * (1.0f / 0.15f), 0.0f, 1.0f);
            tonal_[(size_t)k] = tonal;
            graw_[(size_t)k] = g;
        }
        if (subDone) { subPos_ = 0; subIdx_ = (subIdx_ + 1) % kSubs; }
        ++frames_;
        for (int k = 0; k < B_; ++k)
        {
            // smooth across 3 bins, then in time: open fast, close gently (no twinkling)
            const float gl = graw_[(size_t)std::max(0, k - 1)], gr = graw_[(size_t)std::min(B_ - 1, k + 1)];
            float gf = 0.25f * gl + 0.5f * graw_[(size_t)k] + 0.25f * gr;
            const float t = std::max(tonal_[(size_t)k], std::max(tonal_[(size_t)std::max(0, k - 1)], tonal_[(size_t)std::min(B_ - 1, k + 1)]));
            gf += (1.0f - gf) * t;   // tone guard (covers the note's neighbouring bins too)
            float& gs = gain_[(size_t)k];
            gs = gf > gs ? 0.3f * gs + 0.7f * gf : 0.75f * gs + 0.25f * gf;
            const float g = a <= 0.0f ? 1.0f : gs;
            const float p = std::norm(spec_[0][(size_t)k]) + std::norm(spec_[1][(size_t)k]);
            eIn += p; eOut += p * g * g;
            spec_[0][(size_t)k] *= g; spec_[1][(size_t)k] *= g;
            if (k > 0 && k < N_ / 2) { spec_[0][(size_t)(N_ - k)] = std::conj(spec_[0][(size_t)k]); spec_[1][(size_t)(N_ - k)] = std::conj(spec_[1][(size_t)k]); }
        }
        const float grDb = eIn > 1.0e-18 ? (float)(10.0 * std::log10(std::max(eOut / eIn, 1.0e-6))) : 0.0f;
        meter_ = grDb < meter_ ? grDb : meter_ + (grDb - meter_) * (1.0f - meterRel_);
        // synthesis + overlap-add (sqrt-Hann^2 at 75 % overlap sums to 2)
        for (int c = 0; c < 2; ++c)
        {
            plan_.run(spec_[c].data(), true);
            std::vector<float>& o = out_[c];
            std::copy(o.begin() + H_, o.end(), o.begin());
            std::fill(o.end() - H_, o.end(), 0.0f);
            for (int i = 0; i < N_; ++i) o[(size_t)i] += spec_[c][(size_t)i].real() * win_[(size_t)i] * 0.5f;
            std::copy(in_[c].begin() + H_, in_[c].end(), in_[c].begin());
        }
    }

    static constexpr float kDevRate = 1.0f / 35.0f;   // ~0.2 s average of the frame-to-frame change
    static constexpr float kBias = 6.0f;   // the minimum of the smoothed noise spectrum over ~3 s sits ~7.8 dB under its mean (measured)

    double sr_ = 44100.0;
    int N_ = 1024, H_ = 256, B_ = 513, idx_ = 0, subLen_ = 64, subPos_ = 0, subIdx_ = 0, meterHold_ = 0;
    long long frames_ = 0;
    float amount_ = 0.0f, meter_ = 0.0f, meterRel_ = 0.9f;
    FFTPlan plan_;
    std::vector<float> win_, in_[2], out_[2], ps_, noise_, subMin_, wins_, gain_, graw_, prevP_, dev_, tonal_;
    std::vector<cfloat> spec_[2];
};

MPC_PLUGIN_ENTRY(MeltClean)
