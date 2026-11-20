// MeltMS.cpp - a semi-modular dual-filter monosynth for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// An original design in the spirit of the classic late-70s Japanese semi-modular monosynths with
// the screaming high-pass + low-pass filter pair. No code, presets or names are taken from any
// product. Per voice:
//   VCO 1: Triangle / Saw / Pulse (PW) / Noise, scale 32'-4'.
//   VCO 2: Saw / Square / Narrow pulse / Ring (VCO1 x VCO2), scale, pitch.
//   Freq mod from MG and EG 1. Mixer. Portamento.
//   HPF -> LPF, both 12 dB "aggressive" Sallen-Key style (zero-delay feedback, saturating
//   feedback: they scream and self-oscillate), each with Peak and MG / EG 2 modulation.
//   MG: waveform morph (triangle -> saw -> ramp) + square output; EG 1: Delay-Attack-Release;
//   EG 2: Hold-Attack-Decay-Sustain-Release (VCA and filters). Noise + sample & hold.
// PATCH PANEL: 4 virtual patch cables - "CV jacks" inside the plugin. Sources include the MG,
//   envelopes, S&H, VCO 2 at audio rate (filter FM), keyboard CV, velocity, and the MPC's own
//   controls: mod wheel / touch strip, pad pressure (channel or poly aftertouch) and pitch bend.
// Mono (classic), Legato or Poly (4). The MPC's hardware CV/Gate jacks are driven by the MPC's
// CV tracks, not by plugins - see the README for connecting a real hardware synth.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/adsr.h"
#include "dsp/smoother.h"
#include "dsp/midi_cc.h"

#include <algorithm>

using namespace mpc::dsp;
static constexpr float kPiF = 3.14159265358979f;

// One TPT one-pole (shared by the filters).
struct OnePole
{
    float G = 0.0f, z = 0.0f, beta = 0.0f;
    inline float lp(float x) { const float v = (x - z) * G; const float y = v + z; z = y + v; return y; }
    inline float hp(float x) { const float v = (x - z) * G; const float y = v + z; z = y + v; return x - y; }
    inline float fb() const { return beta * z; }
};

// 2-pole Sallen-Key style low-pass with saturating feedback (zero-delay feedback form).
struct SKLowpass
{
    OnePole lp1, lp2, hp1;
    float K = 0.0f, a0 = 1.0f, sat = 1.0f;
    void set(float fc, float sr, float peak)
    {
        const float g = std::tan(kPiF * fc / sr), G = g / (1.0f + g);
        lp1.G = lp2.G = hp1.G = G;
        K = 0.01f + 2.15f * peak;                            // > 2 (peak above ~93%) = self-oscillation
        lp2.beta = (K - K * G) / (1.0f + g);
        hp1.beta = -1.0f / (1.0f + g);
        a0 = 1.0f / (1.0f - K * G + K * G * G);
    }
    inline float process(float x)
    {
        const float y1 = lp1.lp(x);
        const float s = hp1.fb() + lp2.fb();
        float u = a0 * (y1 + s);
        u = fastTanh(u * 1.2f) / 1.2f;                       // the feedback loop saturates: grit and bounded screaming
        const float y = K * lp2.lp(u);
        hp1.hp(y);
        return y / K;
    }
    void reset() { lp1.z = lp2.z = hp1.z = 0.0f; }
};

// 2-pole Sallen-Key style high-pass with saturating feedback.
struct SKHighpass
{
    OnePole hp1, hp2, lp1;
    float K = 0.0f, a0 = 1.0f;
    void set(float fc, float sr, float peak)
    {
        const float g = std::tan(kPiF * fc / sr), G = g / (1.0f + g);
        hp1.G = hp2.G = lp1.G = G;
        K = 0.01f + 2.15f * peak;
        hp2.beta = -G / (1.0f + g);
        lp1.beta = 1.0f / (1.0f + g);
        a0 = 1.0f / (1.0f - K * G + K * G * G);
    }
    inline float process(float x)
    {
        const float y1 = hp1.hp(x);
        const float s = hp2.fb() + lp1.fb();
        const float u = a0 * (y1 + s);
        float y = K * u;
        y = fastTanh(y * 1.2f) / 1.2f;
        lp1.lp(hp2.hp(y));
        return y / K;
    }
    void reset() { hp1.z = hp2.z = lp1.z = 0.0f; }
};

