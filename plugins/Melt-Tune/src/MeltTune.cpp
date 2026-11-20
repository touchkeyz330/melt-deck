// MeltTune.cpp - real-time vocal pitch correction for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// Original design. Signal flow:
//   DETECT: (L+R)/2 -> 4th-order low-pass -> /4 decimation -> YIN pitch detector every ~6 ms
//           (windowed difference function, cumulative-mean normalisation, parabolic refinement),
//           level gate, median-of-3 smoothing
//   DECIDE: nearest allowed note in Key/Scale (with hysteresis so it does not flicker between
//           notes), correction = Amount x (target - sung) + Transpose; Retune sets how fast the
//           correction moves (0 ms = instant, the hard "robot" effect)
//   SHIFT:  pitch-synchronous overlap-add (PSOLA): grains two sung periods long (Hann window)
//           are cut from the input one whole period apart, so their waveforms line up, and laid
//           down every period/ratio. Same grains on both channels, so stereo is kept.
//   OUT:    Mix with dry, Output gain. "Pitch" shows the note being sung.
//
// Latency: about two sung periods plus 1 ms (roughly 6-25 ms depending on the voice). Not
// reported to the host because it follows the pitch.
// CPU: detection runs at a quarter of the sample rate; the shifter mixes a few grains a sample.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/biquad.h"
#include "dsp/smoother.h"

#include <algorithm>

using namespace mpc::dsp;

