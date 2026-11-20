// ForgeEQ.cpp - a console-style preamp and 3-band EQ for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// An original design in the spirit of classic British console channels: stepped
// frequencies, broad musical bands and a transformer-flavoured preamp. No circuit, name or
// artwork is copied from any product.
//
// Signal path (per channel, stereo):
//   in -> PREAMP: drive gain, "Iron" low-end emphasis + asymmetry, soft saturation
//         (run at 2x sample rate through a 63-tap half-band filter to keep aliasing down),
//         level compensation (Auto Gain: a slow loudness match so Drive changes tone, not volume)
//      -> HIGH-PASS 18 dB/oct (Off / 50 / 80 / 160 / 300 Hz)
//      -> LOW shelf (35 / 60 / 110 / 220 Hz, +-16 dB, slight resonant bump)
//      -> MID bell  (360 Hz .. 7.2 kHz, +-18 dB, Wide or Tight; narrows as you boost)
//      -> HIGH shelf (10 / 12 / 16 kHz "Air", +-16 dB)
//      -> Output trim, dry/wet Mix
//
// CPU: a handful of biquads, a polynomial waveshaper and ~66 multiply-adds of oversampling per
// sample and channel; coefficients are only recalculated when a control moves.
// Latency: 31 samples (reported to the host; the dry signal for Mix is delayed to match).

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/biquad.h"
#include "dsp/smoother.h"

using namespace mpc::dsp;

