// SilkOpto.cpp - an optical-style tube compressor for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// An original design in the spirit of classic optical tube compressors: a smooth, wide-knee
// gain cell whose release depends on the music (fast at first, then slower the longer and
// harder it has been working), a tube output stage, and modern extras (side-chain high-pass,
// parallel Mix, stereo link). No circuit, name or artwork is copied from any product.
//
// Signal path:
//   in -> detector (side-chain HPF, linked L/R energy, ~1 ms smoothing)
//      -> gain computer (threshold, ratio, 10 dB soft knee)
//      -> ballistics: Fixed (opto: two-stage, program-dependent release),
//                     Manual (Attack/Release knobs), or Fix+Man (both blended)
//      -> gain cell -> Tube colour -> make-up Gain -> Mix with dry -> out
//
// CPU: gain maths (log/exp) runs every 16 samples and is interpolated in between.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/biquad.h"
#include "dsp/smoother.h"

using namespace mpc::dsp;

class SilkOpto : public mpc::Plugin
{
public:
    enum Params { kGain, kThreshold, kRatio, kTiming, kAttack, kRelease, kTube, kScHpf, kMix, kLink, kMeter, kNumParams };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Silk Opto";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', '2', 'g', 'x');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        return i;
    }

    SilkOpto() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("gain",  "Gain", 0.0f, 30.0f, 0.0f, "dB", 1));
        addParam(floatParam("thr",   "Threshold", -40.0f, 0.0f, -18.0f, "dB", 1));
        addParam(floatParam("ratio", "Ratio", 1.5f, 10.0f, 3.0f, ":1", 1, 0.5f));
        addParam(choiceParam("timing", "Timing", {"Fixed", "Manual", "Fix+Man"}, 0));
        addParam(logParam  ("atk",   "Attack", 0.5f, 300.0f, 10.0f, "ms", 1));
        addParam(logParam  ("rel",   "Release", 50.0f, 5000.0f, 400.0f, "ms", 0));
        addParam(floatParam("tube",  "Tube", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(choiceParam("schpf", "SC HPF", {"Off", "60 Hz", "100 Hz", "150 Hz"}, 0));
        addParam(floatParam("mix",   "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(boolParam ("link",  "Link", true));
        Param meter = floatParam("gr", "GR", -30.0f, 0.0f, 0.0f, "dB", 1);   // display: gain reduction
        meter.automatable = false;
        addParam(meter);
        if (numParams() != kNumParams) mpc::log("SilkOpto: parameter count mismatch");

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> o)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& p : o) v[(size_t)p.first] = p.second;
            addProgram(name, std::move(v));
        };
        program("Gentle Glue", {});
        program("Vocal Smooth", {{kThreshold, -22}, {kRatio, 4}, {kGain, 5}, {kScHpf, 2}, {kTube, 35}});
        program("Bass Level", {{kThreshold, -20}, {kRatio, 6}, {kTiming, 2}, {kAttack, 20}, {kRelease, 300}, {kGain, 6}});
        program("Drum Bus Squash", {{kThreshold, -28}, {kRatio, 8}, {kTiming, 1}, {kAttack, 15}, {kRelease, 120}, {kGain, 10}, {kMix, 50}, {kScHpf, 1}});
        program("Mix Bus Silk", {{kThreshold, -12}, {kRatio, 2}, {kGain, 2}, {kScHpf, 2}, {kTube, 20}});
        program("Lo-Fi Pump", {{kThreshold, -32}, {kRatio, 10}, {kTiming, 1}, {kAttack, 2}, {kRelease, 250}, {kGain, 14}, {kTube, 80}});
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        const float ctlRate = (float)sampleRate / (float)kCtl;
        auto coef = [ctlRate](float ms) { return 1.0f - std::exp(-1000.0f / (ms * ctlRate)); };
        optoAtk_     = coef(8.0f);     // opto cell: ~8 ms attack
        optoFastRel_ = coef(60.0f);    // first half of the release: ~60 ms
        memCharge_   = coef(1500.0f);  // how fast the cell "remembers" heavy work
        memDecay_    = coef(6000.0f);  // and forgets it
        ctlRate_ = ctlRate;
        detCoef_ = 1.0f - std::exp(-1.0f / (0.001f * (float)sampleRate));   // 1 ms energy smoothing
        gain_.setTime(20.0f, sampleRate);
        mix_.setTime(20.0f, sampleRate);
        scDirty_ = true;
    }

    void reset() override
    {
        det_[0] = det_[1] = 0.0f;
        for (int c = 0; c < 2; ++c) { fast_[c] = slow_[c] = man_[c] = 0.0f; memory_[c] = 0.0f; curGain_[c] = stepGain_[c] = 1.0f; tubeLast_[c] = 0.0f; sc_[c].reset(); }
        ctl_ = 0;
        gain_.snap(dbToGain(get(kGain)));
        mix_.snap(get(kMix) * 0.01f);
        scDirty_ = true;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int hp = mpc::clamp(getInt(kScHpf), 0, 3);
        if (scDirty_ || hp != lastHp_)
        {
            static const float hz[] = { 0.0f, 60.0f, 100.0f, 150.0f };
            const BiquadCoeffs k = hp > 0 ? BiquadCoeffs::highpass(sr_, hz[hp], 0.7071f) : BiquadCoeffs::identity();
            sc_[0].setCoeffs(k); sc_[1].setCoeffs(k);
            lastHp_ = hp; scDirty_ = false;
        }
        const float thr = get(kThreshold), ratio = get(kRatio);
        const int timing = getInt(kTiming);
        const float manAtk = 1.0f - std::exp(-1000.0f / (get(kAttack) * ctlRate_));
        const float manRel = 1.0f - std::exp(-1000.0f / (get(kRelease) * ctlRate_));
        const bool link = getBool(kLink);
        const float tube = get(kTube) * 0.01f;
        const float tubeDrive = 1.0f + 3.0f * tube;           // up to ~12 dB into the tube stage
        const float tubeBias = 0.2f * tube;
        const float tubeBiasOut = shape(tubeBias);
        gain_.setTarget(dbToGain(get(kGain)));
        mix_.setTarget(get(kMix) * 0.01f);

        const int nch = ctx.numOutputs < 2 ? ctx.numOutputs : 2;
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            float in[2] = { ctx.inputs[0][i], nch > 1 ? ctx.inputs[1][i] : ctx.inputs[0][i] };
            // detector energy
            for (int c = 0; c < 2; ++c) { const float s = sc_[c].process(in[c]); det_[c] += detCoef_ * (s * s - det_[c]); }

            if (ctl_ == 0)   // control rate: gain computer and ballistics
            {
                const float e[2] = { link ? std::max(det_[0], det_[1]) : det_[0], link ? std::max(det_[0], det_[1]) : det_[1] };
                for (int c = 0; c < 2; ++c)
                {
                    const float levelDb = 10.0f * std::log10(e[c] * 2.0f + 1.0e-12f);   // x2: sine RMS -> peak level
                    const float target = gainReduction(levelDb, thr, ratio);           // dB, >= 0
                    // Opto: two envelopes share the work; the slow one's release stretches with "memory".
                    const float slowRel = 1.0f - std::exp(-1000.0f / ((300.0f + 3000.0f * memory_[c]) * ctlRate_));
                    fast_[c] += (target > fast_[c] ? optoAtk_ : optoFastRel_) * (target - fast_[c]);
                    slow_[c] += (target > slow_[c] ? optoAtk_ : slowRel) * (target - slow_[c]);
                    const float work = mpc::clamp(target / 10.0f, 0.0f, 1.0f);
                    memory_[c] += (work > memory_[c] ? memCharge_ : memDecay_) * (work - memory_[c]);
                    man_[c] += (target > man_[c] ? manAtk : manRel) * (target - man_[c]);
                    const float opto = 0.5f * (fast_[c] + slow_[c]);
                    const float gr = timing == 0 ? opto : timing == 1 ? man_[c] : 0.5f * (opto + man_[c]);
                    const float next = std::pow(10.0f, -gr / 20.0f);
                    stepGain_[c] = (next - curGain_[c]) / (float)kCtl;   // interpolate to it over the next 16 samples
                }
                if ((meterCount_++ & 63) == 0)   // ~23 updates a second: show gain reduction on the GR control
                    set(kMeter, mpc::clamp(20.0f * std::log10(std::min(curGain_[0], curGain_[1]) + 1.0e-9f), -30.0f, 0.0f));
            }
            ctl_ = (ctl_ + 1) % kCtl;

            const float mk = gain_.process();
            const float mx = mix_.process();
            for (int c = 0; c < nch; ++c)
            {
                curGain_[c] += stepGain_[c];
                float y = in[c] * curGain_[c];
                if (tube > 0.0f) y = (shape(y * tubeDrive * 0.5f + tubeBias) - tubeBiasOut) * 2.0f / tubeDrive;
                y = sanitize(y * mk);
                ctx.outputs[c][i] = in[c] + (y - in[c]) * mx;
            }
        }
    }