class MeltTune : public mpc::Plugin
{
public:
    enum Params { kOutput, kKey, kScale, kRetune, kAmount, kTranspose, kRange, kMix, kPitch, kNumParams };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt-Tune";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 's', 's', 'A');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        return i;
    }

    MeltTune() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("out",    "Output", -24.0f, 12.0f, 0.0f, "dB", 1));
        addParam(choiceParam("key",   "Key", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0));
        addParam(choiceParam("scale", "Scale", {"Chromatic", "Major", "Minor", "Harm Minor", "Pent Major", "Pent Minor", "Blues", "Dorian"}, 0));
        addParam(floatParam("retune", "Retune", 0.0f, 400.0f, 25.0f, "ms", 0, 0.4f));
        addParam(floatParam("amount", "Amount", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(intParam  ("transp", "Transpose", -12, 12, 0, "st"));
        addParam(choiceParam("range", "Voice", {"Low", "Mid", "High"}, 1));
        addParam(floatParam("mix",    "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
        Param pitch = floatParam("pitch", "Pitch", 0.0f, 127.0f, 0.0f, "", 2);   // display only
        pitch.automatable = false;
        addParam(pitch);
        if (numParams() != kNumParams) mpc::log("MeltTune: parameter count mismatch");

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> o)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& p : o) v[(size_t)p.first] = p.second;
            addProgram(name, std::move(v));
        };
        program("Pop Polish", {});
        program("Natural", {{kRetune, 80}, {kAmount, 80}});
        program("Hard Tune", {{kRetune, 0}, {kAmount, 100}});
        program("Hard Tune Minor", {{kRetune, 0}, {kScale, 2}, {kKey, 9}});
        program("Octave Down", {{kAmount, 0}, {kTranspose, -12}});
        program("Octave Up", {{kAmount, 0}, {kTranspose, 12}, {kRange, 2}});
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        if (index != kPitch) return false;
        if (plain < 1.0f) { out = "--"; return true; }
        static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        const int n = (int)std::lround(plain);
        const int cents = (int)std::lround((plain - (float)n) * 100.0f);
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%s%d %+dc", names[((n % 12) + 12) % 12], n / 12 - 1, cents);
        out = buf;
        return true;
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        dsr_ = sampleRate / kDecim;
        const BiquadCoeffs a = BiquadCoeffs::lowpass(sampleRate, (float)(0.2 * dsr_), 0.5412f);   // 4th-order Butterworth
        const BiquadCoeffs b = BiquadCoeffs::lowpass(sampleRate, (float)(0.2 * dsr_), 1.3066f);
        aa1_.setCoeffs(a); aa2_.setCoeffs(b);
        mix_.setTime(20.0f, sampleRate);
        out_.setTime(20.0f, sampleRate);
        for (int i = 0; i < kWinTable; ++i) hann_[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * (float)i / (float)kWinTable);
        hann_[kWinTable] = hann_[0];
        margin_ = 0.001 * sampleRate;
        hopSec_ = (float)(kHop * kDecim / sampleRate);
    }

    void reset() override
    {
        aa1_.reset(); aa2_.reset();
        std::fill(std::begin(dec_), std::end(dec_), 0.0f);
        std::fill(std::begin(bufL_), std::end(bufL_), 0.0f);
        std::fill(std::begin(bufR_), std::end(bufR_), 0.0f);
        decPos_ = decCount_ = hopCount_ = 0;
        for (auto& g : grains_) g = Grain();
        now_ = 4096; untilGrain_ = 0.0; lastCentre_ = 0.0;
        f0_ = 0.0f; hist_[0] = hist_[1] = hist_[2] = 0.0f; skipped_ = false;
        voiced_ = false; targetNote_ = -1000; shift_ = 0.0f; ratio_ = 1.0f; ctl_ = 0;
        level_ = 0.0f;
        mix_.snap(get(kMix) * 0.01f);
        out_.snap(dbToGain(get(kOutput)));
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int range = mpc::clamp(getInt(kRange), 0, 2);
        static const float fmin[] = { 60.0f, 80.0f, 150.0f }, fmax[] = { 500.0f, 900.0f, 1500.0f };
        const float retuneMs = get(kRetune);
        const float retuneCoef = retuneMs <= 0.5f ? 1.0f : 1.0f - std::exp(-1000.0f * (float)kCtl / (retuneMs * (float)sr_));
        const float amount = get(kAmount) * 0.01f;
        const float transpose = (float)getInt(kTranspose);
        const int key = getInt(kKey), scale = getInt(kScale);
        mix_.setTarget(get(kMix) * 0.01f);
        out_.setTarget(dbToGain(get(kOutput)));
        const int nch = ctx.numOutputs < 2 ? ctx.numOutputs : 2;

        for (int i = 0; i < ctx.numFrames; ++i)
        {
            const float inL = ctx.inputs[0][i];
            const float inR = nch > 1 ? ctx.inputs[1][i] : inL;
            const float m = 0.5f * (inL + inR);

            // ---- detection path (decimated) ----
            const float lp = aa2_.process(aa1_.process(m));
            if (++decCount_ >= kDecim)
            {
                decCount_ = 0;
                dec_[decPos_] = lp;
                decPos_ = (decPos_ + 1) & (kDecBuf - 1);
                level_ += 0.01f * (lp * lp - level_);
                if (++hopCount_ >= kHop) { hopCount_ = 0; detect(fmin[range], fmax[range]); decide(key, scale); }
            }

            // ---- control rate: move the correction toward its target ----
            if (ctl_ == 0)
            {
                float want = transpose;
                if (voiced_ && targetNote_ > -1000) want += amount * ((float)targetNote_ - sungNote_);
                shift_ += retuneCoef * (want - shift_);
                ratio_ = semitonesToRatio(mpc::clamp(shift_, -24.0f, 24.0f));
            }
            ctl_ = (ctl_ + 1) % kCtl;

            // ---- pitch shifter (PSOLA) ----
            bufL_[now_ & (kBuf - 1)] = inL; bufR_[now_ & (kBuf - 1)] = inR;
            if (--untilGrain_ <= 0.0) spawnGrain();
            float wl = 0.0f, wrr = 0.0f;
            for (int gI = 0; gI < kGrains; ++gI)
            {
                Grain& gr = grains_[gI];
                if (gr.len <= 0.0) continue;
                const float w = win((float)(gr.pos / gr.len)) * gr.gain;
                const double src = gr.start + gr.pos;
                const int64_t k = (int64_t)std::floor(src);
                const float f = (float)(src - (double)k);
                const float l0 = bufL_[k & (kBuf - 1)], l1 = bufL_[(k + 1) & (kBuf - 1)];
                wl += w * (l0 + f * (l1 - l0));
                if (nch > 1) { const float r0 = bufR_[k & (kBuf - 1)], r1 = bufR_[(k + 1) & (kBuf - 1)]; wrr += w * (r0 + f * (r1 - r0)); }
                gr.pos += 1.0;
                if (gr.pos >= gr.len) gr.len = 0.0;
            }
            if (nch < 2) wrr = wl;
            ++now_;

            const float mx = mix_.process(), g = out_.process();
            ctx.outputs[0][i] = sanitize((inL + (wl - inL) * mx) * g);
            if (nch > 1) ctx.outputs[1][i] = sanitize((inR + (wrr - inR) * mx) * g);
        }
        if ((meter_++ & 15) == 0) set(kPitch, voiced_ ? mpc::clamp(sungNote_, 1.0f, 127.0f) : 0.0f);
    }