class ForgeEQ : public mpc::Plugin
{
public:
    enum Params { kOutput, kDrive, kIron, kHpf, kLowFreq, kLowGain, kMidFreq, kMidGain, kMidQ,
                  kHighFreq, kHighGain, kEqOn, kMix, kAutoGain, kNumParams };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Forge EQ";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'J', 'o', 'y');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        i.latencySamples = kLatency;
        return i;
    }

    ForgeEQ() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("out",   "Output", -24.0f, 12.0f, 0.0f, "dB", 1));
        addParam(floatParam("drive", "Drive", 0.0f, 30.0f, 6.0f, "dB", 1));
        addParam(floatParam("iron",  "Iron", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(choiceParam("hpf",  "High Pass", {"Off", "50 Hz", "80 Hz", "160 Hz", "300 Hz"}, 0));
        addParam(choiceParam("lowf", "Low Freq", {"35 Hz", "60 Hz", "110 Hz", "220 Hz"}, 1));
        addParam(floatParam("lowg",  "Low Gain", -16.0f, 16.0f, 0.0f, "dB", 1));
        addParam(choiceParam("midf", "Mid Freq", {"360 Hz", "700 Hz", "1.6 kHz", "3.2 kHz", "4.8 kHz", "7.2 kHz"}, 2));
        addParam(floatParam("midg",  "Mid Gain", -18.0f, 18.0f, 0.0f, "dB", 1));
        addParam(choiceParam("midq", "Mid Q", {"Wide", "Tight"}, 0));
        addParam(choiceParam("highf","High Freq", {"10 kHz", "12 kHz", "16 kHz"}, 1));
        addParam(floatParam("highg", "High Gain", -16.0f, 16.0f, 0.0f, "dB", 1));
        addParam(boolParam ("eqon",  "EQ", true));
        addParam(floatParam("mix",   "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(boolParam ("autogain", "Auto Gain", true));
        if (numParams() != kNumParams) mpc::log("ForgeEQ: parameter count mismatch");

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> o)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& p : o) v[(size_t)p.first] = p.second;
            addProgram(name, std::move(v));
        };
        program("Flat Warm", {});
        program("Kick Weight", {{kDrive, 10}, {kLowFreq, 1}, {kLowGain, 6}, {kMidFreq, 0}, {kMidGain, -5}, {kHighFreq, 0}, {kHighGain, 3}});
        program("Snare Crack", {{kDrive, 12}, {kHpf, 2}, {kLowFreq, 3}, {kLowGain, 3}, {kMidFreq, 4}, {kMidGain, 5}, {kHighGain, 2}});
        program("Vocal Forward", {{kDrive, 8}, {kHpf, 2}, {kLowFreq, 2}, {kLowGain, -3}, {kMidFreq, 3}, {kMidGain, 4}, {kHighFreq, 1}, {kHighGain, 4}});
        program("Bass DI Grit", {{kDrive, 22}, {kIron, 80}, {kHpf, 1}, {kLowFreq, 2}, {kLowGain, 4}, {kMidFreq, 1}, {kMidGain, 3}, {kOutput, -3}});
        program("Drum Bus Heat", {{kDrive, 16}, {kIron, 65}, {kLowFreq, 1}, {kLowGain, 3}, {kHighFreq, 1}, {kHighGain, 3}, {kMix, 70}});
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        dirty_ = true;
        outGain_.setTime(20.0f, sampleRate);
        driveGain_.setTime(20.0f, sampleRate);
        mix_.setTime(20.0f, sampleRate);
        // DC blocker after the asymmetric saturation (~8 Hz)
        dcR_ = 1.0f - (2.0f * 3.14159265f * 8.0f / (float)sampleRate);
        designHalfband();
        agCoef_ = 1.0f - std::exp(-1.0f / (0.4f * (float)sampleRate));   // ~400 ms loudness window
    }

    void reset() override
    {
        for (auto& c : ch_) c = Channel();
        dirty_ = true;
        outGain_.snap(dbToGain(get(kOutput)));
        driveGain_.snap(dbToGain(get(kDrive)));
        mix_.snap(get(kMix) * 0.01f);
        eIn_ = eOut_ = 0.0f; agGain_ = 1.0f;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        updateCoefficients();
        outGain_.setTarget(dbToGain(get(kOutput)));
        driveGain_.setTarget(dbToGain(get(kDrive)));
        mix_.setTarget(get(kMix) * 0.01f);
        const float iron = get(kIron) * 0.01f;
        const float bias = 0.25f * iron;              // asymmetry -> even harmonics
        const float fBias = shape(bias);
        const bool eqOn = getBool(kEqOn);
        const int nch = ctx.numOutputs < 2 ? ctx.numOutputs : 2;
        const bool autoGain = getBool(kAutoGain);

        for (int i = 0; i < ctx.numFrames; ++i)
        {
            const float dg = driveGain_.process();
            const float og = outGain_.process();
            const float mx = mix_.process();
            if (autoGain && ((agCount_++ & 31) == 0))
            {
                const float target = eOut_ > 1.0e-9f ? mpc::clamp(std::sqrt(eIn_ / eOut_), 0.5f, 8.0f) : agGain_;
                agGain_ += 0.05f * (target - agGain_);
            }
            if (!autoGain) agGain_ = 1.0f;
            for (int c = 0; c < nch; ++c)
            {
                Channel& s = ch_[c];
                const float in = ctx.inputs[c][i];    // read before writing (processing is in place)

                // PREAMP: emphasise lows before the saturator, undo after -> lows saturate first.
                // The saturator runs at twice the sample rate (half-band up -> shape -> half-band down).
                const float pre = s.ironPre.process(in) * dg * kHeadroom;
                float a, b;
                upsample(s, pre, a, b);
                a = shape(a + bias) - fBias;
                b = shape(b + bias) - fBias;
                float y = downsample(s, a, b);
                y = s.ironPost.process(y) / (dg * kHeadroom);
                if (autoGain)
                {
                    // compare loudness before and after the preamp (both channels together)
                    eIn_ += agCoef_ * (s.dry[(s.dryPos + 1) % (kLatency + 1)] * s.dry[(s.dryPos + 1) % (kLatency + 1)] - eIn_);
                    eOut_ += agCoef_ * (y * y - eOut_);
                    y *= agGain_;
                }
                // DC blocker
                const float dc = y - s.dcX + dcR_ * s.dcY; s.dcX = y; s.dcY = dc; y = dc;

                if (eqOn)
                {
                    if (hpfOn_) y = s.hp2.process(s.hp1.process(y));
                    y = s.low.process(y);
                    y = s.mid.process(y);
                    y = s.high.process(y);
                }
                y = sanitize(y) * og;
                // dry path delayed by the oversampler's latency so Mix does not comb-filter
                s.dry[s.dryPos] = in;
                const float dry = s.dry[(s.dryPos + 1) % (kLatency + 1)];
                s.dryPos = (s.dryPos + 1) % (kLatency + 1);
                ctx.outputs[c][i] = dry + (y - dry) * mx;
            }
        }
    }

