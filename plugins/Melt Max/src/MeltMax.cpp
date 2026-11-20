// MeltMax.cpp - a look-ahead brickwall mastering limiter for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// Original design. Turn Threshold down and the mix gets louder while nothing ever passes the
// Ceiling:
//   gain needed per sample = Ceiling / peak, where the peak is taken from the input x
//   (-Threshold) and, with True Peak on, from a 4x interpolated copy (+0.5 dB margin), so peaks BETWEEN samples
//   (the ones that clip converters and lossy encoders) are caught too;
//   -> minimum over the look-ahead window -> moving average over the same window
//      (the gain glides down and reaches its target exactly at the peak: no overs, no clicks)
//   -> release: manual, or Auto: fast recovery from short peaks, slow from sustained limiting
//      (no pumping on dense mixes, no dulling of drums)
//   -> stereo-linked gain -> final safety clamp at the Ceiling -> optional dither to 24 or 16 bit
//      (TPDF, with optional noise shaping that moves the hiss up where ears are least sensitive).
// Latency: look-ahead 1.5 ms + 11 samples (true-peak interpolator), reported to the host.

#include "mpc_plugin.h"
#include "dsp/math.h"

#include <algorithm>
#include <cmath>

using namespace mpc::dsp;

class MeltMax : public mpc::Plugin
{
public:
    enum Params { kThreshold = 0, kCeiling, kRelease, kAuto, kTruePeak, kDither, kReduction, kNumParams };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt Max";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'R', 'B', 'y');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        i.latencySamples = 66 + kInterpDelay - 1;      // updated in prepare() for the real rate
        return i;
    }

    MeltMax() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("thresh", "Threshold", -30.0f, 0.0f, -3.0f, "dB", 1));
        addParam(floatParam("ceil", "Ceiling", -12.0f, 0.0f, -0.3f, "dB", 1));
        addParam(logParam("rel", "Release", 1.0f, 1000.0f, 80.0f, "ms", 0));
        addParam(boolParam("auto", "Auto Rel", true));
        addParam(boolParam("tp", "True Peak", true));
        addParam(choiceParam("dither", "Dither", {"Off", "24-bit", "16-bit", "16 Shaped"}, 0));
        Param gr = floatParam("gr", "Reduction", -30.0f, 0.0f, 0.0f, "dB", 1);
        gr.automatable = false;
        addParam(gr);
        // 4x interpolator for true-peak detection: 96-tap Kaiser-windowed full-band sinc (24 taps
        // per phase). Against 16x oversampling it under-reads by at most ~0.37 dB on very harsh
        // material (clipped noise, hats up to Nyquist), so detection adds a 0.5 dB margin.
        auto i0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 40; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        const double beta = 6.0, fc = 1.0, c = 47.5;
        for (int n = 0; n < 96; ++n)
        {
            const double x = (n - c) / 4.0 * fc;
            const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(3.14159265358979 * x) / (3.14159265358979 * x);
            const double r = (n - c) / c;
            fir_[n % 4][n / 4] = (float)(fc * sinc * i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0(beta));
        }
        for (int p = 0; p < 4; ++p) { float s = 0; for (int k = 0; k < 24; ++k) s += fir_[p][k]; for (int k = 0; k < 24; ++k) fir_[p][k] /= s; }
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        look_ = std::min(kMaxLook - 1, std::max(16, (int)std::lround(0.0015 * sampleRate)));   // 1.5 ms
        setLatency(look_ + kInterpDelay - 1);
        fastRel_ = coef(0.012f);   // auto: quick recovery from short peaks
        slowAtk_ = coef(0.080f);   // auto: average gain reduction follows the music
        slowRel_ = coef(0.600f);
        meterRel_ = std::exp(-1.0f / (0.300f * (float)sampleRate));
        slowUp_ = coef(0.250f);
        reset();
    }

    void reset() override
    {
        std::fill(std::begin(dly_[0]), std::end(dly_[0]), 0.0f);
        std::fill(std::begin(dly_[1]), std::end(dly_[1]), 0.0f);
        std::fill(std::begin(hist_[0]), std::end(hist_[0]), 0.0f);
        std::fill(std::begin(hist_[1]), std::end(hist_[1]), 0.0f);
        std::fill(std::begin(req_), std::end(req_), 1.0f);
        std::fill(std::begin(minq_), std::end(minq_), 0LL);
        qHead_ = qTail_ = 0;
        std::fill(std::begin(boxBuf_), std::end(boxBuf_), 1.0f);
        boxSum_ = (double)look_;
        pos_ = hpos_ = boxPos_ = 0; t_ = 0;
        g_ = slow_ = 1.0f; meter_ = 0.0f; prevPk_ = 0.0f;
        // after a project/preset load (which calls reset) leave the restored meter value alone
        // for half a second, so the saved state reads back exactly
        meterHold_ = (int)(0.5 * sr_);
        err_[0][0] = err_[0][1] = err_[1][0] = err_[1][1] = 0.0f;
        rng_ = 0x12345678u;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const float inGain = dbToGain(-get(kThreshold));
        const float ceil = dbToGain(get(kCeiling));
        const bool autoRel = getBool(kAuto), tp = getBool(kTruePeak);
        const float relC = coef(get(kRelease) * 0.001f);
        const int dither = getInt(kDither);
        const int n = ctx.numFrames;
        const float* inL = ctx.inputs && ctx.numInputs > 0 ? ctx.inputs[0] : nullptr;
        const float* inR = ctx.inputs && ctx.numInputs > 1 ? ctx.inputs[1] : inL;
        float* oL = ctx.outputs[0];
        float* oR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        float blockMinG = 1.0f;

        for (int i = 0; i < n; ++i)
        {
            const float xl = (inL ? inL[i] : 0.0f) * inGain;
            const float xs[2] = {xl, inR ? inR[i] * inGain : xl};
            // ---- detector: sample peak, or true peak from the 4x interpolation (4-sample delay)
            float pk = 0.0f;
            for (int c = 0; c < 2; ++c)
            {
                hist_[c][hpos_] = xs[c];
                if (tp)
                {
                    for (int p = 0; p < 4; ++p)
                    {
                        float acc = 0.0f;
                        for (int k = 0; k < 24; ++k) acc += fir_[p][k] * hist_[c][(hpos_ - k + 32) & 31];
                        pk = std::max(pk, std::fabs(acc) * kTpMargin);
                    }
                }
                pk = std::max(pk, std::fabs(hist_[c][(hpos_ - kInterpDelay + 32) & 31]));
                pk = std::max(pk, std::fabs(hist_[c][(hpos_ - kInterpDelay + 1 + 32) & 31]));
            }
            hpos_ = (hpos_ + 1) & 31;
            // a peak between two samples needs both of them turned down: hold each peak for two
            const float pk2 = std::max(pk, prevPk_);
            prevPk_ = pk;
            const float need = pk2 > ceil ? ceil / pk2 : 1.0f;

            // ---- minimum of the needed gain over the look-ahead window (monotonic queue)
            req_[t_ & (kMaxLook - 1)] = need;
            while (qTail_ > qHead_ && req_[minq_[(qTail_ - 1) & (kMaxLook - 1)] & (kMaxLook - 1)] >= need) --qTail_;
            minq_[qTail_ & (kMaxLook - 1)] = t_;
            ++qTail_;
            while (t_ - minq_[qHead_ & (kMaxLook - 1)] >= look_) ++qHead_;
            const float wmin = req_[minq_[qHead_ & (kMaxLook - 1)] & (kMaxLook - 1)];
            ++t_;
            // ---- moving average over the window: the gain glides down and lands on the peak
            boxSum_ += (double)wmin - (double)boxBuf_[boxPos_];
            boxBuf_[boxPos_] = wmin;
            boxPos_ = boxPos_ + 1 >= look_ ? 0 : boxPos_ + 1;
            if (boxPos_ == 0) { double s = 0; for (int k = 0; k < look_; ++k) s += boxBuf_[k]; boxSum_ = s; }   // no drift
            const float target = std::min(1.0f, (float)(boxSum_ / (double)look_));

            // ---- release
            if (target < g_) g_ = target;
            else if (autoRel)
            {
                // fast back up to the average reduction (short peaks), then slowly from there
                if (g_ < slow_) g_ += (std::min(target, slow_) - g_) * fastRel_;
                else g_ += (target - g_) * slowUp_;
            }
            else g_ += (target - g_) * relC;
            g_ = std::min(g_, target);
            slow_ += (target < slow_ ? slowAtk_ : slowRel_) * (target - slow_);
            blockMinG = std::min(blockMinG, g_);

            // ---- apply to the delayed signal (look-ahead + interpolator delay)
            const int d = look_ + kInterpDelay - 1;   // the window and the average both cover this sample
            float y[2];
            for (int c = 0; c < 2; ++c)
            {
                dly_[c][pos_] = xs[c];
                float v = dly_[c][(pos_ - d + kDly) & (kDly - 1)] * g_;
                v = std::max(-ceil, std::min(ceil, v));   // safety: never above the ceiling
                y[c] = v;
            }
            pos_ = (pos_ + 1) & (kDly - 1);
            if (dither > 0) ditherStereo(y, dither, ceil);
            oL[i] = sanitize(y[0]);
            if (oR) oR[i] = sanitize(y[1]);
        }
        // meter: fast down, slower back up
        const float grDb = 20.0f * std::log10(std::max(blockMinG, 1.0e-4f));
        meter_ = grDb < meter_ ? grDb : meter_ + (grDb - meter_) * (1.0f - std::pow(meterRel_, (float)n));
        if (meterHold_ > 0) meterHold_ -= n;
        else set(kReduction, std::max(-30.0f, meter_));
    }