private:
    static constexpr int kDecim = 4;
    static constexpr int kDecBuf = 1024;     // decimated history (power of two)
    static constexpr int kDetWin = 384;      // YIN window at the decimated rate (~35 ms at 44.1 kHz)
    static constexpr int kHop = 64;          // detect every 64 decimated samples (~6 ms)
    static constexpr int kBuf = 8192;        // shifter delay buffer (power of two)
    static constexpr int kCtl = 32;
    static constexpr int kWinTable = 1024;

    inline float win(float p) const
    {
        const float x = p * (float)kWinTable;
        const int k = (int)x;
        return hann_[k] + (x - (float)k) * (hann_[k + 1] - hann_[k]);
    }

    // Start a grain: two periods of input, centred one whole number of periods from the last one.
    void spawnGrain()
    {
        const double P = mpc::clamp(voiced_ && f0_ > 0.0f ? sr_ / (double)f0_ : 0.006 * sr_, 0.0015 * sr_, 0.02 * sr_);
        const double r = mpc::clamp((double)ratio_, 0.25, 4.0);
        const double S = P / r;                                       // output spacing between grains
        const double L = 2.0 * std::max(P, S);                        // longer grains when shifting down: no gaps
        const double target = (double)now_ - 0.5 * L - margin_ - 2.0;       // newest centre we can read a whole grain around
        double c = lastCentre_ + P * std::round((target - lastCentre_) / P);
        if (c > target) c -= P;
        if (c < target - 4.0 * P - L || c > target) c = target;          // lost track (silence, big jump): resync
        lastCentre_ = c;
        for (int gI = 0; gI < kGrains; ++gI)
            if (grains_[gI].len <= 0.0)
            {
                // The grain was due 'late' samples ago (between output samples): start it that far in,
                // so grains land on their exact fractional times and overlap in phase.
                const double late = -untilGrain_;
                Grain& gr = grains_[gI];
                gr.start = c - 0.5 * L;
                gr.len = L;
                gr.pos = late;
                gr.gain = (float)(2.0 * S / L);                       // Hann grains of length L spaced S sum to L/(2S)
                break;
            }
        untilGrain_ += S;
    }

    // YIN on the most recent decimated samples.
    void detect(float fmin, float fmax)
    {
        const int tauMax = std::min(kDecBuf - kDetWin - 2, (int)(dsr_ / fmin));
        const int tauMin = std::max(2, (int)(dsr_ / fmax));
        // copy the newest kDetWin + tauMax samples into a straight buffer, oldest first
        const int n = kDetWin + tauMax;
        int start = (decPos_ - n) & (kDecBuf - 1);
        for (int j = 0; j < n; ++j) frame_[j] = dec_[(start + j) & (kDecBuf - 1)];

        if (level_ < 1.0e-5f) { voiced_ = false; return; }   // about -50 dBFS: silence
        float running = 0.0f;
        cmnd_[0] = 1.0f;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            float d = 0.0f;
            const float* a = frame_;
            const float* b = frame_ + tau;
            for (int j = 0; j < kDetWin; ++j) { const float e = a[j] - b[j]; d += e * e; }
            running += d;
            cmnd_[tau] = running > 0.0f ? d * (float)tau / running : 1.0f;
        }
        int best = -1;
        for (int tau = tauMin; tau < tauMax; ++tau)
            if (cmnd_[tau] < 0.12f) { while (tau + 1 < tauMax && cmnd_[tau + 1] < cmnd_[tau]) ++tau; best = tau; break; }
        if (best < 0)
        {
            int m = tauMin;
            for (int tau = tauMin; tau < tauMax; ++tau) if (cmnd_[tau] < cmnd_[m]) m = tau;
            if (cmnd_[m] > 0.3f) { voiced_ = false; return; }   // not a pitched sound (breath, "s", noise)
            best = m;
        }
        float t = (float)best;
        if (best > 1 && best < tauMax - 1)
        {
            const float a = cmnd_[best - 1], b = cmnd_[best], c = cmnd_[best + 1];
            const float den = a - 2.0f * b + c;
            if (std::fabs(den) > 1.0e-9f) t += 0.5f * (a - c) / den;
        }
        const float f = (float)dsr_ / t;
        // reject single octave slips: a reading far from both previous ones is ignored once
        const float note = 69.0f + 12.0f * std::log2(f / 440.0f);
        if (voiced_ && std::fabs(note - noteHist_[0]) > 5.0f && std::fabs(note - noteHist_[1]) > 5.0f && !skipped_) { skipped_ = true; return; }
        skipped_ = false;
        noteHist_[1] = voiced_ ? noteHist_[0] : note;
        noteHist_[0] = note;
        f0_ = f;
        voiced_ = true;
    }

    void decide(int key, int scale)
    {
        if (!voiced_) return;
        // The detector looks ~20 ms into the past; project the sung pitch forward along its
        // current slope so fast movements (vibrato, scoops) are corrected in time.
        const float slope = (noteHist_[0] - noteHist_[1]) / hopSec_;          // semitones per second
        sungNote_ = noteHist_[0] + mpc::clamp(slope * kLead, -1.0f, 1.0f);
        static const uint16_t masks[] = {
            0xFFF,                                              // chromatic
            (1<<0)|(1<<2)|(1<<4)|(1<<5)|(1<<7)|(1<<9)|(1<<11),  // major
            (1<<0)|(1<<2)|(1<<3)|(1<<5)|(1<<7)|(1<<8)|(1<<10),  // natural minor
            (1<<0)|(1<<2)|(1<<3)|(1<<5)|(1<<7)|(1<<8)|(1<<11),  // harmonic minor
            (1<<0)|(1<<2)|(1<<4)|(1<<7)|(1<<9),                 // major pentatonic
            (1<<0)|(1<<3)|(1<<5)|(1<<7)|(1<<10),                // minor pentatonic
            (1<<0)|(1<<3)|(1<<5)|(1<<6)|(1<<7)|(1<<10),         // blues
            (1<<0)|(1<<2)|(1<<3)|(1<<5)|(1<<7)|(1<<9)|(1<<10)   // dorian
        };
        const uint16_t mask = masks[mpc::clamp(scale, 0, 7)];
        auto allowed = [&](int n) { return (mask >> (((n - key) % 12 + 12) % 12)) & 1; };
        int best = -1000; float bestD = 1e9f;
        const int c = (int)std::lround(sungNote_);
        for (int n = c - 6; n <= c + 6; ++n)
            if (allowed(n)) { const float d = std::fabs((float)n - sungNote_); if (d < bestD) { bestD = d; best = n; } }
        // hysteresis: keep the current note unless the new one is clearly closer
        if (targetNote_ > -1000 && allowed(targetNote_) && best != targetNote_)
            if (std::fabs((float)targetNote_ - sungNote_) < bestD + 0.15f) best = targetNote_;
        targetNote_ = best;
    }

    double sr_ = 44100.0, dsr_ = 11025.0;
    Biquad aa1_, aa2_;
    float dec_[kDecBuf] = {0};
    float frame_[kDecBuf] = {0};
    float cmnd_[kDecBuf] = {0};
    int decPos_ = 0, decCount_ = 0, hopCount_ = 0;
    float level_ = 0.0f;
    float f0_ = 0.0f, hist_[3] = {0, 0, 0}, sungNote_ = 60.0f, noteHist_[2] = {60.0f, 60.0f}, hopSec_ = 0.0058f;
    bool skipped_ = false;
#ifndef MELTTUNE_LEAD
  #define MELTTUNE_LEAD 0.015f
#endif
    static constexpr float kLead = MELTTUNE_LEAD;   // seconds of look-ahead
    bool voiced_ = false;
    int targetNote_ = -1000;
    float shift_ = 0.0f, ratio_ = 1.0f;
    int ctl_ = 0;
    unsigned meter_ = 0;

    struct Grain { double start = 0.0, len = 0.0, pos = 0.0; float gain = 1.0f; };
    static constexpr int kGrains = 16;
    Grain grains_[kGrains];
    double untilGrain_ = 0.0, lastCentre_ = 0.0, margin_ = 44.0;
    int64_t now_ = 0;
    float bufL_[kBuf] = {0}, bufR_[kBuf] = {0};
    float hann_[kWinTable + 1] = {0};
    Smoother mix_, out_;
};

MPC_PLUGIN_ENTRY(MeltTune)
