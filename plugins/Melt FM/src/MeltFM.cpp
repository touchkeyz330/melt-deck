// MeltFM.cpp - a 4-operator FM synthesizer for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// Classic phase-modulation FM, original design: four operators, 8 algorithms, feedback on
// operator 4, four operator waveforms. Each operator has a frequency ratio (coarse + fine) or
// detune, output level (0-99, about 0.75 dB per step), its own ADSR, velocity sensitivity and
// key scaling. Modulator levels set brightness; carrier levels set volume.
// Patch: Poly / Mono legato with portamento, pitch bend, mod wheel vibrato, sustain pedal,
// LFO (vibrato, brightness, tremolo, with delay), optional low-pass filter.
// Effects: stereo chorus, delay, reverb.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/svf.h"
#include "dsp/adsr.h"
#include "dsp/smoother.h"
#include "dsp/delay_line.h"
#include "dsp/midi_cc.h"
#include "reverb.h"

#include <algorithm>

using namespace mpc::dsp;

static constexpr int kOps = 4;
static constexpr float kPiF = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Operator waves: 4096-point tables, shared
// ---------------------------------------------------------------------------
static constexpr int kTab = 4096;
struct WaveTables
{
    float t[4][kTab + 1];
    WaveTables()
    {
        for (int i = 0; i <= kTab; ++i)
        {
            const double s = std::sin(2.0 * 3.14159265358979 * (double)i / kTab);
            t[0][i] = (float)s;                                        // Sine
            t[1][i] = (float)(s > 0 ? s : 0.0) * 2.0f - 0.6366f;       // Half (DC removed)
            t[2][i] = (float)std::fabs(s) * 2.0f - 1.2732f;            // Abs  (DC removed)
            t[3][i] = (float)std::max(-1.0, std::min(1.0, s * 3.0));   // Square-ish
        }
    }
};
static const WaveTables& tables() { static WaveTables w; return w; }

// Algorithms: which operators modulate each operator, and which are carriers.
// Operators are numbered 1-4 (index 0-3); feedback is always on operator 4.
struct Algo { const char* name; uint8_t mods[kOps]; uint8_t carriers; };   // bitmasks over ops 0..3
static const Algo kAlgos[8] = {
    {"1 4>3>2>1",   {1u << 1, 1u << 2, 1u << 3, 0}, 0b0001},
    {"2 (3+4)>2>1", {1u << 1, (1u << 2) | (1u << 3), 0, 0}, 0b0001},
    {"3 3>2>1,4>1", {(1u << 1) | (1u << 3), 1u << 2, 0, 0}, 0b0001},
    {"4 4>3>1,2>1", {(1u << 1) | (1u << 2), 0, 1u << 3, 0}, 0b0001},
    {"5 2>1 | 4>3", {1u << 1, 0, 1u << 3, 0}, 0b0101},
    {"6 4>1,2,3",   {1u << 3, 1u << 3, 1u << 3, 0}, 0b0111},
    {"7 4>3 |1|2",  {0, 0, 1u << 3, 0}, 0b0111},
    {"8 1|2|3|4",   {0, 0, 0, 0}, 0b1111},
};