private:
    static constexpr int kCtl = 16;

    static inline float gainReduction(float levelDb, float thr, float ratio)
    {
        const float knee = 10.0f, over = levelDb - thr, slope = 1.0f - 1.0f / ratio;
        if (over <= -0.5f * knee) return 0.0f;
        if (over < 0.5f * knee) { const float t = over + 0.5f * knee; return slope * t * t / (2.0f * knee); }
        return slope * over;
    }
    // gentle tube curve: y = u - 4/27 u^3 up to |u| = 1.5
    static inline float shape(float u)
    {
        if (u > 1.5f) return 1.0f;
        if (u < -1.5f) return -1.0f;
        return u - (4.0f / 27.0f) * u * u * u;
    }

    double sr_ = 44100.0;
    float ctlRate_ = 2756.0f, detCoef_ = 0.02f;
    float optoAtk_ = 0.1f, optoFastRel_ = 0.01f, memCharge_ = 0.001f, memDecay_ = 0.0001f;
    float det_[2] = {0, 0}, fast_[2] = {0, 0}, slow_[2] = {0, 0}, man_[2] = {0, 0}, memory_[2] = {0, 0};
    float curGain_[2] = {1, 1}, stepGain_[2] = {0, 0}, tubeLast_[2] = {0, 0};
    int ctl_ = 0, lastHp_ = -1;
    unsigned meterCount_ = 0;
    bool scDirty_ = true;
    Biquad sc_[2];
    Smoother gain_, mix_;
};

MPC_PLUGIN_ENTRY(SilkOpto)
