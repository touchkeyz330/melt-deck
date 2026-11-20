// MeltSweep.cpp - a one-knob DJ-style filter for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// One knob: centre = off, turn left = low-pass (darker), turn right = high-pass (thinner).
// Resonance rises as you turn, so sweeps sing without a second knob. The filter is a pair of
// zero-delay-feedback state-variable filters (24 dB/oct, or 12), modulation-safe, smoothed
// so fast twists never click or zipper. Near the centre it fades smoothly into a true bypass.
// Optional extras on a second page: resonance amount, slope, drive, mix.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/svf.h"

#include <algorithm>
#include <cmath>

using namespace mpc::dsp;

class MeltSweep : public mpc::Plugin
{
public:
    enum Params { kFilter = 0, kReso, kSlope, kDrive, kMix, kNumParams };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt Sweep";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'l', 'm', 'a');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        return i;
    }

    MeltSweep() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("filter", "Filter", -100.0f, 100.0f, 0.0f, "", 0));
        addParam(floatParam("reso", "Resonance", 0.0f, 100.0f, 65.0f, "%", 0));
        addParam(choiceParam("slope", "Slope", {"24 dB", "12 dB"}, 0));
        addParam(floatParam("drive", "Drive", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("mix", "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
    }

    // Show "LP 1.2k" / "OFF" / "HP 350" instead of a bare number.
    bool formatValue(int index, float v, std::string& out) override
    {
        if (index != kFilter) return false;
        if (std::fabs(v) < kDead * 100.0f) { out = "OFF"; return true; }
        const float hz = cutoffFor(v * 0.01f);
        char b[24];
        if (hz >= 1000.0f) std::snprintf(b, sizeof b, "%s %.1fk", v < 0 ? "LP" : "HP", hz / 1000.0f);
        else std::snprintf(b, sizeof b, "%s %.0f", v < 0 ? "LP" : "HP", hz);
        out = b;
        return true;
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        for (auto& f : f_) f.setSampleRate(sampleRate);
        // knob smoothing: ~30 ms, applied per sample on the knob position
        smoothCoef_ = std::exp(-1.0f / (0.030f * (float)sampleRate));
        reset();
    }

    void reset() override
    {
        for (auto& f : f_) f.reset();
        pos_ = get(kFilter) * 0.01f;
        ctl_ = 0;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const float target = get(kFilter) * 0.01f;
        const float resoAmt = get(kReso) * 0.01f;
        const bool steep = getInt(kSlope) == 0;
        const float drive = get(kDrive) * 0.01f;
        const float driveGain = 1.0f + drive * 6.0f, driveNorm = 1.0f / fastTanh(driveGain);
        const float mix = get(kMix) * 0.01f;
        const int n = ctx.numFrames;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        const float* inL = ctx.inputs && ctx.numInputs > 0 ? ctx.inputs[0] : nullptr;
        const float* inR = ctx.inputs && ctx.numInputs > 1 ? ctx.inputs[1] : inL;

        for (int i = 0; i < n; ++i)
        {
            const float xl = inL ? inL[i] : 0.0f;
            const float xr = inR ? inR[i] : xl;
            pos_ = target + (pos_ - target) * smoothCoef_;
            // update filter coefficients every 8 samples from the smoothed knob
            if ((ctl_++ & 7) == 0) updateFilter(resoAmt);
            const float a = std::fabs(pos_);
            // the filters always run (even at the centre) so they are settled when you turn in
            float fl = xl, fr = xr;
            if (drive > 0.0f) { fl = fastTanh(fl * driveGain) * driveNorm; fr = fastTanh(fr * driveGain) * driveNorm; }
            // a low-pass chain and a high-pass chain both run; the idle one sits wide open
            float ll = f_[0].process(fl, 0), lr = f_[2].process(fr, 0);
            float hl = f_[4].process(fl, 2), hr = f_[6].process(fr, 2);
            if (steep) { ll = f_[1].process(ll, 0); lr = f_[3].process(lr, 0); hl = f_[5].process(hl, 2); hr = f_[7].process(hr, 2); }
            if (pos_ < 0.0f) { fl = ll; fr = lr; } else { fl = hl; fr = hr; }
            fl = safe(fl * makeup_); fr = safe(fr * makeup_);   // tame the loudest whistles
            // fade from bypass into the filter just outside the centre (exact bypass inside)
            const float w = mpc::clamp((a - kDead * 0.5f) / (kDead * 1.5f), 0.0f, 1.0f) * mix;
            const float yl = w > 0.0f ? xl + w * (fl - xl) : xl;
            const float yr = w > 0.0f ? xr + w * (fr - xr) : xr;
            outL[i] = sanitize(yl);
            if (outR) outR[i] = sanitize(yr);
        }
    }

private:
    static float safe(float x)   // transparent below about -2 dBFS
    {
        const float a = std::fabs(x);
        if (a < 0.8f) return x;
        const float y = 0.8f + 0.2f * fastTanh((a - 0.8f) / 0.2f);
        return x < 0 ? -y : y;
    }
    static constexpr float kDead = 0.02f;   // +-2 % around the centre is "off"

    // Knob position (-1..1) -> cutoff. Left: 20 kHz down to 40 Hz. Right: 20 Hz up to 12 kHz.
    static float cutoffFor(float p)
    {
        const float a = mpc::clamp((std::fabs(p) - kDead) / (1.0f - kDead), 0.0f, 1.0f);
        if (p < 0.0f) return 20000.0f * std::pow(40.0f / 20000.0f, a);
        return 20.0f * std::pow(12000.0f / 20.0f, a);
    }

    void updateFilter(float resoAmt)
    {
        const float a = std::fabs(pos_);
        const float lpHz = std::min(pos_ < 0.0f ? cutoffFor(pos_) : 20000.0f, 0.45f * (float)sr_);
        const float hpHz = pos_ > 0.0f ? cutoffFor(pos_) : 20.0f;
        // resonance grows over the first part of the turn, so it sings during the sweep
        const float r = 0.95f * std::pow(resoAmt, 0.7f) * mpc::clamp((a - kDead) / 0.25f, 0.0f, 1.0f);
        const float rl = pos_ < 0.0f ? r : 0.0f, rh = pos_ > 0.0f ? r : 0.0f;
        const bool twelve = getInt(kSlope) == 1;   // 12 dB: only stage 1 runs, so it gets the full resonance
        f_[0].set(lpHz, twelve ? rl : rl * 0.6f); f_[2].set(lpHz, twelve ? rl : rl * 0.6f);
        f_[1].set(lpHz, rl); f_[3].set(lpHz, rl);
        f_[4].set(hpHz, twelve ? rh : rh * 0.6f); f_[6].set(hpHz, twelve ? rh : rh * 0.6f);
        f_[5].set(hpHz, rh); f_[7].set(hpHz, rh);
        makeup_ = 1.0f;   // pass band stays at unity: no level drop when the filter engages
    }

    double sr_ = 44100.0;
    SVF f_[8];   // LP chain: 0-3 (L1, L2, R1, R2), HP chain: 4-7
    float pos_ = 0.0f, smoothCoef_ = 0.999f, makeup_ = 1.0f;
    unsigned ctl_ = 0;
};

MPC_PLUGIN_ENTRY(MeltSweep)
