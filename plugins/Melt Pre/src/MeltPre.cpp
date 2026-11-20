// MeltPre.cpp - a British-style class-A mic preamp for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// An original model in the spirit of the classic transformer-coupled, two-stage class-A
// console mic pres (the "red knob" kind). Not a circuit copy of any product. Per channel:
//
//   in -> phase -> stepped GAIN (5 dB steps) + fine ->
//     2x oversampled:
//       INPUT TRANSFORMER  lows saturate first; Impedance sets how the source "loads" it
//       STAGE 1 (class-A)  biased soft clipper: 2nd harmonic leads
//       STAGE 2 (output amp, class-A) takes over as Gain goes past +20: thicker, 3rd harmonic
//       OUTPUT TRANSFORMER gentle top-end roll-off and a little weight
//   -> DC block -> High-pass (Off/50/80/160/300 Hz, 18 dB/oct) -> Auto Trim -> Output -> Mix
//
// Auto Trim removes the gain you add (plus the loudness the saturation takes), so the
// Gain knob changes colour, not volume. Latency: 31 samples, reported to the host.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/biquad.h"

#include <algorithm>
#include <cmath>

using namespace mpc::dsp;

class MeltPre : public mpc::Plugin
{
public:
    enum Params { kGain = 0, kFine, kImpedance, kHpf, kPhase, kOutput, kAutoTrim, kMix, kNumParams };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt Pre";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'n', 'S', 'V');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        i.latencySamples = kLatency;
        return i;
    }

    MeltPre() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(choiceParam("gain", "Gain", {"0 dB", "+5 dB", "+10 dB", "+15 dB", "+20 dB", "+25 dB", "+30 dB"}, 2));
        addParam(floatParam("fine", "Fine", -5.0f, 5.0f, 0.0f, "dB", 1));
        addParam(choiceParam("imp", "Impedance", {"Lo", "Hi"}, 0));
        addParam(choiceParam("hpf", "High Pass", {"Off", "50 Hz", "80 Hz", "160 Hz", "300 Hz"}, 0));
        addParam(boolParam("phase", "Phase", false));
        addParam(floatParam("out", "Output", -24.0f, 12.0f, 0.0f, "dB", 1));
        addParam(boolParam("trim", "Auto Trim", true));
        addParam(floatParam("mix", "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
        // half-band low-pass for 2x oversampling (same proven design as Melt Bus)
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
        lfA_ = 1.0f - std::exp(-2.0f * 3.14159265f * 120.0f / (float)(2.0 * sampleRate));
        dcA_ = 1.0f - std::exp(-2.0f * 3.14159265f * 8.0f / (float)sampleRate);
        lastImp_ = lastHpf_ = -1;
        reset();
    }

    void reset() override
    {
        for (int c = 0; c < 2; ++c)
        {
            std::fill(std::begin(upHist_[c]), std::end(upHist_[c]), 0.0f);
            std::fill(std::begin(dnHist_[c]), std::end(dnHist_[c]), 0.0f);
            std::fill(std::begin(dry_[c]), std::end(dry_[c]), 0.0f);
            lf_[c] = dcLp_[c] = 0.0f;
            dnPos_[c] = 0;
        }
        upPos_ = dryPos_ = 0;
        tone1_.reset(); tone2_.reset();
        for (auto& b : hpf_) b.reset();
        gainDb_ = targetGainDb();
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int imp = getInt(kImpedance), hp = getInt(kHpf);
        if (imp != lastImp_) setImpedance(imp);
        if (hp != lastHpf_) setHpf(hp);
        const float tgt = targetGainDb();
        const float ph = getBool(kPhase) ? -1.0f : 1.0f;
        const bool trim = getBool(kAutoTrim);
        const float outG = dbToGain(get(kOutput));
        const float mix = get(kMix) * 0.01f;
        const float k = 1.0f - std::exp(-1.0f / (0.02f * (float)sr_));
        const int n = ctx.numFrames;
        const float* inL = ctx.inputs && ctx.numInputs > 0 ? ctx.inputs[0] : nullptr;
        const float* inR = ctx.inputs && ctx.numInputs > 1 ? ctx.inputs[1] : inL;
        float* oL = ctx.outputs[0];
        float* oR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        for (int i = 0; i < n; ++i)
        {
            gainDb_ += k * (tgt - gainDb_);
            // the gain is shared between the two stages: stage 2 only gets pushed past +20 dB
            const float g1 = dbToGain(std::min(gainDb_, 20.0f));
            const float g2 = dbToGain(std::max(0.0f, gainDb_ - 20.0f));
            const float x[2] = {inL ? inL[i] : 0.0f, inR ? inR[i] : (inL ? inL[i] : 0.0f)};
            float y[2];
            for (int c = 0; c < 2; ++c)
            {
                upHist_[c][upPos_] = x[c] * ph * g1 * kHead;   // headroom: 0 dBFS sits ~9 dB under the stage's clip point
                float u[2];
                for (int p = 0; p < 2; ++p)
                {
                    float acc = 0.0f;
                    for (int t = p, j = 0; t < kTaps; t += 2, ++j) acc += h_[t] * upHist_[c][(upPos_ - j + kHist) & (kHist - 1)];
                    u[p] = 2.0f * acc;
                }
                float acc = 0.0f;
                for (int p = 0; p < 2; ++p)
                {
                    float s = u[p];
                    // input transformer: lows saturate on their own, earlier
                    lf_[c] += lfA_ * (s - lf_[c]);
                    const float lo = lf_[c];
                    s = (s - lo) + ironSat_ * fastTanh(lo / ironSat_);
                    // stage 1, then stage 2 (output amp) with its own extra gain
                    s = stage(s, 0.10f);
                    s = stage(s * g2, 0.16f) / g2;
                    dnHist_[c][dnPos_[c]] = s;
                    dnPos_[c] = (dnPos_[c] + 1) & (kHist - 1);
                    if (p == 0)
                        for (int t = 0; t < kTaps; ++t) acc += h_[t] * dnHist_[c][(dnPos_[c] - 1 - t + kHist) & (kHist - 1)];
                }
                acc *= 1.0f / kHead;
                dcLp_[c] += dcA_ * (acc - dcLp_[c]);
                y[c] = acc - dcLp_[c];
                dry_[c][dryPos_] = x[c];
            }
            upPos_ = (upPos_ + 1) & (kHist - 1);
            float l = y[0], r = y[1];
            tone1_.process(l, r); tone2_.process(l, r);   // output transformer + impedance colour
            if (lastHpf_ > 0) for (auto& b : hpf_) b.process(l, r);
            if (trim)
            {
                // remove the added gain, plus what the saturation takes off a typical mix
                const float d = std::max(0.0f, gainDb_);
                const float g = dbToGain(-d + trimMakeup(d));
                l *= g; r *= g;
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
    static constexpr int kHist = 128;
    static constexpr int kLatency = 31;
    static constexpr float kHead = 0.35f;

    // measured loudness the saturation takes off a mix-level beat (~-22 dBFS RMS) at each gain
    static float trimMakeup(float d)
    {
        static const float tab[7] = {0.0f, 0.0f, 0.2f, 0.7f, 1.8f, 7.7f, 14.6f};   // saturation loss only (not the Impedance tone)
        const float p = mpc::clamp(d / 5.0f, 0.0f, 6.0f);
        const int i = std::min(5, (int)p);
        return tab[i] + (p - (float)i) * (tab[i + 1] - tab[i]);
    }
    float targetGainDb() const { return 5.0f * (float)getInt(kGain) + get(kFine); }

    // class-A stage: biased tanh, unity small-signal slope; bias sets the 2nd-harmonic share
    static inline float stage(float x, float b)
    {
        const float tb = std::tanh(b);
        return (fastTanh(x + b) - tb) / (1.0f - tb * tb);
    }

    void setImpedance(int imp)
    {
        lastImp_ = imp;
        if (imp == 0)   // Lo: the source loads the transformer: thicker lows, softer top
        {
            tone1_.setCoeffs(BiquadCoeffs::lowShelf(sr_, 120.0f, 0.7f, 1.2f));
            tone2_.setCoeffs(BiquadCoeffs::highShelf(sr_, 9000.0f, 0.7f, -1.5f));
            ironSat_ = 0.75f;   // iron saturates a little sooner
        }
        else            // Hi: lighter load: more open and crisp, a touch leaner
        {
            tone1_.setCoeffs(BiquadCoeffs::lowShelf(sr_, 80.0f, 0.7f, -0.4f));
            tone2_.setCoeffs(BiquadCoeffs::highShelf(sr_, 6000.0f, 0.7f, 1.3f));
            ironSat_ = 0.95f;
        }
        tone1_.reset(); tone2_.reset();
    }

    void setHpf(int hp)
    {
        lastHpf_ = hp;
        static const float f[5] = {0.0f, 50.0f, 80.0f, 160.0f, 300.0f};
        if (hp > 0)
        {
            // 18 dB/oct: 3rd-order Butterworth = a 1st-order section + a 2nd-order section (Q 1.0)
            const float K = std::tan(3.14159265f * f[hp] / (float)sr_);
            BiquadCoeffs one;   // bilinear 1st-order high-pass
            one.b0 = 1.0f / (1.0f + K); one.b1 = -one.b0; one.b2 = 0.0f; one.a1 = (K - 1.0f) / (K + 1.0f); one.a2 = 0.0f;
            hpf_[0].setCoeffs(one);
            hpf_[1].setCoeffs(BiquadCoeffs::highpass(sr_, f[hp], 1.0f));
        }
        for (auto& b : hpf_) b.reset();
    }

    double sr_ = 44100.0;
    float h_[kTaps] = {};
    float upHist_[2][kHist] = {}, dnHist_[2][kHist] = {}, dry_[2][kHist] = {};
    int upPos_ = 0, dnPos_[2] = {}, dryPos_ = 0;
    float lf_[2] = {}, lfA_ = 0.01f, dcA_ = 0.001f, dcLp_[2] = {};
    float ironSat_ = 0.8f, gainDb_ = 0.0f;
    StereoBiquad tone1_, tone2_, hpf_[2];
    int lastImp_ = -1, lastHpf_ = -1;
};

MPC_PLUGIN_ENTRY(MeltPre)
