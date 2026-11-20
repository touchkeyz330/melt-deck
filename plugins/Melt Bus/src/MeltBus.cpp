// MeltBus.cpp - a console bus / summing-amp colour plugin for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// An original model in the spirit of the classic British transformer-coupled class-A console
// bus amplifiers. Not a circuit copy of any product. Per channel:
//
//   in x Drive ──► 2x oversampled:
//        INPUT TRANSFORMER  lows (below ~100 Hz) saturate first and harder, like an iron core
//        CLASS-A AMP        biased soft clipper: even (2nd) harmonics first, odd (3rd) as you push
//   ──► DC block ──► OUTPUT TONE (Flat / Warm / Bright) ──► console bleed (L<->R) ──► Auto Gain ──► Mix ──► Trim
//
// Auto Gain divides the drive back out, so turning Drive changes the colour, not the volume:
// quiet parts pass at the same level and only the loud parts get thicker.
// Latency: 31 samples (the oversampling filters), reported to the host.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/biquad.h"

#include <algorithm>
#include <cmath>

using namespace mpc::dsp;

class MeltBus : public mpc::Plugin
{
public:
    enum Params { kDrive = 0, kTone, kOutput, kMix, kAutoGain, kBleed, kNoise, kNumParams };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt Bus";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', '6', 'K', 'n');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        i.latencySamples = kLatency;                   // 31 samples at 1x
        return i;
    }

    MeltBus() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("drive", "Drive", 0.0f, 24.0f, 4.0f, "dB", 1));
        addParam(choiceParam("tone", "Tone", {"Flat", "Warm", "Bright"}, 1));
        addParam(floatParam("out", "Output", -12.0f, 12.0f, 0.0f, "dB", 1));
        addParam(floatParam("mix", "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(boolParam("auto", "Auto Gain", true));
        addParam(floatParam("bleed", "Bleed", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(boolParam("noise", "Noise", false));
        // half-band low-pass for 2x oversampling: windowed sinc (Kaiser, beta 8), cutoff fs/4
        const double beta = 8.5;
        auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 30; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        double sum = 0;
        for (int n = 0; n < kTaps; ++n)
        {
            const double m = n - (kTaps - 1) / 2.0;
            const double sinc = m == 0 ? 0.5 : std::sin(3.14159265358979 * 0.5 * m) / (3.14159265358979 * m);
            const double r = 2.0 * n / (kTaps - 1) - 1.0;
            h_[n] = (float)(sinc * i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0(beta));
            sum += h_[n];
        }
        for (float& v : h_) v = (float)(v / sum);
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        // input-transformer split (at the 2x rate) and DC blocker (1x)
        lfA_ = 1.0f - std::exp(-2.0f * 3.14159265f * 100.0f / (float)(2.0 * sampleRate));
        dcA_ = 1.0f - std::exp(-2.0f * 3.14159265f * 8.0f / (float)sampleRate);
        bleedA_ = 1.0f - std::exp(-2.0f * 3.14159265f * 1500.0f / (float)sampleRate);
        lastTone_ = -1;
        reset();
    }

    void reset() override
    {
        for (int c = 0; c < 2; ++c)
        {
            std::fill(std::begin(upHist_[c]), std::end(upHist_[c]), 0.0f);
            std::fill(std::begin(dnHist_[c]), std::end(dnHist_[c]), 0.0f);
            lf_[c] = dcX_[c] = dcY_[c] = bleedHp_[c] = 0.0f;
            dcLp_[c] = 0.0f;
        }
        lowShelf_.reset(); highShelf_.reset();
        upPos_ = dryPos_ = 0; dnPos_[0] = dnPos_[1] = 0;
        for (int c = 0; c < 2; ++c) std::fill(std::begin(dry_[c]), std::end(dry_[c]), 0.0f);
        driveG_ = dbToGain(get(kDrive));
        rng_ = 22222u;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int tone = getInt(kTone);
        if (tone != lastTone_) setTone(tone);
        const float driveTarget = dbToGain(get(kDrive));
        const bool autoGain = getBool(kAutoGain);
        const float outG = dbToGain(get(kOutput));
        const float mix = get(kMix) * 0.01f;
        const float bleed = get(kBleed) * 0.01f * 0.0178f;   // up to -35 dB of crosstalk
        const bool noise = getBool(kNoise);
        const float k = 1.0f - std::exp(-1.0f / (0.02f * (float)sr_));
        const int n = ctx.numFrames;
        const float* inL = ctx.inputs && ctx.numInputs > 0 ? ctx.inputs[0] : nullptr;
        const float* inR = ctx.inputs && ctx.numInputs > 1 ? ctx.inputs[1] : inL;
        float* oL = ctx.outputs[0];
        float* oR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        for (int i = 0; i < n; ++i)
        {
            driveG_ += k * (driveTarget - driveG_);
            const float xl = inL ? inL[i] : 0.0f, xr = inR ? inR[i] : xl;
            const float x[2] = {xl, xr};
            float y[2];
            for (int c = 0; c < 2; ++c)
            {
                // ---- 2x up
                upHist_[c][upPos_] = x[c] * driveG_;
                float u[2];
                for (int ph = 0; ph < 2; ++ph)
                {
                    float acc = 0.0f;
                    for (int t = ph, j = 0; t < kTaps; t += 2, ++j)
                        acc += h_[t] * upHist_[c][(upPos_ - j + kHist) & (kHist - 1)];
                    u[ph] = 2.0f * acc;
                }
                // ---- nonlinear stages at 2x; the 2x-down output is taken right after phase 0 so
                // the total delay is a whole number of samples (dry and wet line up for Mix)
                float acc = 0.0f;
                for (int ph = 0; ph < 2; ++ph)
                {
                    float s = u[ph];
                    // input transformer: the low band saturates on its own, earlier
                    lf_[c] += lfA_ * (s - lf_[c]);
                    const float lo = lf_[c], hi = s - lo;
                    const float loSat = transformer(lo);
                    s = hi + loSat;
                    // class-A stage: biased tanh, unity small-signal slope
                    s = classA(s);
                    dnHist_[c][dnPos_[c]] = s;
                    dnPos_[c] = (dnPos_[c] + 1) & (kHist - 1);
                    if (ph == 0)
                        for (int t = 0; t < kTaps; ++t) acc += h_[t] * dnHist_[c][(dnPos_[c] - 1 - t + kHist) & (kHist - 1)];
                }
                // DC block (the bias makes a little DC)
                dcLp_[c] += dcA_ * (acc - dcLp_[c]);
                y[c] = acc - dcLp_[c];
                // the dry signal at the same delay (15 samples) for Mix
                dry_[c][dryPos_] = x[c];
            }
            upPos_ = (upPos_ + 1) & (kHist - 1);
            // output tone
            float l = y[0], r = y[1];
            if (lastTone_ != 0) { lowShelf_.process(l, r); highShelf_.process(l, r); }
            // console bleed: a little of each side, mostly mids/highs (capacitive crosstalk)
            if (bleed > 0.0f)
            {
                bleedHp_[0] += bleedA_ * (l - bleedHp_[0]);
                bleedHp_[1] += bleedA_ * (r - bleedHp_[1]);
                const float hl = l - bleedHp_[0], hr = r - bleedHp_[1];
                const float l2 = l + bleed * (hr + 0.3f * bleedHp_[1]);
                const float r2 = r + bleed * (hl + 0.3f * bleedHp_[0]);
                l = l2; r = r2;
            }
            if (autoGain)
            {
                // divide the drive back out, plus the loudness the saturation shaves off a typical
                // full mix (~-16 dBFS RMS, measured: 0.4 dB at no drive up to ~10 dB at +24)
                const float d = 20.0f * std::log10(driveG_);
                const float g = dbToGain(0.4f + 0.017f * d * d) / driveG_;
                l *= g; r *= g;
            }
            if (noise)
            {
                auto rnd = [&]() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return (float)(rng_ >> 8) / 8388608.0f - 1.0f; };
                l += rnd() * 3.0e-5f; r += rnd() * 3.0e-5f;   // about -90 dBFS console hiss
            }
            const int dp = (dryPos_ - kLatency + kHist) & (kHist - 1);
            const float dl = dry_[0][dp], dr = dry_[1][dp];
            dryPos_ = (dryPos_ + 1) & (kHist - 1);
            l = dl + mix * (l - dl); r = dr + mix * (r - dr);
            oL[i] = sanitize(l * outG);
            if (oR) oR[i] = sanitize(r * outG);
        }
    }

private:
    static constexpr int kTaps = 63;
    static constexpr int kHist = 128;         // power of two, > kTaps
    static constexpr int kLatency = 31;      // (63-1)/2 at 2x, up + down, decimated on the even phase

    // Iron-core feel: transparent when small, rounds off big low-frequency swings.
    static inline float transformer(float x)
    {
        const float a = 0.9f;   // saturation point (relative to full scale)
        return a * fastTanh(x / a);
    }
    // Class-A amplifier: tanh with a DC bias -> asymmetric -> 2nd harmonic first.
    static inline float classA(float x)
    {
        constexpr float b = 0.08f;   // gentle bias: 2nd harmonic leads, ~-46 dB at -12 dBFS with no drive
        const float tb = std::tanh(b);
        const float slope = 1.0f - tb * tb;
        return (fastTanh(x + b) - tb) / slope;
    }

    void setTone(int tone)
    {
        lastTone_ = tone;
        if (tone == 1)        // Warm: weight in the lows, softer top
        {
            lowShelf_.setCoeffs(BiquadCoeffs::lowShelf(sr_, 80.0f, 0.7f, 1.5f));
            highShelf_.setCoeffs(BiquadCoeffs::highShelf(sr_, 12000.0f, 0.7f, -1.0f));
        }
        else if (tone == 2)   // Bright: open, airy top, slightly leaner low end
        {
            lowShelf_.setCoeffs(BiquadCoeffs::lowShelf(sr_, 50.0f, 0.7f, -0.5f));
            highShelf_.setCoeffs(BiquadCoeffs::highShelf(sr_, 10000.0f, 0.7f, 1.8f));
        }
        lowShelf_.reset(); highShelf_.reset();
    }

    double sr_ = 44100.0;
    float h_[kTaps] = {};
    float upHist_[2][kHist] = {}, dnHist_[2][kHist] = {}, dry_[2][kHist] = {};
    int upPos_ = 0, dnPos_[2] = {}, dryPos_ = 0;
    float lf_[2] = {}, lfA_ = 0.01f, dcA_ = 0.001f, dcLp_[2] = {}, dcX_[2] = {}, dcY_[2] = {};
    float bleedHp_[2] = {}, bleedA_ = 0.2f;
    StereoBiquad lowShelf_, highShelf_;
    int lastTone_ = -1;
    float driveG_ = 1.0f;
    uint32_t rng_ = 22222u;
};

MPC_PLUGIN_ENTRY(MeltBus)