// ---------------------------------------------------------------------------
class MeltFM : public mpc::Plugin
{
public:
    enum OpParam { oCoarse, oFine, oDetune, oLevel, oWave, oA, oD, oS, oR, oVel, oKeyScale, kOpStride };
    enum Common
    {
        kAlgo = 0, kFeedback, kVolume, kMode, kPorta, kBend, kWheel,
        kLfoRate, kLfoWave, kLfoDelay, kLfoPitch, kLfoBright, kLfoAmp, kCutoff, kReso,
        kChoRate, kChoDepth, kChoLevel, kDlyTime, kDlyFb, kDlyMix, kRevTime, kRevLevel, kRevDamp,
        kOp1,
        kNumParams = kOp1 + kOps * kOpStride
    };
    static int op(int o, int p) { return kOp1 + o * kOpStride + p; }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt FM";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'V', 'v', 'Y');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    MeltFM() : mpc::Plugin(makeInfo()), tab_(tables())
    {
        using namespace mpc;
        std::vector<std::string> algos;
        for (const auto& a : kAlgos) algos.push_back(a.name);
        addParam(choiceParam("algo", "Algorithm", algos, 4));
        addParam(intParam("fb", "Feedback", 0, 7, 0));
        addParam(floatParam("vol", "Volume", -40.0f, 6.0f, -4.0f, "dB", 1));
        addParam(choiceParam("mode", "Key Mode", {"Poly", "Mono"}, 0));
        addParam(floatParam("porta", "Portamento", 0.0f, 2000.0f, 0.0f, "ms", 0, 0.3f));
        addParam(intParam("bend", "Bend Range", 0, 24, 2, "st"));
        addParam(floatParam("wheel", "Wheel Vib", 0.0f, 100.0f, 40.0f, "ct", 0));
        addParam(logParam("lrate", "LFO Rate", 0.05f, 20.0f, 5.5f, "Hz", 2));
        addParam(choiceParam("lwave", "LFO Wave", {"Tri", "Sine", "Saw", "Square", "Random"}, 0));
        addParam(floatParam("ldelay", "LFO Delay", 0.0f, 5000.0f, 0.0f, "ms", 0, 0.3f));
        addParam(floatParam("lpit", "LFO>Pitch", 0.0f, 100.0f, 0.0f, "ct", 0));
        addParam(floatParam("lbright", "LFO>Bright", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("lamp", "LFO>Amp", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam("cut", "Cutoff", 20.0f, 20000.0f, 20000.0f, "Hz", 0));
        addParam(floatParam("res", "Reso", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam("chorate", "Chorus Rate", 0.05f, 8.0f, 0.6f, "Hz", 2));
        addParam(floatParam("chodep", "Chorus Depth", 0.0f, 100.0f, 40.0f, "%", 0));
        addParam(floatParam("cholev", "Chorus Level", 0.0f, 100.0f, 25.0f, "%", 0));
        addParam(floatParam("dtime", "Delay Time", 20.0f, 1000.0f, 330.0f, "ms", 0));
        addParam(floatParam("dfb", "Delay Fdbk", 0.0f, 90.0f, 30.0f, "%", 0));
        addParam(floatParam("dmix", "Delay Mix", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam("revtime", "Reverb Time", 0.2f, 12.0f, 2.0f, "s", 1));
        addParam(floatParam("revlev", "Reverb Level", 0.0f, 100.0f, 20.0f, "%", 0));
        addParam(floatParam("revdamp", "Reverb Damp", 0.0f, 100.0f, 40.0f, "%", 0));
        for (int o = 0; o < kOps; ++o)
        {
            const std::string n = std::to_string(o + 1);
            auto id = [&](const char* s) { return std::string("o") + n + s; };
            auto nm = [&](const char* s) { return std::string("Op") + n + " " + s; };
            addParam(intParam(id("crs").c_str(), nm("Ratio").c_str(), 0, 31, 1));
            addParam(floatParam(id("fine").c_str(), nm("Fine").c_str(), 0.0f, 99.0f, 0.0f, "%", 0));
            addParam(floatParam(id("det").c_str(), nm("Detune").c_str(), -20.0f, 20.0f, 0.0f, "ct", 1));
            addParam(intParam(id("lev").c_str(), nm("Level").c_str(), 0, 99, o == 0 ? 99 : 0));
            addParam(choiceParam(id("wave").c_str(), nm("Wave").c_str(), {"Sine", "Half", "Abs", "Square"}, 0));
            addParam(logParam(id("a").c_str(), nm("Attack").c_str(), 0.5f, 10000.0f, 1.0f, "ms", 0));
            addParam(logParam(id("d").c_str(), nm("Decay").c_str(), 1.0f, 20000.0f, 1500.0f, "ms", 0));
            addParam(floatParam(id("s").c_str(), nm("Sustain").c_str(), 0.0f, 100.0f, 70.0f, "%", 0));
            addParam(logParam(id("r").c_str(), nm("Release").c_str(), 1.0f, 10000.0f, 300.0f, "ms", 0));
            addParam(floatParam(id("vel").c_str(), nm("Velocity").c_str(), 0.0f, 100.0f, 40.0f, "%", 0));
            addParam(floatParam(id("ks").c_str(), nm("Key Scale").c_str(), 0.0f, 100.0f, 0.0f, "%", 0));
        }
        if (numParams() != kNumParams) mpc::log("MeltFM: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);
        makePrograms();
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        for (auto& v : voices_) { v.filter.setSampleRate(sampleRate); for (auto& e : v.env) e.setSampleRate(sampleRate); }
        choL_.prepare((int)(0.04 * sampleRate) + 4);
        choR_.prepare((int)(0.04 * sampleRate) + 4);
        dlyL_.prepare((int)(1.05 * sampleRate) + 4);
        dlyR_.prepare((int)(1.05 * sampleRate) + 4);
        reverb_.prepare(sampleRate);
        volume_.setTime(15.0f, sampleRate);
        controllers_.prepare(sampleRate / kCtl, 10.0f);
        reset();
    }

    void reset() override
    {
        for (auto& v : voices_) { v.note = -1; v.held = v.sustained = false; v.filter.reset(); for (auto& e : v.env) e.reset(); }
        choL_.reset(); choR_.reset(); dlyL_.reset(); dlyR_.reset(); reverb_.reset();
        choPh_ = lfoPh_ = vibPh_ = 0.0f; rnd_ = 0.0f; rng_ = 1234567u;
        sustain_ = false; age_ = 0; held_ = 0; lastNote_ = -1.0f;
        volume_.snap(dbToGain(get(kVolume)));
        controllers_.reset();
    }

    void process(mpc::ProcessContext& ctx) override
    {
        readSettings();
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        const int nf = std::min(ctx.numFrames, kMaxBlock);
        std::fill(buf_, buf_ + nf, 0.0f);

        int pos = 0;
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            const int at = std::min(std::max(ev.sampleOffset, pos), nf);
            if (at > pos) { render(pos, at); pos = at; }
            handleMidi(ev);
        }
        render(pos, nf);
        effects(L, R, nf);
        for (int i = nf; i < ctx.numFrames; ++i) { L[i] = 0.0f; if (R) R[i] = 0.0f; }
    }

private:
    static constexpr int kVoices = 16;
    static constexpr int kCtl = 16;
    static constexpr int kMaxBlock = 4096;

    struct OpSettings { float ratio = 1, detune = 0, level = 1, vel = 0.4f, ks = 0; int wave = 0; float env[4] = {1, 1500, 0.7f, 300}; };

    struct Voice
    {
        int note = -1;
        bool held = false, sustained = false;
        unsigned age = 0;
        float vel = 1.0f, pitch = 60.0f, pitchTarget = 60.0f, time = 0.0f;
        float ph[kOps] = {}, gain[kOps] = {}, fb1 = 0, fb2 = 0;
        ADSR env[kOps];
        SVF filter;
        bool active(uint8_t carriers) const
        {
            for (int o = 0; o < kOps; ++o) if ((carriers >> o) & 1u) if (env[o].isActive()) return true;
            return false;
        }
    };

    struct Settings
    {
        int algo = 4, mode = 0, lwave = 0;
        float fb = 0, porta = 0, bend = 2, wheel = 0.4f;
        float lrate = 5.5f, ldelay = 0, lpit = 0, lbright = 0, lamp = 0, cutoff = 20000, reso = 0;
        float choRate = 0.6f, choDepth = 0.4f, choLevel = 0.25f, dTime = 330, dFb = 0.3f, dMix = 0, revLevel = 0.2f;
        OpSettings op[kOps];
    } s_;

    static float levelToGain(int l) { return l <= 0 ? 0.0f : std::pow(2.0f, ((float)l - 99.0f) / 8.0f); }
    static float coarseToRatio(int c) { return c == 0 ? 0.5f : (float)c; }

    void readSettings()
    {
        s_.algo = mpc::clamp(getInt(kAlgo), 0, 7);
        const int fb = getInt(kFeedback);
        s_.fb = fb == 0 ? 0.0f : 0.25f * std::pow(2.0f, (float)(fb - 7));   // in cycles: up to pi/2 rad
        s_.mode = getInt(kMode);
        s_.porta = get(kPorta);
        s_.bend = (float)getInt(kBend);
        s_.wheel = get(kWheel);
        s_.lrate = get(kLfoRate); s_.lwave = getInt(kLfoWave); s_.ldelay = get(kLfoDelay) * 0.001f;
        s_.lpit = get(kLfoPitch) * 0.01f; s_.lbright = get(kLfoBright) * 0.01f; s_.lamp = get(kLfoAmp) * 0.01f;
        s_.cutoff = get(kCutoff); s_.reso = get(kReso) * 0.01f;
        s_.choRate = get(kChoRate); s_.choDepth = get(kChoDepth) * 0.01f; s_.choLevel = get(kChoLevel) * 0.01f;
        s_.dTime = get(kDlyTime); s_.dFb = get(kDlyFb) * 0.01f; s_.dMix = get(kDlyMix) * 0.01f;
        s_.revLevel = get(kRevLevel) * 0.01f;
        reverb_.set(get(kRevTime), get(kRevDamp) * 0.01f);
        volume_.setTarget(dbToGain(get(kVolume)));
        for (int o = 0; o < kOps; ++o)
        {
            OpSettings& x = s_.op[o];
            x.ratio = coarseToRatio(getInt(op(o, oCoarse))) * (1.0f + get(op(o, oFine)) * 0.01f);
            x.detune = get(op(o, oDetune)) * 0.01f;
            x.level = levelToGain(getInt(op(o, oLevel)));
            x.wave = mpc::clamp(getInt(op(o, oWave)), 0, 3);
            x.env[0] = get(op(o, oA)); x.env[1] = get(op(o, oD)); x.env[2] = get(op(o, oS)) * 0.01f; x.env[3] = get(op(o, oR));
            x.vel = get(op(o, oVel)) * 0.01f;
            x.ks = get(op(o, oKeyScale)) * 0.01f;
        }
        const uint8_t car = kAlgos[s_.algo].carriers;
        for (auto& v : voices_)
            if (v.active(car))
                for (int o = 0; o < kOps; ++o) v.env[o].set(s_.op[o].env[0], s_.op[o].env[1], s_.op[o].env[2], s_.op[o].env[3]);
    }

    // ---- notes -------------------------------------------------------------
    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn()) noteOn(ev.note(), ev.velocity());
        else if (ev.isNoteOff()) noteOff(ev.note());
        else if (ev.isAllNotesOff()) { held_ = 0; for (auto& v : voices_) release(v); }
        else if (ev.isController() && ev.controller() == cc::Sustain)
        {
            sustain_ = ev.controllerValue() >= 64;
            if (!sustain_) for (auto& v : voices_) if (v.sustained) release(v);
        }
        else controllers_.handle(ev);
    }

    void noteOn(int note, int velocity)
    {
        const float from = lastNote_ < 0 ? (float)note : lastNote_;
        lastNote_ = (float)note;
        const uint8_t car = kAlgos[s_.algo].carriers;
        if (s_.mode == 1)
        {
            if (held_ < 16) heldKeys_[held_++] = note;
            Voice& v = voices_[0];
            if (v.active(car) && (v.held || v.sustained))
            {
                v.note = note; v.held = true; v.sustained = false; v.pitchTarget = (float)note;
                if (s_.porta <= 0.0f) v.pitch = (float)note;
                return;
            }
            start(v, note, velocity, s_.porta > 0.0f ? (v.active(car) ? v.pitch : from) : (float)note);
            return;
        }
        start(allocate(car), note, velocity, s_.porta > 0.0f ? from : (float)note);
    }

    void noteOff(int note)
    {
        if (s_.mode == 1)
        {
            int w = 0;
            for (int i = 0; i < held_; ++i) if (heldKeys_[i] != note) heldKeys_[w++] = heldKeys_[i];
            held_ = w;
            Voice& v = voices_[0];
            if (v.note != note) return;
            if (held_ > 0)
            {
                v.note = heldKeys_[held_ - 1]; v.pitchTarget = (float)v.note;
                if (s_.porta <= 0.0f) v.pitch = v.pitchTarget;
                lastNote_ = v.pitchTarget;
                return;
            }
        }
        for (auto& v : voices_)
            if (v.held && v.note == note) { v.held = false; if (sustain_) v.sustained = true; else release(v); }
    }

    void start(Voice& v, int note, int velocity, float fromPitch)
    {
        const bool wasActive = v.active(kAlgos[s_.algo].carriers);
        v.note = note; v.held = true; v.sustained = false; v.age = ++age_;
        v.vel = (float)velocity / 127.0f;
        v.pitch = fromPitch; v.pitchTarget = (float)note; v.time = 0.0f;
        for (int o = 0; o < kOps; ++o)
        {
            const OpSettings& x = s_.op[o];
            v.ph[o] = 0.0f;
            v.gain[o] = 0.0f;
            if (!wasActive) v.env[o].reset();
            v.env[o].set(x.env[0], x.env[1], x.env[2], x.env[3]);
            v.env[o].noteOn();
        }
        v.fb1 = v.fb2 = 0.0f;
        if (!wasActive) v.filter.reset();
    }

    void release(Voice& v)
    {
        v.held = v.sustained = false;
        for (auto& e : v.env) e.noteOff();
    }

    Voice& allocate(uint8_t car)
    {
        for (auto& v : voices_) if (!v.active(car)) return v;
        Voice* best = &voices_[0];
        for (auto& v : voices_)
        {
            const bool rel = !v.held && !v.sustained, bestRel = !best->held && !best->sustained;
            if ((rel && !bestRel) || (rel == bestRel && v.age < best->age)) best = &v;
        }
        return *best;
    }

    // ---- sound -------------------------------------------------------------
    float lfo(float inc)
    {
        lfoPh_ += inc;
        if (lfoPh_ >= 1.0f)
        {
            lfoPh_ -= std::floor(lfoPh_);
            rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
            rnd_ = (float)(rng_ >> 8) / 8388608.0f - 1.0f;
        }
        const float p = lfoPh_;
        switch (s_.lwave)
        {
            case 1: return std::sin(2.0f * kPiF * p);
            case 2: return 1.0f - 2.0f * p;
            case 3: return p < 0.5f ? 1.0f : -1.0f;
            case 4: return rnd_;
            default: return p < 0.25f ? 4.0f * p : p < 0.75f ? 2.0f - 4.0f * p : 4.0f * p - 4.0f;
        }
    }

    void render(int from, int to)
    {
        for (int c = from; c < to; c += kCtl) renderChunk(c, std::min(kCtl, to - c));
    }

    void renderChunk(int off, int n)
    {
        controllers_.tick();
        const float dtc = (float)kCtl / (float)sr_;
        const float bendSemis = controllers_.pitchBend() * s_.bend;
        vibPh_ += 5.5f * dtc; if (vibPh_ >= 1.0f) vibPh_ -= 1.0f;
        const float wheelSemis = controllers_.modWheel() * s_.wheel * 0.01f * std::sin(2.0f * kPiF * vibPh_);
        const float lfoRaw = lfo(s_.lrate * dtc);
        const float glide = s_.porta > 0.0f ? std::exp(-dtc / (s_.porta * 0.001f * 0.4f)) : 0.0f;
        const Algo& al = kAlgos[s_.algo];
        const int nCar = __builtin_popcount(al.carriers);
        const float carNorm = 1.0f / std::sqrt((float)nCar);
        const bool filt = s_.cutoff < 19500.0f || s_.reso > 0.01f;
        const float invN = 1.0f / (float)n;
        const float* tw[kOps];
        for (int o = 0; o < kOps; ++o) tw[o] = tab_.t[s_.op[o].wave];

        for (auto& v : voices_)
        {
            if (!v.active(al.carriers)) continue;
            v.pitch = v.pitchTarget + (v.pitch - v.pitchTarget) * glide;
            const float fade = s_.ldelay > 0.0f ? mpc::clamp((v.time - s_.ldelay * 0.5f) / (s_.ldelay * 0.5f), 0.0f, 1.0f) : 1.0f;
            const float lf = lfoRaw * fade;
            const float note = v.pitch + bendSemis + wheelSemis + lf * s_.lpit;
            const float baseHz = 440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f);
            const float bright = 1.0f - s_.lbright * 0.5f * (1.0f - lf);
            const float amp = (1.0f - s_.lamp * 0.5f * (1.0f - lf)) * carNorm;
            float inc[kOps], dg[kOps];
            for (int o = 0; o < kOps; ++o)
            {
                const OpSettings& x = s_.op[o];
                inc[o] = std::min(0.45f, baseHz * x.ratio * std::pow(2.0f, x.detune / 12.0f) / (float)sr_);
                float g = x.level * lerp(1.0f, v.vel, x.vel);
                if (x.ks > 0.0f && v.pitch > 60.0f) g *= std::pow(2.0f, -x.ks * (v.pitch - 60.0f) / 12.0f);
                g *= ((al.carriers >> o) & 1u) ? amp : bright;
                dg[o] = (g - v.gain[o]) * invN;
            }
            if (filt)
            {
                const float oct = 0.5f * (v.pitch - 60.0f) / 12.0f;
                v.filter.set(mpc::clamp(s_.cutoff * std::pow(2.0f, oct), 20.0f, 20000.0f), s_.reso);
            }
            float* out = buf_ + off;
            for (int i = 0; i < n; ++i)
            {
                float y[kOps];
                float e[kOps];
                for (int o = 0; o < kOps; ++o) { v.gain[o] += dg[o]; e[o] = v.env[o].process() * v.gain[o]; }
                // operator 4 (with feedback), then 3, 2, 1: modulators always have higher numbers
                for (int o = kOps - 1; o >= 0; --o)
                {
                    float m = 0.0f;
                    const uint8_t mods = al.mods[o];
                    for (int k = o + 1; k < kOps; ++k) if ((mods >> k) & 1u) m += y[k];
                    if (o == kOps - 1) m += (v.fb1 + v.fb2) * 0.5f * s_.fb * 4.0f;
                    float p = v.ph[o] + m;
                    p -= std::floor(p);
                    const float fi = p * (float)kTab;
                    const int i0 = (int)fi;
                    const float fr = fi - (float)i0;
                    const int ii = i0 & (kTab - 1);   // p can round up to exactly 1.0
                    const float* t = tw[o];
                    const float w = t[ii] + fr * (t[ii + 1] - t[ii]);
                    y[o] = w * e[o];
                    v.ph[o] += inc[o]; if (v.ph[o] >= 1.0f) v.ph[o] -= 1.0f;
                }
                v.fb2 = v.fb1; v.fb1 = y[kOps - 1];
                float s = 0.0f;
                for (int o = 0; o < kOps; ++o) if ((al.carriers >> o) & 1u) s += y[o];
                if (filt) s = v.filter.process(s, 0);
                out[i] += s * 0.25f;
            }
            v.time += dtc;
        }
    }

    void effects(float* L, float* R, int n)
    {
        const float choInc = s_.choRate / (float)sr_;
        const float base = 0.010f * (float)sr_, depth = 0.006f * (float)sr_ * s_.choDepth;
        const float cl = s_.choLevel, rl = s_.revLevel, dm = s_.dMix, dfb = s_.dFb;
        const float dlen = s_.dTime * 0.001f * (float)sr_;
        for (int i = 0; i < n; ++i)
        {
            const float x = buf_[i];
            float l = x, r = x;
            choL_.write(x);
            if (cl > 0.0f)
            {
                choPh_ += choInc; if (choPh_ >= 1.0f) choPh_ -= 1.0f;
                const float wl = choL_.readLinear(base + depth * std::sin(2.0f * kPiF * choPh_));
                const float wr = choL_.readLinear(base + depth * std::cos(2.0f * kPiF * choPh_));
                l = x * (1.0f - 0.4f * cl) + wl * cl;
                r = x * (1.0f - 0.4f * cl) + wr * cl;
            }
            const float tl = dlyL_.readLinear(dlen), tr = dlyR_.readLinear(dlen);
            dlyL_.write((l + r) * 0.5f * (dm > 0.0f ? 1.0f : 0.0f) + tr * dfb);
            dlyR_.write(tl * dfb);
            l += tl * dm; r += tr * dm;
            float rvL, rvR;
            reverb_.process(rl > 0.0f ? l : 0.0f, rl > 0.0f ? r : 0.0f, rvL, rvR);
            l += rvL * rl; r += rvR * rl;
            const float g = volume_.process();
            L[i] = sanitize(softClip(l * g));
            if (R) R[i] = sanitize(softClip(r * g));
        }
    }

    static float softClip(float x)
    {
        const float a = std::fabs(x);
        if (a < 0.7f) return x;
        const float y = 0.7f + 0.3f * fastTanh((a - 0.7f) / 0.3f);
        return x < 0 ? -y : y;
    }

    // ---- presets (all original) -------------------------------------------
    void makePrograms()
    {
        struct O { int o; std::initializer_list<std::pair<int, float>> v; };
        auto program = [&](const char* name, std::initializer_list<std::pair<int, float>> common, std::initializer_list<O> ops)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& p : common) v[(size_t)p.first] = p.second;
            for (const auto& o : ops) for (const auto& p : o.v) v[(size_t)op(o.o, p.first)] = p.second;
            addProgram(name, std::move(v));
        };
        // algorithm index is 0-based here (4 == "5 2>1 | 4>3")
        program("Tine Piano", {{kAlgo, 4}, {kChoLevel, 30}, {kRevLevel, 18}},
            {{0, {{oLevel, 99}, {oD, 4000}, {oS, 10}, {oR, 450}, {oVel, 50}, {oKeyScale, 20}}},
             {1, {{oCoarse, 1}, {oLevel, 74}, {oD, 2500}, {oS, 0}, {oR, 400}, {oVel, 90}, {oKeyScale, 40}}},
             {2, {{oCoarse, 1}, {oDetune, 7}, {oLevel, 92}, {oD, 1200}, {oS, 0}, {oR, 300}, {oVel, 50}}},
             {3, {{oCoarse, 14}, {oLevel, 70}, {oA, 0.5f}, {oD, 180}, {oS, 0}, {oR, 100}, {oVel, 100}}}});
        program("Solid Bass", {{kAlgo, 0}, {kFeedback, 5}, {kMode, 1}, {kPorta, 20}, {kChoLevel, 0}, {kRevLevel, 0}},
            {{0, {{oLevel, 99}, {oD, 3000}, {oS, 60}, {oR, 120}, {oVel, 20}}},
             {1, {{oCoarse, 1}, {oLevel, 80}, {oD, 400}, {oS, 30}, {oR, 120}, {oVel, 70}}},
             {2, {{oCoarse, 1}, {oLevel, 68}, {oD, 250}, {oS, 10}, {oR, 100}}},
             {3, {{oCoarse, 1}, {oLevel, 60}, {oD, 200}, {oS, 0}, {oR, 100}}}});
        program("Deep Sine Bass", {{kAlgo, 4}, {kMode, 1}, {kPorta, 30}, {kChoLevel, 0}, {kRevLevel, 0}},
            {{0, {{oLevel, 99}, {oD, 3000}, {oS, 80}, {oR, 100}, {oVel, 20}}},
             {1, {{oCoarse, 1}, {oLevel, 70}, {oD, 120}, {oS, 0}, {oR, 80}, {oVel, 80}}},
             {2, {{oLevel, 0}}}, {3, {{oLevel, 0}}}});
        program("Bell Tower", {{kAlgo, 4}, {kChoLevel, 30}, {kRevLevel, 40}, {kRevTime, 4.5f}},
            {{0, {{oLevel, 99}, {oD, 6000}, {oS, 0}, {oR, 3000}, {oVel, 40}}},
             {1, {{oCoarse, 3}, {oFine, 50}, {oLevel, 80}, {oD, 4000}, {oS, 0}, {oR, 2500}, {oVel, 60}}},
             {2, {{oCoarse, 2}, {oDetune, 3}, {oLevel, 85}, {oD, 3500}, {oS, 0}, {oR, 2500}}},
             {3, {{oCoarse, 7}, {oFine, 7}, {oLevel, 72}, {oD, 2500}, {oS, 0}, {oR, 2000}}}});
        program("Brass Section", {{kAlgo, 3}, {kFeedback, 6}, {kChoLevel, 35}, {kRevLevel, 18}, {kLfoPitch, 6}, {kLfoDelay, 600}, {kLfoRate, 5.2f}},
            {{0, {{oLevel, 99}, {oA, 40}, {oD, 600}, {oS, 85}, {oR, 200}, {oVel, 30}}},
             {1, {{oCoarse, 1}, {oLevel, 76}, {oA, 80}, {oD, 500}, {oS, 70}, {oR, 200}, {oVel, 60}}},
             {2, {{oCoarse, 1}, {oLevel, 70}, {oA, 60}, {oD, 700}, {oS, 60}, {oR, 200}}},
             {3, {{oCoarse, 1}, {oLevel, 62}, {oA, 50}, {oD, 600}, {oS, 50}, {oR, 200}}}});
        program("Marimba Wood", {{kAlgo, 4}, {kChoLevel, 10}, {kRevLevel, 22}},
            {{0, {{oLevel, 99}, {oA, 0.5f}, {oD, 700}, {oS, 0}, {oR, 300}, {oVel, 40}}},
             {1, {{oCoarse, 4}, {oLevel, 68}, {oA, 0.5f}, {oD, 120}, {oS, 0}, {oR, 100}, {oVel, 80}}},
             {2, {{oCoarse, 1}, {oLevel, 80}, {oA, 0.5f}, {oD, 400}, {oS, 0}, {oR, 200}}},
             {3, {{oCoarse, 10}, {oLevel, 55}, {oA, 0.5f}, {oD, 60}, {oS, 0}, {oR, 50}}}});
        program("Drawbar FM", {{kAlgo, 7}, {kChoRate, 6.0f}, {kChoDepth, 25}, {kChoLevel, 45}, {kRevLevel, 15}},
            {{0, {{oCoarse, 0}, {oLevel, 95}, {oA, 2}, {oS, 100}, {oR, 40}, {oVel, 0}}},
             {1, {{oCoarse, 1}, {oLevel, 97}, {oA, 2}, {oS, 100}, {oR, 40}, {oVel, 0}}},
             {2, {{oCoarse, 2}, {oLevel, 90}, {oA, 2}, {oS, 100}, {oR, 40}, {oVel, 0}}},
             {3, {{oCoarse, 3}, {oLevel, 85}, {oA, 1}, {oD, 150}, {oS, 50}, {oR, 40}, {oVel, 0}}}});
        program("Glass Pad", {{kAlgo, 4}, {kChoLevel, 50}, {kRevLevel, 45}, {kRevTime, 5.0f}, {kLfoBright, 30}, {kLfoRate, 0.3f}, {kLfoWave, 1}},
            {{0, {{oLevel, 99}, {oA, 900}, {oD, 3000}, {oS, 85}, {oR, 2500}, {oVel, 20}}},
             {1, {{oCoarse, 1}, {oLevel, 70}, {oA, 1500}, {oD, 4000}, {oS, 60}, {oR, 2500}}},
             {2, {{oCoarse, 2}, {oDetune, 6}, {oLevel, 90}, {oA, 1200}, {oD, 3000}, {oS, 85}, {oR, 2500}}},
             {3, {{oCoarse, 5}, {oLevel, 62}, {oA, 2000}, {oD, 3000}, {oS, 50}, {oR, 2500}}}});
        program("Pluck Harp", {{kAlgo, 4}, {kChoLevel, 25}, {kRevLevel, 30}},
            {{0, {{oLevel, 99}, {oA, 0.5f}, {oD, 1800}, {oS, 0}, {oR, 900}, {oVel, 50}}},
             {1, {{oCoarse, 2}, {oLevel, 72}, {oA, 0.5f}, {oD, 300}, {oS, 0}, {oR, 200}, {oVel, 80}}},
             {2, {{oCoarse, 1}, {oDetune, -5}, {oLevel, 88}, {oA, 0.5f}, {oD, 1400}, {oS, 0}, {oR, 800}}},
             {3, {{oCoarse, 3}, {oLevel, 66}, {oA, 0.5f}, {oD, 200}, {oS, 0}, {oR, 150}}}});
        program("Metal Lead", {{kAlgo, 1}, {kFeedback, 4}, {kMode, 1}, {kPorta, 50}, {kDlyMix, 25}, {kDlyTime, 375}, {kLfoPitch, 15}, {kLfoDelay, 500}, {kLfoRate, 5.5f}, {kLfoWave, 1}},
            {{0, {{oLevel, 99}, {oA, 5}, {oS, 90}, {oR, 200}}},
             {1, {{oCoarse, 2}, {oLevel, 72}, {oA, 5}, {oD, 600}, {oS, 70}, {oR, 200}, {oVel, 60}}},
             {2, {{oCoarse, 3}, {oLevel, 62}, {oA, 5}, {oS, 60}, {oR, 200}}},
             {3, {{oCoarse, 1}, {oFine, 1}, {oLevel, 60}, {oA, 5}, {oS, 60}, {oR, 200}}}});
        program("Toy Keys", {{kAlgo, 4}, {kChoLevel, 20}, {kRevLevel, 25}},
            {{0, {{oCoarse, 2}, {oLevel, 99}, {oA, 0.5f}, {oD, 900}, {oS, 0}, {oR, 300}}},
             {1, {{oCoarse, 7}, {oLevel, 66}, {oA, 0.5f}, {oD, 300}, {oS, 0}, {oR, 150}, {oVel, 70}}},
             {2, {{oCoarse, 4}, {oLevel, 75}, {oA, 0.5f}, {oD, 500}, {oS, 0}, {oR, 200}}},
             {3, {{oCoarse, 11}, {oLevel, 55}, {oA, 0.5f}, {oD, 120}, {oS, 0}, {oR, 80}}}});
        program("Synth Strings", {{kAlgo, 6}, {kFeedback, 7}, {kChoLevel, 55}, {kRevLevel, 35}, {kLfoPitch, 5}, {kLfoRate, 5.0f}, {kCutoff, 5000}},
            {{0, {{oLevel, 92}, {oDetune, -8}, {oA, 400}, {oD, 2000}, {oS, 90}, {oR, 800}, {oVel, 20}}},
             {1, {{oLevel, 92}, {oDetune, 8}, {oA, 450}, {oD, 2000}, {oS, 90}, {oR, 800}, {oVel, 20}}},
             {2, {{oCoarse, 2}, {oLevel, 80}, {oA, 500}, {oD, 2000}, {oS, 90}, {oR, 800}, {oVel, 20}}},
             {3, {{oCoarse, 1}, {oLevel, 70}, {oA, 600}, {oD, 3000}, {oS, 80}, {oR, 800}}}});
        program("Wobble Talk", {{kAlgo, 0}, {kFeedback, 3}, {kMode, 1}, {kPorta, 60}, {kLfoBright, 80}, {kLfoRate, 3.0f}, {kLfoWave, 1}, {kChoLevel, 0}, {kRevLevel, 10}},
            {{0, {{oLevel, 99}, {oS, 90}, {oR, 120}}},
             {1, {{oCoarse, 1}, {oLevel, 82}, {oS, 90}, {oR, 120}}},
             {2, {{oCoarse, 2}, {oLevel, 70}, {oS, 90}, {oR, 120}}},
             {3, {{oCoarse, 1}, {oLevel, 64}, {oS, 90}, {oR, 120}}}});
        program("Clav Funk", {{kAlgo, 2}, {kFeedback, 5}, {kChoLevel, 10}, {kRevLevel, 10}},
            {{0, {{oCoarse, 1}, {oLevel, 99}, {oA, 0.5f}, {oD, 900}, {oS, 20}, {oR, 60}, {oVel, 50}}},
             {1, {{oCoarse, 3}, {oLevel, 78}, {oA, 0.5f}, {oD, 400}, {oS, 30}, {oR, 60}, {oVel, 80}}},
             {2, {{oCoarse, 1}, {oLevel, 70}, {oA, 0.5f}, {oD, 300}, {oS, 20}, {oR, 60}}},
             {3, {{oCoarse, 5}, {oLevel, 60}, {oA, 0.5f}, {oD, 200}, {oS, 0}, {oR, 60}}}});
        program("Kalimba Drop", {{kAlgo, 4}, {kChoLevel, 15}, {kRevLevel, 35}, {kDlyMix, 18}, {kDlyTime, 250}},
            {{0, {{oLevel, 99}, {oA, 0.5f}, {oD, 1200}, {oS, 0}, {oR, 600}, {oVel, 40}}},
             {1, {{oCoarse, 6}, {oLevel, 64}, {oA, 0.5f}, {oD, 90}, {oS, 0}, {oR, 60}, {oVel, 90}}},
             {2, {{oLevel, 0}}}, {3, {{oLevel, 0}}}});
        program("Init FM", {}, {});
    }

    const WaveTables& tab_;
    double sr_ = 44100.0;
    Voice voices_[kVoices];
    unsigned age_ = 0;
    bool sustain_ = false;
    int heldKeys_[16] = {}, held_ = 0;
    float lastNote_ = -1.0f, vibPh_ = 0.0f, lfoPh_ = 0.0f, rnd_ = 0.0f;
    uint32_t rng_ = 1234567u;
    float buf_[kMaxBlock];
    DelayLine choL_, choR_, dlyL_, dlyR_;
    Reverb reverb_;
    float choPh_ = 0.0f;
    Smoother volume_;
    MidiControllers controllers_;
};

MPC_PLUGIN_ENTRY(MeltFM)