private:
    static constexpr float kHeadroom = 0.5f;   // 0 dBFS at 0 dB drive sits in the gentle part of the curve
    static constexpr int kTaps = 63;           // half-band FIR: 32 even taps + the centre tap
    static constexpr int kHalf = 32;
    static constexpr int kOddDelay = 16;       // the centre tap sits 15.5 base samples back
    static constexpr int kLatency = 31;        // 31 (up) + 31 (down) samples at 2x = 31 at 1x

    struct Channel
    {
        Biquad ironPre, ironPost, hp1, hp2, low, mid, high;
        float dcX = 0.0f, dcY = 0.0f;
        float upHist[kHalf * 2] = {0};      // last kHalf inputs, stored twice for wrap-free reads
        int upPos = 0;
        float evHist[kHalf * 2] = {0};      // even-phase 2x samples, same trick
        int evPos = 0;
        float odHist[kOddDelay + 1] = {0};  // odd-phase 2x samples (only the centre tap needs them)
        int odPos = 0;
        float dry[kLatency + 1] = {0};
        int dryPos = 0;
    };

    // Soft saturation: y = u - 4/27 u^3 up to |u| = 1.5, then flat at +-1.
    static inline float shape(float u)
    {
        if (u > 1.5f) return 1.0f;
        if (u < -1.5f) return -1.0f;
        return u - (4.0f / 27.0f) * u * u * u;
    }
    // Kaiser-windowed sinc half-band low-pass (beta 8): flat to 18 kHz, -80 dB from 26 kHz at 44.1k.
    void designHalfband()
    {
        auto i0 = [](double x) { double sum = 1.0, term = 1.0; for (int k = 1; k < 40; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; } return sum; };
        const double beta = 8.0, mid = (kTaps - 1) * 0.5;
        double sum = 0.0;
        double h[kTaps];
        for (int n = 0; n < kTaps; ++n)
        {
            const double t = n - mid;
            const double sinc = t == 0.0 ? 0.5 : std::sin(3.14159265358979 * 0.5 * t) / (3.14159265358979 * t);
            const double r = t / mid;
            h[n] = sinc * i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0(beta);
            sum += h[n];
        }
        for (int j = 0; j < kHalf; ++j) hEven_[j] = (float)(h[2 * j] / sum);
        hCentre_ = (float)(h[kTaps / 2] / sum);
    }

    // One input sample -> two samples at 2x. Even output = even taps; odd output = centre tap only.
    inline void upsample(Channel& s, float x, float& even, float& odd) const
    {
        s.upPos = (s.upPos + kHalf - 1) % kHalf;
        s.upHist[s.upPos] = s.upHist[s.upPos + kHalf] = x;   // newest first
        const float* p = s.upHist + s.upPos;
        float acc = 0.0f;
        for (int j = 0; j < kHalf; ++j) acc += hEven_[j] * p[j];
        even = 2.0f * acc;
        odd = 2.0f * hCentre_ * p[15];
    }

    // Two samples at 2x -> one output sample.
    inline float downsample(Channel& s, float even, float odd) const
    {
        s.evPos = (s.evPos + kHalf - 1) % kHalf;
        s.evHist[s.evPos] = s.evHist[s.evPos + kHalf] = even;
        const float* p = s.evHist + s.evPos;
        float acc = 0.0f;
        for (int j = 0; j < kHalf; ++j) acc += hEven_[j] * p[j];
        // centre tap: the odd sample from 15.5 input samples ago -> 16 odd samples back
        s.odHist[s.odPos] = odd;
        s.odPos = (s.odPos + 1) % (kOddDelay + 1);
        return acc + hCentre_ * s.odHist[s.odPos];
    }

    void updateCoefficients()
    {
        const float vals[] = { get(kIron), (float)getInt(kHpf), (float)getInt(kLowFreq), get(kLowGain), (float)getInt(kMidFreq),
                               get(kMidGain), (float)getInt(kMidQ), (float)getInt(kHighFreq), get(kHighGain) };
        bool changed = dirty_;
        for (int k = 0; k < 9; ++k) if (vals[k] != last_[k]) { changed = true; last_[k] = vals[k]; }
        if (!changed) return;
        dirty_ = false;

        static const float hpfHz[]  = { 0.0f, 50.0f, 80.0f, 160.0f, 300.0f };
        static const float lowHz[]  = { 35.0f, 60.0f, 110.0f, 220.0f };
        static const float midHz[]  = { 360.0f, 700.0f, 1600.0f, 3200.0f, 4800.0f, 7200.0f };
        static const float highHz[] = { 10000.0f, 12000.0f, 16000.0f };

        const float ironDb = 6.0f * get(kIron) * 0.01f;
        const BiquadCoeffs pre  = BiquadCoeffs::lowShelf(sr_, 150.0f, 0.6f, ironDb);
        const BiquadCoeffs post = BiquadCoeffs::lowShelf(sr_, 150.0f, 0.6f, -ironDb);

        const int h = mpc::clamp(getInt(kHpf), 0, 4);
        hpfOn_ = h > 0;
        // 3rd-order Butterworth high-pass = 2nd order (Q 1.0) + 1st order (as a Q-0.5 biquad pair below)
        const BiquadCoeffs hp2 = h > 0 ? BiquadCoeffs::highpass(sr_, hpfHz[h], 1.0f) : BiquadCoeffs::identity();
        const BiquadCoeffs hp1 = h > 0 ? firstOrderHighpass(hpfHz[h]) : BiquadCoeffs::identity();

        const float lg = get(kLowGain), mg = get(kMidGain), hg = get(kHighGain);
        const BiquadCoeffs low = BiquadCoeffs::lowShelf(sr_, lowHz[mpc::clamp(getInt(kLowFreq), 0, 3)], 0.9f, lg);
        // The bell narrows as you boost or cut harder: gentle at small settings, focused at big ones.
        const float q = (getInt(kMidQ) == 0 ? 0.6f : 1.4f) * (1.0f + std::fabs(mg) / 18.0f);
        const BiquadCoeffs mid = BiquadCoeffs::peak(sr_, midHz[mpc::clamp(getInt(kMidFreq), 0, 5)], q, mg);
        const BiquadCoeffs high = BiquadCoeffs::highShelf(sr_, highHz[mpc::clamp(getInt(kHighFreq), 0, 2)], 0.7f, hg);

        for (auto& c : ch_)
        {
            c.ironPre.setCoeffs(pre); c.ironPost.setCoeffs(post);
            c.hp1.setCoeffs(hp1); c.hp2.setCoeffs(hp2);
            c.low.setCoeffs(low); c.mid.setCoeffs(mid); c.high.setCoeffs(high);
        }
    }

    BiquadCoeffs firstOrderHighpass(float hz) const
    {
        // bilinear transform of s / (s + w)
        const float k = std::tan(3.14159265f * mpc::clamp(hz, 1.0f, 0.45f * (float)sr_) / (float)sr_);
        BiquadCoeffs c;
        const float n = 1.0f / (1.0f + k);
        c.b0 = n; c.b1 = -n; c.b2 = 0.0f; c.a1 = (k - 1.0f) * n; c.a2 = 0.0f;
        return c;
    }

    double sr_ = 44100.0;
    Channel ch_[2];
    Smoother outGain_, driveGain_, mix_;
    float dcR_ = 0.999f;
    bool hpfOn_ = false, dirty_ = true;
    float last_[9] = {0};
    float hEven_[kHalf] = {0};
    float hCentre_ = 0.5f;
    float eIn_ = 0.0f, eOut_ = 0.0f, agGain_ = 1.0f, agCoef_ = 0.0001f;
    unsigned agCount_ = 0;
};

MPC_PLUGIN_ENTRY(ForgeEQ)