private:
    static constexpr int kMaxLook = 512;    // power of two
    static constexpr int kDly = 1024;       // power of two
    static constexpr int kInterpDelay = 12;
    static constexpr float kTpMargin = 1.0593f;   // +0.5 dB

    float coef(float seconds) const { return 1.0f - std::exp(-1.0f / (std::max(1.0e-4f, seconds) * (float)sr_)); }

    float rnd() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return (float)(rng_ >> 8) / 16777216.0f; }   // 0..1

    // TPDF dither + quantise; "16 Shaped" feeds the quantisation error back (2nd order) so the
    // noise sits mostly above ~10 kHz.
    void ditherStereo(float* y, int mode, float ceil)
    {
        const float q = mode == 1 ? 1.0f / 8388608.0f : 1.0f / 32768.0f;   // one LSB
        for (int c = 0; c < 2; ++c)
        {
            float v = y[c];
            if (mode == 3) v -= 1.5f * err_[c][0] - 0.6f * err_[c][1];
            const float d = (rnd() - rnd()) * q;                               // TPDF, +-1 LSB
            float out = std::round((v + d) / q) * q;
            const float top = std::floor(ceil / q) * q;              // the ceiling, on the grid
            out = std::max(-top, std::min(top, out));
            if (mode == 3) { err_[c][1] = err_[c][0]; err_[c][0] = out - v; }
            y[c] = out;
        }
    }

    double sr_ = 44100.0;
    int look_ = 66;
    float fir_[4][24] = {};
    float hist_[2][32] = {};
    int hpos_ = 0;
    float req_[kMaxLook] = {};
    long long minq_[kMaxLook] = {};
    long long qHead_ = 0, qTail_ = 0, t_ = 0;
    float boxBuf_[kMaxLook] = {};
    double boxSum_ = 0.0;
    int boxPos_ = 0;
    float dly_[2][kDly] = {};
    int pos_ = 0;
    float g_ = 1.0f, slow_ = 1.0f, meter_ = 0.0f;
    int meterHold_ = 0;
    float prevPk_ = 0.0f;
    float slowUp_ = 0.0001f, fastRel_ = 0.01f, slowAtk_ = 0.001f, slowRel_ = 0.0001f, meterRel_ = 0.999f;
    float err_[2][2] = {};
    uint32_t rng_ = 0x12345678u;
};

MPC_PLUGIN_ENTRY(MeltMax)