class MeltMS : public mpc::Plugin
{
public:
    enum Params
    {
        kV1Wave = 0, kV1Scale, kV1Pw, kV2Wave, kV2Scale, kV2Pitch, kPorta, kMgFm, kEg1Fm, kV1Lev, kV2Lev,
        kHpCut, kHpPeak, kHpMg, kHpEg2, kLpCut, kLpPeak, kLpMg, kLpEg2,
        kMgWave, kMgFreq, kE1Delay, kE1Attack, kE1Release, kE2Hold, kE2Attack, kE2Decay, kE2Sustain, kE2Release,
        kP1Src, kP1Dst, kP1Amt, kP2Src, kP2Dst, kP2Amt, kP3Src, kP3Dst, kP3Amt, kP4Src, kP4Dst, kP4Amt,
        kMode, kTune, kKbdTrack, kDrive, kVolume, kBend,
        kNumParams
    };
    enum Src { sMgTri, sMgSq, sEg1, sEg2Rev, sNoise, sSH, sVco2, sKbd, sVel, sWheel, sPress, sBend };
    enum Dst { dTotal, dVco2, dHp, dLp, dVca, dPw, dMgRate, dShClock };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt MS";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'N', 'Y', 'o');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    MeltMS() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        const std::vector<std::string> scales = {"32'", "16'", "8'", "4'"};
        const std::vector<std::string> srcs = {"MG Tri", "MG Square", "EG1", "EG2 Rev", "Noise", "S&H", "VCO2 Out", "Kbd CV", "Velocity", "Wheel/Strip", "Pressure", "Pitch Bend"};
        const std::vector<std::string> dsts = {"Total Freq", "VCO2 Freq", "HP Cutoff", "LP Cutoff", "VCA", "PW", "MG Freq", "S&H Clock"};
        addParam(choiceParam("v1w", "VCO1 Wave", {"Triangle", "Saw", "Pulse", "Noise"}, 1));
        addParam(choiceParam("v1s", "VCO1 Scale", scales, 1));
        addParam(floatParam("v1pw", "VCO1 PW", 5.0f, 95.0f, 50.0f, "%", 0));
        addParam(choiceParam("v2w", "VCO2 Wave", {"Saw", "Square", "Narrow", "Ring"}, 1));
        addParam(choiceParam("v2s", "VCO2 Scale", scales, 1));
        addParam(floatParam("v2p", "VCO2 Pitch", -12.0f, 12.0f, 0.1f, "st", 2));
        addParam(floatParam("porta", "Portamento", 0.0f, 2000.0f, 0.0f, "ms", 0, 0.3f));
        addParam(floatParam("mgfm", "MG Freq Mod", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("eg1fm", "EG1 Freq Mod", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("v1l", "VCO1 Level", 0.0f, 100.0f, 80.0f, "%", 0));
        addParam(floatParam("v2l", "VCO2 Level", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam("hpc", "HP Cutoff", 10.0f, 15000.0f, 15.0f, "Hz", 0));
        addParam(floatParam("hpp", "HP Peak", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("hpmg", "HP MG Mod", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("hpeg", "HP EG2 Mod", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam("lpc", "LP Cutoff", 20.0f, 18000.0f, 1500.0f, "Hz", 0));
        addParam(floatParam("lpp", "LP Peak", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam("lpmg", "LP MG Mod", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("lpeg", "LP EG2 Mod", -100.0f, 100.0f, 40.0f, "%", 0));
        addParam(floatParam("mgw", "MG Wave", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(logParam("mgf", "MG Freq", 0.05f, 50.0f, 4.0f, "Hz", 2));
        addParam(logParam("e1d", "EG1 Delay", 0.5f, 5000.0f, 1.0f, "ms", 0));
        addParam(logParam("e1a", "EG1 Attack", 0.5f, 10000.0f, 5.0f, "ms", 0));
        addParam(logParam("e1r", "EG1 Release", 1.0f, 10000.0f, 300.0f, "ms", 0));
        addParam(logParam("e2h", "EG2 Hold", 0.5f, 5000.0f, 0.5f, "ms", 0));
        addParam(logParam("e2a", "EG2 Attack", 0.5f, 10000.0f, 2.0f, "ms", 0));
        addParam(logParam("e2d", "EG2 Decay", 1.0f, 10000.0f, 400.0f, "ms", 0));
        addParam(floatParam("e2s", "EG2 Sustain", 0.0f, 100.0f, 60.0f, "%", 0));
        addParam(logParam("e2r", "EG2 Release", 1.0f, 10000.0f, 200.0f, "ms", 0));
        static const int defS[4] = {sWheel, sPress, sMgTri, sVel}, defD[4] = {dLp, dLp, dTotal, dVca};
        for (int p = 0; p < 4; ++p)
        {
            const std::string n = std::to_string(p + 1);
            addParam(choiceParam(("c" + n + "s").c_str(), ("Cable" + n + " From").c_str(), srcs, defS[p]));
            addParam(choiceParam(("c" + n + "d").c_str(), ("Cable" + n + " To").c_str(), dsts, defD[p]));
            addParam(floatParam(("c" + n + "a").c_str(), ("Cable" + n + " Amt").c_str(), -100.0f, 100.0f, p == 0 ? 40.0f : 0.0f, "%", 0));
        }
        addParam(choiceParam("mode", "Key Mode", {"Mono", "Legato", "Poly 4"}, 0));
        addParam(floatParam("tune", "Master Tune", -100.0f, 100.0f, 0.0f, "ct", 0));
        addParam(floatParam("kbd", "Kbd Track", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam("drive", "Drive", 0.0f, 100.0f, 20.0f, "%", 0));
        addParam(floatParam("vol", "Volume", -40.0f, 6.0f, -6.0f, "dB", 1));
        addParam(intParam("bend", "Bend Range", 0, 12, 2, "st"));
        if (numParams() != kNumParams) mpc::log("MeltMS: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);
        makePrograms();
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        for (auto& v : voices_) { v.eg2.setSampleRate(sampleRate); }
        volume_.setTime(15.0f, sampleRate);
        controllers_.prepare(sampleRate / kCtl, 10.0f);
        reset();
    }

    void reset() override
    {
        for (auto& v : voices_) v.kill();
        mgPh_ = 0.0f; shVal_ = 0.0f; shPrev_ = false; pink_[0] = pink_[1] = pink_[2] = 0.0f; pressure_ = 0.0f;
        held_ = 0; lastNote_ = -1.0f; age_ = 0; sustain_ = false;
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
        const float drive = 1.0f + s_.drive * 4.0f, dn = 1.0f / fastTanh(drive);
        for (int i = 0; i < nf; ++i)
        {
            float x = buf_[i];
            if (s_.drive > 0.0f) x = fastTanh(x * drive) * dn;
            x *= volume_.process();
            if (std::fabs(x) < 1.0e-20f) x = 0.0f;
            L[i] = sanitize(x);
            if (R) R[i] = sanitize(x);
        }
        for (int i = nf; i < ctx.numFrames; ++i) { L[i] = 0.0f; if (R) R[i] = 0.0f; }
    }

private:
    static constexpr int kMaxVoices = 4, kCtl = 16, kMaxBlock = 4096;

    // EG 1: Delay -> Attack -> (hold while gate) -> Release
    struct EG1
    {
        int stage = 0; float level = 0.0f, t = 0.0f;   // 0 idle, 1 delay, 2 attack, 3 held, 4 release
        void on() { stage = 1; t = 0.0f; }
        void off() { if (stage != 0) stage = 4; }
        float step(float dt, float dly, float att, float rel)
        {
            switch (stage)
            {
                case 1: t += dt; if (t >= dly) stage = 2; break;
                case 2: level += dt / std::max(1.0e-4f, att); if (level >= 1.0f) { level = 1.0f; stage = 3; } break;
                case 4: level -= level * (1.0f - std::exp(-dt / std::max(1.0e-4f, rel * 0.3f))); if (level < 1.0e-4f) { level = 0.0f; stage = 0; } break;
                default: break;
            }
            return level;
        }
    };

    struct Voice
    {
        int note = -1;
        bool held = false, sustained = false, active = false;
        unsigned age = 0;
        float vel = 1.0f, pitch = 60.0f, target = 60.0f;
        float ph1 = 0.0f, ph2 = 0.0f, holdT = 0.0f, ping = 0.0f;
        bool holding = false;
        EG1 eg1;
        ADSR eg2;
        SKHighpass hpf;
        SKLowpass lpf;
        uint32_t rng = 1;
        void kill() { active = false; note = -1; held = sustained = false; eg2.reset(); eg1 = EG1(); hpf.reset(); lpf.reset(); }
    };

    struct Settings
    {
        int v1w = 1, v2w = 1, mode = 0, src[4] = {}, dst[4] = {};
        float v1oct = 0, v2oct = 0, pw = 0.5f, v2pitch = 0, porta = 0, mgFm = 0, eg1Fm = 0, v1l = 0.8f, v2l = 0;
        float hpc = 15, hpp = 0, hpmg = 0, hpeg = 0, lpc = 1500, lpp = 0.3f, lpmg = 0, lpeg = 0.4f;
        float mgw = 0.5f, mgf = 4, e1d = 0, e1a = 0.005f, e1r = 0.3f, e2h = 0, amt[4] = {};
        float env2[4] = {}, tune = 0, kbd = 0.5f, drive = 0.2f, bend = 2;
    } s_;

    void readSettings()
    {
        static const float octs[4] = {-1.0f, 0.0f, 1.0f, 2.0f};   // 32' 16' 8' 4' relative to 16'
        s_.v1w = getInt(kV1Wave); s_.v1oct = octs[mpc::clamp(getInt(kV1Scale), 0, 3)]; s_.pw = get(kV1Pw) * 0.01f;
        s_.v2w = getInt(kV2Wave); s_.v2oct = octs[mpc::clamp(getInt(kV2Scale), 0, 3)]; s_.v2pitch = get(kV2Pitch);
        s_.porta = get(kPorta); s_.mgFm = get(kMgFm) * 0.01f; s_.eg1Fm = get(kEg1Fm) * 0.01f;
        s_.v1l = get(kV1Lev) * 0.01f; s_.v2l = get(kV2Lev) * 0.01f;
        s_.hpc = get(kHpCut); s_.hpp = get(kHpPeak) * 0.01f; s_.hpmg = get(kHpMg) * 0.01f; s_.hpeg = get(kHpEg2) * 0.01f;
        s_.lpc = get(kLpCut); s_.lpp = get(kLpPeak) * 0.01f; s_.lpmg = get(kLpMg) * 0.01f; s_.lpeg = get(kLpEg2) * 0.01f;
        s_.mgw = get(kMgWave) * 0.01f; s_.mgf = get(kMgFreq);
        s_.e1d = get(kE1Delay) * 0.001f; s_.e1a = get(kE1Attack) * 0.001f; s_.e1r = get(kE1Release) * 0.001f;
        s_.e2h = get(kE2Hold) * 0.001f;
        s_.env2[0] = get(kE2Attack); s_.env2[1] = get(kE2Decay); s_.env2[2] = get(kE2Sustain) * 0.01f; s_.env2[3] = get(kE2Release);
        for (int p = 0; p < 4; ++p) { s_.src[p] = getInt(kP1Src + 3 * p); s_.dst[p] = getInt(kP1Dst + 3 * p); s_.amt[p] = get(kP1Amt + 3 * p) * 0.01f; }
        s_.mode = getInt(kMode); s_.tune = get(kTune) * 0.01f; s_.kbd = get(kKbdTrack) * 0.01f;
        s_.drive = get(kDrive) * 0.01f; s_.bend = (float)getInt(kBend);
        volume_.setTarget(dbToGain(get(kVolume)));
        for (auto& v : voices_) if (v.active) v.eg2.set(s_.env2[0], s_.env2[1], s_.env2[2], s_.env2[3]);
    }

    // ---- notes ----------------------------------------------------------------------------
    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn()) noteOn(ev.note(), ev.velocity());
        else if (ev.isNoteOff()) noteOff(ev.note());
        else if (ev.isAllNotesOff()) { held_ = 0; for (auto& v : voices_) release(v); }
        else if (ev.status() == 0xA0) pressure_ = (float)ev.data[2] / 127.0f;   // poly pressure (MPCe pads): latest pad wins
        else if (ev.isController() && ev.controller() == cc::Sustain)
        {
            sustain_ = ev.controllerValue() >= 64;
            if (!sustain_) for (auto& v : voices_) if (v.sustained) release(v);
        }
        else
        {
            if (ev.isAftertouch()) pressure_ = (float)ev.data[1] / 127.0f;
            controllers_.handle(ev);
        }
    }

    void noteOn(int note, int vel)
    {
        const float from = lastNote_ < 0 ? (float)note : lastNote_;
        lastNote_ = (float)note;
        if (s_.mode <= 1)
        {
            if (held_ < 16) heldKeys_[held_++] = note;
            Voice& v = voices_[0];
            if (v.active && (v.held || v.sustained))
            {
                v.note = note; v.held = true; v.target = (float)note;
                if (s_.porta <= 0.0f) v.pitch = (float)note;
                if (s_.mode == 0) { v.vel = (float)vel / 127.0f; trigger(v); }   // Mono retriggers, Legato does not
                return;
            }
            start(v, note, vel, s_.porta > 0.0f ? from : (float)note);
            return;
        }
        Voice* best = &voices_[0];
        for (auto& v : voices_) { if (!v.active) { best = &v; break; } if (v.age < best->age) best = &v; }
        start(*best, note, vel, s_.porta > 0.0f ? from : (float)note);
    }

    void noteOff(int note)
    {
        if (s_.mode <= 1)
        {
            int w = 0;
            for (int i = 0; i < held_; ++i) if (heldKeys_[i] != note) heldKeys_[w++] = heldKeys_[i];
            held_ = w;
            Voice& v = voices_[0];
            if (v.note == note && held_ > 0)
            {
                v.note = heldKeys_[held_ - 1]; v.target = (float)v.note; lastNote_ = v.target;
                if (s_.porta <= 0.0f) v.pitch = v.target;
                return;
            }
        }
        for (auto& v : voices_)
            if (v.held && v.note == note) { v.held = false; if (sustain_) v.sustained = true; else release(v); }
    }

    void trigger(Voice& v)
    {
        v.eg1.on();
        v.ping = 1.0f;   // the gate edge 'pings' the filters - at high Peak they ring (filter kicks, zaps)
        v.holding = s_.e2h > 0.0f; v.holdT = 0.0f;
        v.eg2.set(s_.env2[0], s_.env2[1], s_.env2[2], s_.env2[3]);
        if (!v.holding) v.eg2.noteOn();
    }

    void start(Voice& v, int note, int vel, float from)
    {
        if (!v.active) { v.hpf.reset(); v.lpf.reset(); v.eg2.reset(); v.eg1 = EG1(); v.ph1 = v.ph2 = 0.0f; }
        v.active = true; v.note = note; v.held = true; v.sustained = false; v.age = ++age_;
        v.vel = (float)vel / 127.0f; v.pitch = from; v.target = (float)note;
        v.rng = 0x9E3779B9u * (age_ + 5);
        trigger(v);
    }

    void release(Voice& v) { v.held = v.sustained = false; v.eg1.off(); v.holding = false; v.eg2.noteOff(); }

    // ---- sound -----------------------------------------------------------------------------
    static inline float rnd(uint32_t& r) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return (float)(r >> 8) / 8388608.0f - 1.0f; }
    static inline float blep(float t, float dt)
    {
        if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
        if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
        return 0.0f;
    }
    static inline float saw(float ph, float dt) { return 2.0f * ph - 1.0f - blep(ph, dt); }
    static inline float pulse(float ph, float dt, float pw)
    {
        float p2 = ph + 1.0f - pw; if (p2 >= 1.0f) p2 -= 1.0f;
        return (saw(ph, dt) - saw(p2, dt)) * 0.5f;
    }

    void render(int from, int to) { for (int c = from; c < to; c += kCtl) renderChunk(c, std::min(kCtl, to - c)); }

    void renderChunk(int off, int n)
    {
        controllers_.tick();
        const float dtc = (float)n / (float)sr_;
        const float wheel = controllers_.modWheel(), bendV = controllers_.pitchBend();
        // MG: waveform morph: 0 = falling ramp, 0.5 = triangle, 1 = rising saw; plus a square output
        float mgRate = s_.mgf;
        for (int p = 0; p < 4; ++p)
            if (s_.dst[p] == dMgRate && s_.amt[p] != 0.0f)
            {
                const float sv = s_.src[p] == sWheel ? wheel : s_.src[p] == sPress ? pressure_ : s_.src[p] == sBend ? bendV : 0.0f;
                mgRate *= std::pow(8.0f, sv * s_.amt[p]);
            }
        mgPh_ += mgRate * dtc; if (mgPh_ >= 1.0f) mgPh_ -= std::floor(mgPh_);
        const float w = mpc::clamp(s_.mgw, 0.02f, 0.98f);
        const float mgTri = mgPh_ < w ? -1.0f + 2.0f * mgPh_ / w : 1.0f - 2.0f * (mgPh_ - w) / (1.0f - w);
        const float mgSq = mgPh_ < 0.5f ? 1.0f : 0.0f;   // unipolar pulse, like a gate
        // sample & hold: noise sampled on each MG rising edge (or a patched clock)
        float clockSrc = mgSq;
        for (int p = 0; p < 4; ++p) if (s_.dst[p] == dShClock && s_.amt[p] != 0.0f)
        {
            const float sv = s_.src[p] == sMgTri ? mgTri : s_.src[p] == sWheel ? wheel : s_.src[p] == sPress ? pressure_ : mgSq;
            clockSrc = sv * s_.amt[p] > 0.25f ? 1.0f : 0.0f;
        }
        const bool clk = clockSrc > 0.5f;
        if (clk && !shPrev_) shVal_ = rnd(shRng_);
        shPrev_ = clk;
        const float bendSemis = bendV * s_.bend;
        const float glide = s_.porta > 0.0f ? std::exp(-dtc / (s_.porta * 0.001f * 0.4f)) : 0.0f;

        for (auto& v : voices_)
        {
            if (!v.active) continue;
            v.pitch = v.target + (v.pitch - v.target) * glide;
            // envelopes (control rate for EG1; EG2 hold stage)
            const float e1 = v.eg1.step(dtc, s_.e1d, s_.e1a, s_.e1r);
            if (v.holding) { v.holdT += dtc; if (v.holdT >= s_.e2h) { v.holding = false; v.eg2.noteOn(); } }
            const float e2 = v.eg2.level();
            // patch panel (control-rate sources); VCO 2 audio-rate sources are applied per sample below
            const float srcs[12] = {mgTri, mgSq, e1, 1.0f - e2, 0.0f, shVal_, 0.0f, (v.pitch - 60.0f) / 12.0f, v.vel, wheel, pressure_, bendV};
            float pTotal = 0, pV2 = 0, pHp = 0, pLp = 0, pVca = 0, pPw = 0;
            float aLp[2] = {}, aHp[2] = {}, aTot[2] = {};   // audio-rate depths: [0] VCO2, [1] noise
            for (int p = 0; p < 4; ++p)
            {
                const float a = s_.amt[p];
                if (a == 0.0f) continue;
                if (s_.src[p] == sVco2 || s_.src[p] == sNoise)
                {   // audio-rate sources: collect their depths, applied inside the sample loop
                    const int k = s_.src[p] == sVco2 ? 0 : 1;
                    if (s_.dst[p] == dLp) aLp[k] += a;
                    else if (s_.dst[p] == dHp) aHp[k] += a;
                    else if (s_.dst[p] == dTotal) aTot[k] += a;
                    else if (s_.dst[p] == dVca && k == 1) pVca += 0.0f;   // noise -> VCA has no musical use; ignored
                    continue;
                }
                const float x = srcs[s_.src[p]] * a;
                switch (s_.dst[p])
                {
                    case dTotal: pTotal += x * 24.0f; break;   // up to +-2 octaves
                    case dVco2: pV2 += x * 24.0f; break;
                    case dHp: pHp += x * 4.0f; break;          // octaves
                    case dLp: pLp += x * 4.0f; break;
                    case dVca: pVca += x; break;
                    case dPw: pPw += x * 0.45f; break;
                    default: break;
                }
            }
            const float fm = s_.mgFm * mgTri * 2.0f + s_.eg1Fm * e1 * 24.0f;   // semitones
            const float note = v.pitch + bendSemis + s_.tune + fm + pTotal;
            const float d1 = std::min(0.45f, 440.0f * std::pow(2.0f, (note + 12.0f * s_.v1oct - 69.0f) / 12.0f) / (float)sr_);
            const float d2 = std::min(0.45f, 440.0f * std::pow(2.0f, (note + 12.0f * s_.v2oct + s_.v2pitch + pV2 - 69.0f) / 12.0f) / (float)sr_);
            const float pw = mpc::clamp(s_.pw + pPw, 0.05f, 0.95f);
            const float kt = s_.kbd * (v.pitch - 60.0f) / 12.0f;
            const float hpOct = s_.hpmg * mgTri * 3.0f + s_.hpeg * e2 * 6.0f + pHp + kt;
            const float lpOct = s_.lpmg * mgTri * 3.0f + s_.lpeg * e2 * 6.0f + pLp + kt;
            const float hpBase = mpc::clamp(s_.hpc * std::pow(2.0f, mpc::clamp(hpOct, -10.0f, 10.0f)), 10.0f, 0.42f * (float)sr_);
            const float lpBase = mpc::clamp(s_.lpc * std::pow(2.0f, mpc::clamp(lpOct, -10.0f, 10.0f)), 20.0f, 0.42f * (float)sr_);
            const bool audioMod = aLp[0] != 0.0f || aLp[1] != 0.0f || aHp[0] != 0.0f || aHp[1] != 0.0f;
            const bool audioFm = aTot[0] != 0.0f || aTot[1] != 0.0f;
            if (!audioMod) { v.hpf.set(hpBase, (float)sr_, s_.hpp); v.lpf.set(lpBase, (float)sr_, s_.lpp); }
            const float vca = mpc::clamp(1.0f + pVca, 0.0f, 2.0f) * 0.35f;
            const float pingAmt = 2.0f * s_.lpp * s_.lpp * s_.lpp * s_.lpp;

            float* b = buf_ + off;
            float pnPrev = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                // VCO 2 first (it can modulate everything at audio rate)
                float o2;
                switch (s_.v2w)
                {
                    case 1: o2 = pulse(v.ph2, d2, 0.5f); break;
                    case 2: o2 = pulse(v.ph2, d2, 0.12f); break;
                    default: o2 = saw(v.ph2, d2); break;
                }
                float dd1 = d1;
                if (audioFm) dd1 = std::min(0.45f, d1 * std::pow(2.0f, (o2 * aTot[0] + pnPrev * aTot[1]) * 2.0f));
                float o1;
                switch (s_.v1w)
                {
                    case 0: o1 = v.ph1 < 0.5f ? 4.0f * v.ph1 - 1.0f : 3.0f - 4.0f * v.ph1; break;
                    case 2: o1 = pulse(v.ph1, dd1, pw); break;
                    case 3: o1 = rnd(v.rng) * 0.8f; break;
                    default: o1 = saw(v.ph1, dd1); break;
                }
                if (s_.v2w == 3) o2 = o1 * saw(v.ph2, d2);   // ring modulator
                v.ph1 += dd1; if (v.ph1 >= 1.0f) v.ph1 -= 1.0f;
                v.ph2 += d2; if (v.ph2 >= 1.0f) v.ph2 -= 1.0f;
                // pink-ish noise for patching
                const float white = rnd(v.rng);
                pink_[0] = 0.997f * pink_[0] + 0.029591f * white; pink_[1] = 0.985f * pink_[1] + 0.032534f * white; pink_[2] = 0.95f * pink_[2] + 0.048056f * white;
                const float pn = mpc::clamp((pink_[0] + pink_[1] + pink_[2]) * 3.0f, -1.0f, 1.0f);
                pnPrev = pn;
                if (audioMod)
                {
                    const float hpF = mpc::clamp(hpBase * std::pow(2.0f, mpc::clamp((aHp[0] * o2 + aHp[1] * pn) * 6.0f, -10.0f, 10.0f)), 10.0f, 0.42f * (float)sr_);
                    const float lpF = mpc::clamp(lpBase * std::pow(2.0f, mpc::clamp((aLp[0] * o2 + aLp[1] * pn) * 6.0f, -10.0f, 10.0f)), 20.0f, 0.42f * (float)sr_);
                    v.hpf.set(hpF, (float)sr_, s_.hpp); v.lpf.set(lpF, (float)sr_, s_.lpp);
                }
                float x = o1 * s_.v1l + o2 * s_.v2l;
                x += (v.ping * pingAmt + white * 1.0e-4f);   // gate ping + a whisper of noise so a self-oscillating filter always starts
                v.ping *= 0.6f;
                x = v.lpf.process(v.hpf.process(x * 0.6f));
                const float a = v.eg2.process();
                b[i] += x * a * vca;
            }
            if (!v.eg2.isActive() && !v.holding) v.kill();
            else if (v.eg2.stage() == ADSR::Stage::Sustain && v.eg2.level() < 1.0e-4f && !v.held) v.kill();
        }
    }

    // ---- presets (all original) -------------------------------------------------------------
    void makePrograms()
    {
        auto program = [&](const char* name, std::initializer_list<std::pair<int, float>> o)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& p : o) v[(size_t)p.first] = p.second;
            addProgram(name, std::move(v));
        };
        // sources: 0 MGTri 1 MGSq 2 EG1 3 EG2Rev 4 Noise 5 S&H 6 VCO2 7 Kbd 8 Vel 9 Wheel 10 Press 11 Bend
        // dests: 0 Total 1 VCO2 2 HP 3 LP 4 VCA 5 PW 6 MGFreq 7 S&H Clock
        program("Fat Bass", {{kV1Wave, 1}, {kV1Scale, 0}, {kV2Wave, 1}, {kV2Scale, 0}, {kV2Lev, 70}, {kV2Pitch, 0.08f}, {kLpCut, 300}, {kLpPeak, 35}, {kLpEg2, 55},
                             {kE2Attack, 1}, {kE2Decay, 300}, {kE2Sustain, 50}, {kE2Release, 80}, {kDrive, 40}});
        program("Screaming Lead", {{kV1Wave, 1}, {kV2Wave, 1}, {kV2Pitch, 0.15f}, {kV2Lev, 60}, {kLpCut, 1800}, {kLpPeak, 82}, {kHpCut, 200}, {kHpPeak, 55}, {kLpEg2, 30},
                                   {kPorta, 60}, {kMode, 1}, {kP3Amt, 6}, {kDrive, 45}});
        program("Filter FM Growl", {{kV1Wave, 2}, {kV1Pw, 40}, {kV2Wave, 0}, {kV2Scale, 1}, {kV2Pitch, 7.0f}, {kV2Lev, 0}, {kP2Src, sVco2}, {kP2Dst, dLp}, {kP2Amt, 50},
                                    {kLpCut, 700}, {kLpPeak, 70}, {kLpEg2, 30}, {kDrive, 50}});
        program("S&H Bleeps", {{kV1Wave, 0}, {kV1Scale, 2}, {kLpCut, 3000}, {kLpPeak, 75}, {kMgFreq, 7.0f}, {kP1Src, sSH}, {kP1Dst, dLp}, {kP1Amt, 60}, {kP3Src, sSH}, {kP3Dst, dTotal},
                               {kP3Amt, 25}, {kE2Sustain, 100}, {kLpEg2, 0}});
        program("Wind Noise", {{kV1Wave, 3}, {kLpCut, 900}, {kLpPeak, 80}, {kHpCut, 300}, {kHpPeak, 40}, {kMgFreq, 0.15f}, {kLpMg, 60}, {kHpMg, -30}, {kE2Attack, 600},
                               {kE2Sustain, 100}, {kE2Release, 2000}, {kLpEg2, 0}});
        program("Siren", {{kV1Wave, 1}, {kV1Scale, 2}, {kMgFm, 100}, {kMgFreq, 0.6f}, {kMgWave, 50}, {kLpCut, 4000}, {kLpPeak, 40}, {kLpEg2, 0}, {kE2Sustain, 100}});
        program("Squelch Acid", {{kV1Wave, 1}, {kV1Scale, 1}, {kLpCut, 250}, {kLpPeak, 88}, {kLpEg2, 65}, {kE2Attack, 1}, {kE2Decay, 260}, {kE2Sustain, 20}, {kE2Release, 60},
                                 {kMode, 1}, {kPorta, 50}, {kDrive, 55}});
        program("Ring Clang", {{kV1Wave, 2}, {kV1Pw, 30}, {kV2Wave, 3}, {kV2Pitch, 5.3f}, {kV2Lev, 90}, {kV1Lev, 20}, {kLpCut, 5000}, {kLpPeak, 30}, {kLpEg2, 0}, {kE2Attack, 1},
                               {kE2Decay, 700}, {kE2Sustain, 0}, {kE2Release, 600}});
        program("MS Brass", {{kV1Wave, 1}, {kV2Wave, 0}, {kV2Pitch, 0.12f}, {kV2Lev, 80}, {kLpCut, 500}, {kLpPeak, 25}, {kLpEg2, 50}, {kE2Attack, 50}, {kE2Decay, 600},
                             {kE2Sustain, 70}, {kE1Delay, 200}, {kE1Attack, 300}, {kEg1Fm, 0}, {kMgFm, 8}, {kMode, 2}});
        program("Filter Kick", {{kV1Lev, 0}, {kV2Lev, 0}, {kLpCut, 50}, {kLpPeak, 100}, {kLpEg2, 55}, {kE2Attack, 0.5f}, {kE2Decay, 260}, {kE2Sustain, 0}, {kE2Release, 150},
                                {kKbdTrack, 0}, {kDrive, 50}, {kP1Amt, 0}, {kVolume, 4}});
        program("Pressure Pad Lead", {{kV1Wave, 2}, {kV1Pw, 30}, {kV2Wave, 1}, {kV2Pitch, 12.1f}, {kV2Lev, 40}, {kLpCut, 900}, {kLpPeak, 60}, {kLpEg2, 10}, {kP1Amt, 0},
                                      {kP2Src, sPress}, {kP2Dst, dLp}, {kP2Amt, 70}, {kP3Src, sPress}, {kP3Dst, dPw}, {kP3Amt, 50}, {kE2Sustain, 100}});
        program("Init", {{kLpCut, 18000}, {kLpPeak, 0}, {kLpEg2, 0}, {kP1Amt, 0}, {kE2Sustain, 100}, {kDrive, 0}});
    }

    double sr_ = 44100.0;
    Voice voices_[kMaxVoices];
    unsigned age_ = 0;
    bool sustain_ = false;
    int heldKeys_[16] = {}, held_ = 0;
    float lastNote_ = -1.0f, mgPh_ = 0.0f, shVal_ = 0.0f, pressure_ = 0.0f, pink_[3] = {};
    bool shPrev_ = false;
    uint32_t shRng_ = 2468u;
    float buf_[kMaxBlock];
    Smoother volume_;
    MidiControllers controllers_;
};

MPC_PLUGIN_ENTRY(MeltMS)
