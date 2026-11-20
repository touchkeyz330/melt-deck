// MeltStutter.cpp - a tempo-synced stutter / beat-repeat effect for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// Original design. The input is always being recorded into a ring buffer. When the stutter is
// engaged (the Stutter switch - put it on a Q-Link or automate it - or Auto mode), it waits for
// the next grid point (Quantize), grabs a slice one Rate long from that point, and repeats it:
//   * Gate     plays only the first part of each repeat (choppy "trance gate" stutters)
//   * Roll     each repeat gets shorter (Faster: the classic snare-roll build) or longer
//   * Pitch    each repeat drops or rises by this many semitones (tape-stop dives, risers)
//   * Fade     each repeat is quieter than the last
//   * Reverse  repeats after the first play backwards
// Every repeat has 1.5 ms fades at its edges and engaging/releasing crossfades with the dry
// signal, so nothing clicks. Tempo and grid come from the MPC's transport (90 BPM free-running
// grid when it isn't playing).

#include "mpc_plugin.h"
#include "dsp/math.h"

#include <algorithm>
#include <cmath>

using namespace mpc::dsp;

class MeltStutter : public mpc::Plugin
{
public:
    enum Params { kStutter = 0, kRate, kGate, kRoll, kPitch, kFade, kReverse, kQuant, kMix, kAuto, kChance, kAutoLen, kNumParams };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt Stutter";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'X', 'B', 'C');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        return i;
    }

    MeltStutter() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(boolParam("on", "Stutter", false));
        addParam(choiceParam("rate", "Rate", {"1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/4T", "1/8T", "1/16T", "1/32T"}, 3));
        addParam(floatParam("gate", "Gate", 10.0f, 100.0f, 100.0f, "%", 0));
        addParam(choiceParam("roll", "Roll", {"Off", "Faster", "Slower"}, 0));
        addParam(floatParam("pitch", "Pitch", -12.0f, 12.0f, 0.0f, "st", 1));
        addParam(floatParam("fade", "Fade", 0.0f, 50.0f, 0.0f, "%", 0));
        addParam(boolParam("rev", "Reverse", false));
        addParam(choiceParam("quant", "Quantize", {"Off", "1/16", "1/8", "1/4", "Bar"}, 1));
        addParam(floatParam("mix", "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(boolParam("auto", "Auto", false));
        addParam(floatParam("chance", "Chance", 0.0f, 100.0f, 25.0f, "%", 0));
        addParam(choiceParam("alen", "Auto Len", {"1/8", "1/4", "1/2", "1 Bar"}, 1));
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        ringLen_ = 1;
        while (ringLen_ < (int)(sampleRate * 8.0)) ringLen_ <<= 1;   // 8+ seconds
        bufL_.assign((size_t)ringLen_, 0.0f);
        bufR_.assign((size_t)ringLen_, 0.0f);
        edge_ = std::max(8, (int)(0.0015 * sampleRate));
        reset();
    }

    void reset() override
    {
        std::fill(bufL_.begin(), bufL_.end(), 0.0f);
        std::fill(bufR_.begin(), bufR_.end(), 0.0f);
        w_ = 0; state_ = Idle; wet_ = 0.0f; freePpq_ = 0.0; lastGridIdx_ = -1; lastAutoIdx_ = -1;
        autoUntil_ = -1.0; rng_ = 0xC0FFEEu;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        static const double rateBeats[10] = {2.0, 1.0, 0.5, 0.25, 0.125, 0.0625, 2.0 / 3, 1.0 / 3, 1.0 / 6, 1.0 / 12};
        static const double quantBeats[5] = {0.0, 0.25, 0.5, 1.0, 4.0};
        static const double autoLenBeats[4] = {0.5, 1.0, 2.0, 4.0};
        const double bpm = ctx.transport.tempoValid && ctx.transport.bpm > 20.0 ? ctx.transport.bpm : 90.0;
        const double spb = sr_ * 60.0 / bpm;                       // samples per beat
        const bool hostClock = ctx.transport.playing && ctx.transport.ppqValid;
        double ppq = hostClock ? ctx.transport.ppqPos : freePpq_;
        const double dppq = 1.0 / spb;

        const bool button = getBool(kStutter);
        const bool autoOn = getBool(kAuto);
        const float chance = get(kChance) * 0.01f;
        const double grid = quantBeats[mpc::clamp(getInt(kQuant), 0, 4)];
        const double autoGrid = 1.0;                                // Auto rolls the dice every beat
        rate_ = rateBeats[mpc::clamp(getInt(kRate), 0, 9)] * spb;
        gate_ = get(kGate) * 0.01f;
        roll_ = getInt(kRoll);
        pitchStep_ = get(kPitch);
        fade_ = 1.0f - get(kFade) * 0.01f;
        reverse_ = getBool(kReverse);
        const float mix = get(kMix) * 0.01f;
        const double autoLen = autoLenBeats[mpc::clamp(getInt(kAutoLen), 0, 3)];
        const float xfade = 1.0f - std::exp(-1.0f / (0.004f * (float)sr_));   // 4 ms dry/wet crossfade

        const int n = ctx.numFrames;
        const float* inL = ctx.inputs && ctx.numInputs > 0 ? ctx.inputs[0] : nullptr;
        const float* inR = ctx.inputs && ctx.numInputs > 1 ? ctx.inputs[1] : inL;
        float* oL = ctx.outputs[0];
        float* oR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        for (int i = 0; i < n; ++i)
        {
            const float xl = inL ? inL[i] : 0.0f, xr = inR ? inR[i] : xl;
            bufL_[(size_t)(w_ & (ringLen_ - 1))] = xl;
            bufR_[(size_t)(w_ & (ringLen_ - 1))] = xr;

            // ---- grid events
            const long long gIdx = grid > 0.0 ? (long long)std::floor(ppq / grid + 1e-9) : -2;
            const bool onGrid = grid <= 0.0 || gIdx != lastGridIdx_;
            lastGridIdx_ = gIdx;
            const long long aIdx = (long long)std::floor(ppq / autoGrid + 1e-9);
            if (autoOn && aIdx != lastAutoIdx_ && autoUntil_ < ppq)
            {
                if (rnd() < chance) autoUntil_ = ppq + autoLen;    // roll the dice on every beat
            }
            lastAutoIdx_ = aIdx;
            const bool want = button || (autoOn && ppq < autoUntil_);

            // ---- state machine: Idle -> (want) Armed -> (grid) Repeating -> (!want) Idle
            if (state_ == Idle && want) state_ = Armed;
            if (state_ == Armed && !want) state_ = Idle;
            if (state_ == Armed && onGrid) start();
            if (state_ == Repeating && !want) state_ = Idle;

            float yl = xl, yr = xr;
            float target = 0.0f;
            if (state_ == Repeating)
            {
                target = 1.0f;
                float sl, sr;
                readRepeat(sl, sr);
                yl = sl; yr = sr;
            }
            wet_ += xfade * (target * mix - wet_);
            if (target == 0.0f && wet_ < 1.0e-4f) wet_ = 0.0f;   // fully dry again: bit-exact pass-through
            if (state_ != Repeating && wet_ > 1.0e-4f)
            {
                // releasing: keep the last repeat sounding under the crossfade
                float sl, sr;
                readRepeat(sl, sr);
                yl = sl; yr = sr;
            }
            const float ol = xl + wet_ * (yl - xl), orr = xr + wet_ * (yr - xr);
            oL[i] = sanitize(ol);
            if (oR) oR[i] = sanitize(orr);
            ++w_;
            ppq += dppq;
        }
        freePpq_ = hostClock ? 0.0 : ppq;
    }

private:
    enum State { Idle, Armed, Repeating };

    float rnd() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return (float)(rng_ >> 8) / 16777216.0f; }

    void start()
    {
        state_ = Repeating;
        sliceStart_ = w_;      // this sample is the first of the slice
        len_ = rate_;
        k_ = 0;
        phase_ = 0.0;
        speed_ = 1.0;
        gain_ = 1.0f;
    }

    // one output sample of the repeating slice; advances the repeat clock
    void readRepeat(float& l, float& r)
    {
        const double L = std::max(16.0, len_);
        const double gated = L * gate_;
        // position inside the slice (source samples)
        double p = phase_ * speed_;
        const bool rev = reverse_ && k_ > 0;
        double src = rev ? (L - 1.0 - p) : p;
        const bool outside = src < 0.0 || src >= L - 1.0;   // pitched up/down past the slice
        // the first repeat is the live audio itself: never read ahead of what is recorded
        const double maxSrc = (double)(w_ - sliceStart_);
        if (src > maxSrc) src = maxSrc;
        if (src < 0.0) src = 0.0;
        const double a = (double)sliceStart_ + src;
        const long long i0 = (long long)std::floor(a);
        const float f = (float)(a - (double)i0);
        const size_t m = (size_t)(ringLen_ - 1);
        const float l0 = bufL_[(size_t)i0 & m], l1 = bufL_[(size_t)(i0 + 1) & m];
        const float r0 = bufR_[(size_t)i0 & m], r1 = bufR_[(size_t)(i0 + 1) & m];
        l = l0 + f * (l1 - l0); r = r0 + f * (r1 - r0);
        // edges: 1.5 ms fade in/out of each repeat (inside the gate), silence after the gate
        float env;
        if (phase_ >= gated) env = 0.0f;
        else
        {
            const double toEnd = gated - phase_;
            env = (float)std::min(1.0, std::min(phase_ / edge_, toEnd / edge_));
            if (k_ == 0) env = (float)std::min(1.0, toEnd / edge_);   // first repeat starts in the music: no fade-in
        }
        if (outside) env = 0.0f;
        env *= gain_;
        l *= env; r *= env;
        // advance
        phase_ += 1.0;
        if (phase_ >= L)
        {
            phase_ = 0.0;
            ++k_;
            gain_ *= fade_;
            speed_ = std::pow(2.0, (double)pitchStep_ * k_ / 12.0);
            if (roll_ == 1) len_ = std::max(len_ * 0.75, sr_ * 0.0125);          // faster, down to ~12 ms
            else if (roll_ == 2) len_ = std::min(len_ * 1.333, sr_ * 2.0);       // slower, up to 2 s
        }
    }

    double sr_ = 44100.0;
    int ringLen_ = 1;
    std::vector<float> bufL_, bufR_;
    long long w_ = 0, sliceStart_ = 0, lastGridIdx_ = -1, lastAutoIdx_ = -1;
    State state_ = Idle;
    double rate_ = 11025, len_ = 11025, phase_ = 0.0, speed_ = 1.0, freePpq_ = 0.0, autoUntil_ = -1.0, edge_ = 66;
    int k_ = 0, roll_ = 0;
    float gate_ = 1.0f, pitchStep_ = 0.0f, fade_ = 1.0f, gain_ = 1.0f, wet_ = 0.0f;
    bool reverse_ = false;
    uint32_t rng_ = 0xC0FFEEu;
};

MPC_PLUGIN_ENTRY(MeltStutter)
