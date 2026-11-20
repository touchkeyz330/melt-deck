// MeltQuad.cpp - a four-part performance instrument for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// Original design. Four parts (A-D), each its own synth with a Sound picked from 26 original
// sounds built on the band-limited wave set in waves.h. Layer them (all on Omni), split them
// across the keyboard (Key Low / Key High) or play each from its own MIDI channel. Per part:
// tuning, filter (LP/BP/HP) with envelope, ADSR, level, pan, low/high EQ, one-knob compressor
// and two sends. Shared FX: FX1 tempo-synced ping-pong delay, FX2 room reverb. Master and Width.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/adsr.h"
#include "dsp/svf.h"
#include "dsp/biquad.h"
#include "dsp/delay_line.h"
#include "dsp/smoother.h"
#include "dsp/midi_cc.h"
#include "waves.h"
#include "reverb.h"

#include <algorithm>
#include <mutex>

using namespace mpc::dsp;
static constexpr float kPiF = 3.14159265358979f;

static const std::vector<wr::Wave>& quadWaves()
{
    static std::vector<wr::Wave> w;
    static std::once_flag once;
    std::call_once(once, [] { w = wr::builtinWaves(); });
    return w;
}

// An original Sound: two wavetable oscillators (+ optional unison on osc 1), noise, pitch envelope.
struct Sound
{
    const char* name;
    int w1, w2;            // wave indices (see waves.h order)
    float semi2, detune;   // osc 2 offset (semitones) and detune (cents)
    float mix;             // osc 2 level
    int unison;            // 1..3 copies of osc 1 (spread across the stereo field)
    float spread;          // unison spread, cents
    float noise;           // noise level
    float pitchEnv, pitchMs;   // start offset (semitones) decaying over ms - 808s and plucks
    float gain;            // level trim so every Sound sits at a similar loudness
};

// wave order: 0 Sine 1 Triangle 2 Saw 3 Square 4 Pulse25 5 Pulse10 6 Organ 7 SoftOrgan 8 Hollow 9 BrightSaw 10 VoxAh 11 VoxEe
// 12 VoxOo 13 Glass 14 BellTone 15 Choir 16 FMBright 17 FMHollow 18 FMMetal 19 Sync2.5 20 Sync4.3 21 Fold 22 Digital 23 ClipSine
// 24 HalfSine 25 RampSq 26 CombSaw 27 OddSaw 28 Stairs 29 Buzz 30 Pluck 31 Metal
static const Sound kSounds[] = {
    {"Velvet Keys", 7, 14, 12.0f, 0.0f, 0.35f, 1, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {"Glass EP", 13, 0, 0.0f, 3.0f, 0.6f, 1, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {"Warm Pad", 2, 1, -12.0f, 0.0f, 0.5f, 3, 12.0f, 0.0f, 0.0f, 0.0f, 0.75f},
    {"Silk Strings", 9, 2, 0.0f, 7.0f, 0.5f, 3, 18.0f, 0.0f, 0.0f, 0.0f, 0.7f},
    {"Choir Ah", 10, 15, 0.0f, 6.0f, 0.7f, 2, 8.0f, 0.0f, 0.0f, 0.0f, 0.9f},
    {"Air Vox", 12, 11, 0.0f, 4.0f, 0.5f, 1, 0.0f, 0.05f, 0.0f, 0.0f, 1.0f},
    {"Hollow Flute", 8, 0, 12.0f, 0.0f, 0.2f, 1, 0.0f, 0.04f, 0.0f, 0.0f, 1.0f},
    {"Soul Brass", 2, 2, 0.0f, 9.0f, 0.9f, 1, 0.0f, 0.0f, 0.0f, 0.0f, 0.75f},
    {"Drawbar Organ", 6, 0, 12.0f, 0.0f, 0.3f, 1, 0.0f, 0.0f, 0.0f, 0.0f, 0.9f},
    {"FM Bell", 16, 14, 12.0f, 0.0f, 0.4f, 1, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {"Metal Keys", 18, 13, 0.0f, 0.0f, 0.5f, 1, 0.0f, 0.0f, 0.0f, 0.0f, 0.9f},
    {"Kalimba", 24, 30, 12.0f, 0.0f, 0.3f, 1, 0.0f, 0.0f, 0.3f, 15.0f, 1.0f},
    {"Pluck", 30, 2, 0.0f, 8.0f, 0.4f, 1, 0.0f, 0.0f, 0.2f, 10.0f, 1.0f},
    {"Sub Bass", 0, 1, 12.0f, 0.0f, 0.15f, 1, 0.0f, 0.0f, 0.0f, 0.0f, 1.2f},
    {"Saw Bass", 2, 3, -12.0f, 0.0f, 0.6f, 1, 0.0f, 0.0f, 0.0f, 0.0f, 0.8f},
    {"Buzz Bass", 29, 0, -12.0f, 0.0f, 0.5f, 1, 0.0f, 0.0f, 0.0f, 0.0f, 1.7f},
    {"Clip 808", 23, 0, 0.0f, 0.0f, 0.5f, 1, 0.0f, 0.0f, 12.0f, 45.0f, 1.2f},
    {"Square Lead", 3, 4, 0.0f, 6.0f, 0.5f, 1, 0.0f, 0.0f, 0.0f, 0.0f, 0.8f},
    {"Sync Lead", 20, 19, 0.0f, 5.0f, 0.6f, 1, 0.0f, 0.0f, 0.0f, 0.0f, 0.8f},
    {"Odd Lead", 27, 25, 7.0f, 0.0f, 0.3f, 1, 0.0f, 0.0f, 0.0f, 0.0f, 0.85f},
    {"Digital Vox", 22, 11, 0.0f, 4.0f, 0.5f, 1, 0.0f, 0.0f, 0.0f, 0.0f, 0.9f},
    {"Comb Pad", 26, 8, 12.0f, 0.0f, 0.4f, 3, 10.0f, 0.0f, 0.0f, 0.0f, 0.75f},
    {"Stairs Arp", 28, 5, 12.0f, 0.0f, 0.3f, 1, 0.0f, 0.0f, 0.0f, 0.0f, 0.85f},
    {"Fold Grit", 21, 2, 0.0f, 10.0f, 0.4f, 1, 0.0f, 0.0f, 0.0f, 0.0f, 0.85f},
    {"Noise Air", 0, 0, 0.0f, 0.0f, 0.0f, 1, 0.0f, 1.0f, 0.0f, 0.0f, 0.5f},
    {"Basic Saw", 2, 2, 0.0f, 0.0f, 0.0f, 1, 0.0f, 0.0f, 0.0f, 0.0f, 0.9f},
};
static constexpr int kNumSounds = (int)(sizeof(kSounds) / sizeof(kSounds[0]));

class MeltQuad : public mpc::Plugin
{
public:
    // per-part parameter block
    enum PartParam { pOn = 0, pChan, pSound, pLevel, pPan, pSemi, pFine, pKeyLo, pKeyHi, pFType, pCutoff, pReso, pEnvAmt,
                     pA, pD, pS, pR, pSend1, pSend2, pEqLo, pEqHi, pComp, kPP };
    enum Global { kParts = 4, kGlob = kParts * kPP, kMaster = kGlob, kWidth, kDlyTime, kDlyFb, kDlyRet, kRevSize, kRevDamp, kRevRet, kNumParams };
    static constexpr int P(int part, int p) { return part * kPP + p; }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt Quad";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'Y', 'm', 'I');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    MeltQuad() : mpc::Plugin(makeInfo()), waves_(quadWaves())
    {
        using namespace mpc;
        std::vector<std::string> chans = {"Omni"};
        for (int c = 1; c <= 16; ++c) chans.push_back(std::to_string(c));
        std::vector<std::string> sounds;
        for (const auto& s : kSounds) sounds.push_back(s.name);
        static const int defSound[4] = {0, 2, 13, 4};
        for (int p = 0; p < kParts; ++p)
        {
            const std::string L(1, (char)('A' + p)), id(1, (char)('a' + p));
            auto nm = [&](const char* n) { return L + " " + n; };
            auto ident = [&](const char* n) { return id + n; };
            addParam(boolParam(ident("on").c_str(), nm("On").c_str(), p < 2));
            addParam(choiceParam(ident("ch").c_str(), nm("Channel").c_str(), chans, 0));
            addParam(choiceParam(ident("snd").c_str(), nm("Sound").c_str(), sounds, defSound[p]));
            addParam(floatParam(ident("lvl").c_str(), nm("Level").c_str(), -40.0f, 6.0f, -6.0f, "dB", 1));
            addParam(floatParam(ident("pan").c_str(), nm("Pan").c_str(), -100.0f, 100.0f, 0.0f, "", 0));
            addParam(intParam(ident("semi").c_str(), nm("Semi").c_str(), -24, 24, 0, "st"));
            addParam(floatParam(ident("fine").c_str(), nm("Fine").c_str(), -50.0f, 50.0f, 0.0f, "ct", 0));
            addParam(intParam(ident("klo").c_str(), nm("Key Low").c_str(), 0, 127, 0, ""));
            addParam(intParam(ident("khi").c_str(), nm("Key High").c_str(), 0, 127, 127, ""));
            addParam(choiceParam(ident("ft").c_str(), nm("Filter").c_str(), {"LP", "BP", "HP"}, 0));
            addParam(logParam(ident("cut").c_str(), nm("Cutoff").c_str(), 20.0f, 20000.0f, 8000.0f, "Hz", 0));
            addParam(floatParam(ident("res").c_str(), nm("Reso").c_str(), 0.0f, 100.0f, 10.0f, "%", 0));
            addParam(floatParam(ident("env").c_str(), nm("Env Amt").c_str(), -100.0f, 100.0f, 0.0f, "%", 0));
            addParam(logParam(ident("a").c_str(), nm("Attack").c_str(), 0.5f, 10000.0f, 3.0f, "ms", 0));
            addParam(logParam(ident("d").c_str(), nm("Decay").c_str(), 1.0f, 10000.0f, 600.0f, "ms", 0));
            addParam(floatParam(ident("s").c_str(), nm("Sustain").c_str(), 0.0f, 100.0f, 80.0f, "%", 0));
            addParam(logParam(ident("r").c_str(), nm("Release").c_str(), 1.0f, 10000.0f, 400.0f, "ms", 0));
            addParam(floatParam(ident("fx1").c_str(), nm("Delay Send").c_str(), 0.0f, 100.0f, 0.0f, "%", 0));
            addParam(floatParam(ident("fx2").c_str(), nm("Reverb Send").c_str(), 0.0f, 100.0f, 20.0f, "%", 0));
            addParam(floatParam(ident("eql").c_str(), nm("Low EQ").c_str(), -12.0f, 12.0f, 0.0f, "dB", 1));
            addParam(floatParam(ident("eqh").c_str(), nm("High EQ").c_str(), -12.0f, 12.0f, 0.0f, "dB", 1));
            addParam(floatParam(ident("cmp").c_str(), nm("Comp").c_str(), 0.0f, 100.0f, 0.0f, "%", 0));
        }
        addParam(floatParam("mst", "Master", -40.0f, 6.0f, -3.0f, "dB", 1));
        addParam(floatParam("wid", "Width", 0.0f, 200.0f, 100.0f, "%", 0));
        addParam(choiceParam("dt", "Delay Time", {"1/16", "1/8T", "1/8", "1/8D", "1/4", "1/4D", "1/2"}, 3));
        addParam(floatParam("dfb", "Delay Feedback", 0.0f, 95.0f, 35.0f, "%", 0));
        addParam(floatParam("drt", "Delay Return", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam("rsz", "Reverb Size", 0.3f, 8.0f, 2.2f, "s", 1));
        addParam(floatParam("rdm", "Reverb Damp", 0.0f, 100.0f, 40.0f, "%", 0));
        addParam(floatParam("rrt", "Reverb Return", 0.0f, 100.0f, 50.0f, "%", 0));
        if (numParams() != kNumParams) mpc::log("MeltQuad: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);
        makePrograms();
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        for (auto& v : voices_) { v.amp.setSampleRate(sampleRate); v.flt.setSampleRate(sampleRate); v.fltR.setSampleRate(sampleRate); }
        dlyL_.prepare((int)(3.1 * sampleRate) + 4);
        dlyR_.prepare((int)(3.1 * sampleRate) + 4);
        reverb_.prepare(sampleRate);
        controllers_.prepare(sampleRate / kCtl, 10.0f);
        master_.setTime(20.0f, sampleRate);
        for (auto& pt : parts_) { pt.level.setTime(20.0f, sampleRate); }
        compAtk_ = std::exp(-1.0f / (0.005f * (float)sampleRate));
        compRel_ = std::exp(-1.0f / (0.120f * (float)sampleRate));
        reset();
    }

    void reset() override
    {
        for (auto& v : voices_) v.kill();
        dlyL_.reset(); dlyR_.reset(); reverb_.reset();
        dlyLp_[0] = dlyLp_[1] = 0.0f;
        for (auto& pt : parts_) { pt.eqLo.reset(); pt.eqHi.reset(); pt.env = 0.0f; pt.level.snap(0.0f); pt.eqKey = -999.0f; }
        master_.snap(dbToGain(get(kMaster)));
        controllers_.reset();
        age_ = 0; sustain_ = false;
        rng_ = 0x2468ACEu;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        readSettings(ctx);
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        int pos = 0;
        const int nf = ctx.numFrames;
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            const int at = std::min(std::max(ev.sampleOffset, pos), nf);
            while (pos < at) { const int n = std::min(kMaxBlock, at - pos); render(L, R, pos, n); pos += n; }
            handleMidi(ev);
        }
        while (pos < nf) { const int n = std::min(kMaxBlock, nf - pos); render(L, R, pos, n); pos += n; }
    }

private:
    static constexpr int kMaxVoices = 32, kCtl = 16, kMaxBlock = 256;

    struct Voice
    {
        bool active = false, held = false, sustained = false;
        int part = 0, note = 0, sound = 0;
        unsigned age = 0;
        float vel = 1.0f, ph[3] = {}, ph2 = 0.0f, pe = 0.0f;
        ADSR amp;
        SVF flt, fltR;
        void kill() { active = held = sustained = false; amp.reset(); flt.reset(); fltR.reset(); }
    };

    struct PartSettings
    {
        bool on = true; int chan = 0, sound = 0, ftype = 0, klo = 0, khi = 127;
        float lvl = 0.5f, panL = 0.707f, panR = 0.707f, tune = 0, cut = 8000, reso = 0.1f, envAmt = 0;
        float a = 3, d = 600, s = 0.8f, r = 400, send1 = 0, send2 = 0.2f, eqLo = 0, eqHi = 0, comp = 0;
    };

    struct PartState
    {
        StereoBiquad eqLo, eqHi;
        float eqKey = -999.0f, env = 0.0f;
        Smoother level;
        float bufL[kMaxBlock], bufR[kMaxBlock];
    };

    void readSettings(mpc::ProcessContext& ctx)
    {
        for (int p = 0; p < kParts; ++p)
        {
            PartSettings& s = ps_[p];
            s.on = getBool(P(p, pOn)); s.chan = getInt(P(p, pChan)); s.sound = mpc::clamp(getInt(P(p, pSound)), 0, kNumSounds - 1);
            s.lvl = dbToGain(get(P(p, pLevel)));
            const float pan = get(P(p, pPan)) * 0.01f, th = (pan + 1.0f) * 0.25f * kPiF;
            s.panL = std::cos(th) * 1.414f; s.panR = std::sin(th) * 1.414f;
            s.tune = (float)getInt(P(p, pSemi)) + get(P(p, pFine)) * 0.01f;
            s.klo = getInt(P(p, pKeyLo)); s.khi = getInt(P(p, pKeyHi));
            s.ftype = getInt(P(p, pFType)); s.cut = get(P(p, pCutoff)); s.reso = get(P(p, pReso)) * 0.01f; s.envAmt = get(P(p, pEnvAmt)) * 0.01f;
            s.a = get(P(p, pA)); s.d = get(P(p, pD)); s.s = get(P(p, pS)) * 0.01f; s.r = get(P(p, pR));
            s.send1 = get(P(p, pSend1)) * 0.01f; s.send2 = get(P(p, pSend2)) * 0.01f;
            s.eqLo = get(P(p, pEqLo)); s.eqHi = get(P(p, pEqHi)); s.comp = get(P(p, pComp)) * 0.01f;
            PartState& st = parts_[p];
            const float key = s.eqLo * 100.0f + s.eqHi;
            if (key != st.eqKey)
            {
                st.eqKey = key;
                st.eqLo.setCoeffs(BiquadCoeffs::lowShelf(sr_, 200.0f, 0.7071f, s.eqLo));
                st.eqHi.setCoeffs(BiquadCoeffs::highShelf(sr_, 6000.0f, 0.7071f, s.eqHi));
            }
            st.level.setTarget(s.on ? s.lvl : 0.0f);
        }
        for (auto& v : voices_) if (v.active) { const PartSettings& s = ps_[v.part]; v.amp.set(s.a, s.d, s.s, s.r); }
        master_.setTarget(dbToGain(get(kMaster)));
        width_ = get(kWidth) * 0.01f;
        static const float beats[7] = {0.25f, 1.0f / 3.0f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
        const double bpm = (ctx.transport.tempoValid && ctx.transport.bpm > 20.0) ? ctx.transport.bpm : 120.0;
        dlySamples_ = std::min((float)(3.0 * sr_), (float)(60.0 / bpm * beats[mpc::clamp(getInt(kDlyTime), 0, 6)] * sr_));
        dlyFb_ = get(kDlyFb) * 0.01f; dlyRet_ = get(kDlyRet) * 0.01f;
        reverb_.set(get(kRevSize), get(kRevDamp) * 0.01f); revRet_ = get(kRevRet) * 0.01f;
    }

    // ---- MIDI -------------------------------------------------------------------------------
    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn()) noteOn(ev.channel(), ev.note(), ev.velocity());
        else if (ev.isNoteOff()) noteOff(ev.channel(), ev.note());
        else if (ev.isAllNotesOff()) { for (auto& v : voices_) if (v.active) { v.held = v.sustained = false; v.amp.noteOff(); } }
        else if (ev.isController() && ev.controller() == cc::Sustain)
        {
            sustain_ = ev.controllerValue() >= 64;
            if (!sustain_) for (auto& v : voices_) if (v.sustained) { v.sustained = false; v.amp.noteOff(); }
        }
        else controllers_.handle(ev);
    }

    bool partHears(int p, int ch, int note) const
    {
        const PartSettings& s = ps_[p];
        if (!s.on) return false;
        if (s.chan != 0 && s.chan - 1 != ch) return false;
        const int lo = std::min(s.klo, s.khi), hi = std::max(s.klo, s.khi);
        return note >= lo && note <= hi;
    }

    void noteOn(int ch, int note, int vel)
    {
        for (int p = 0; p < kParts; ++p)
        {
            if (!partHears(p, ch, note)) continue;
            Voice* best = nullptr;
            for (auto& v : voices_) if (!v.active) { best = &v; break; }
            if (!best) { best = &voices_[0]; for (auto& v : voices_) if (v.age < best->age) best = &v; }   // steal the oldest
            Voice& v = *best;
            const PartSettings& s = ps_[p];
            v.kill();
            v.active = v.held = true; v.part = p; v.note = note; v.sound = s.sound; v.age = ++age_;
            v.vel = (float)vel / 127.0f;
            for (int u = 0; u < 3; ++u) v.ph[u] = rnd01() * (kSounds[s.sound].unison > 1 ? 1.0f : 0.0f);
            v.ph2 = 0.0f; v.pe = 1.0f;
            v.amp.set(s.a, s.d, s.s, s.r);
            v.amp.noteOn();
        }
    }

    void noteOff(int ch, int note)
    {
        for (auto& v : voices_)
            if (v.active && v.held && v.note == note && (ps_[v.part].chan == 0 || ps_[v.part].chan - 1 == ch))
            {
                v.held = false;
                if (sustain_) v.sustained = true; else v.amp.noteOff();
            }
    }

    // ---- audio ------------------------------------------------------------------------------
    inline float rnd01() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return (float)(rng_ >> 8) / 16777216.0f; }
    inline float readWave(int idx, int lv, float ph) const
    {
        const float* tb = waves_[(size_t)idx].level(lv);
        const float p = ph * (float)wr::kTable; const int i = (int)p; const float f = p - (float)i;
        return tb[i] + f * (tb[i + 1] - tb[i]);
    }
    static float softClip(float x) { return x / (1.0f + std::fabs(x) * 0.25f) * 1.0f; }

    void render(float* L, float* R, int off, int n)
    {
        for (auto& pt : parts_) { std::fill(pt.bufL, pt.bufL + n, 0.0f); std::fill(pt.bufR, pt.bufR + n, 0.0f); }
        for (int c = 0; c < n; c += kCtl) renderVoices(c, std::min(kCtl, n - c));
        // part strips -> mix + sends
        float sendD[kMaxBlock], sendR[kMaxBlock], sendRR[kMaxBlock], mixL[kMaxBlock], mixR[kMaxBlock];
        std::fill(sendD, sendD + n, 0.0f); std::fill(sendR, sendR + n, 0.0f); std::fill(sendRR, sendRR + n, 0.0f);
        std::fill(mixL, mixL + n, 0.0f); std::fill(mixR, mixR + n, 0.0f);
        for (int p = 0; p < kParts; ++p)
        {
            PartState& st = parts_[p];
            const PartSettings& s = ps_[p];
            const float thr = dbToGain(-6.0f - 24.0f * s.comp), ratio = 1.0f + 5.0f * s.comp;
            const float makeup = dbToGain((6.0f + 24.0f * s.comp) * (1.0f - 1.0f / ratio) * 0.5f);
            for (int i = 0; i < n; ++i)
            {
                float l = st.bufL[i], r = st.bufR[i];
                if (s.eqLo != 0.0f) st.eqLo.process(l, r);
                if (s.eqHi != 0.0f) st.eqHi.process(l, r);
                if (s.comp > 0.0f)
                {
                    const float lvl = std::max(std::fabs(l), std::fabs(r));
                    st.env = lvl > st.env ? compAtk_ * st.env + (1.0f - compAtk_) * lvl : compRel_ * st.env + (1.0f - compRel_) * lvl;
                    float g = 1.0f;
                    if (st.env > thr) g = std::pow(st.env / thr, 1.0f / ratio - 1.0f);
                    l *= g * makeup; r *= g * makeup;
                }
                const float gv = st.level.process();
                l *= gv * s.panL; r *= gv * s.panR;
                mixL[i] += l; mixR[i] += r;
                sendD[i] += (l + r) * 0.5f * s.send1;
                sendR[i] += l * s.send2; sendRR[i] += r * s.send2;
            }
        }
        const float dampC = 0.35f;
        for (int i = 0; i < n; ++i)
        {
            // FX1: ping-pong delay (input enters on the left, bounces L <-> R, darkens with each repeat)
            const float tl = dlyL_.readLinear(dlySamples_), tr = dlyR_.readLinear(dlySamples_);
            dlyLp_[0] += dampC * (tl - dlyLp_[0]); dlyLp_[1] += dampC * (tr - dlyLp_[1]);
            dlyL_.write(sendD[i] + dlyLp_[1] * dlyFb_);
            dlyR_.write(dlyLp_[0] * dlyFb_);
            // FX2: room reverb
            float rl, rr;
            reverb_.process(sendR[i], sendRR[i], rl, rr);
            float l = mixL[i] + tl * dlyRet_ + rl * revRet_;
            float r = mixR[i] + tr * dlyRet_ + rr * revRet_;
            // width (mid/side) + master
            const float m = 0.5f * (l + r), sd = 0.5f * (l - r) * width_;
            const float gv = master_.process();
            l = softClip((m + sd) * gv); r = softClip((m - sd) * gv);
            if (std::fabs(l) < 1.0e-20f) l = 0.0f;
            if (std::fabs(r) < 1.0e-20f) r = 0.0f;
            L[off + i] = sanitize(l);
            if (R) R[off + i] = sanitize(r);
        }
    }

    void renderVoices(int off, int n)
    {
        controllers_.tick();
        const float bend = controllers_.pitchBend() * 2.0f;
        vibPh_ += 5.5f * (float)n / (float)sr_; if (vibPh_ >= 1.0f) vibPh_ -= 1.0f;
        const float vib = controllers_.modWheel() * 0.35f * std::sin(2.0f * kPiF * vibPh_);   // mod wheel = vibrato
        const float dtc = (float)n / (float)sr_;
        static const float uPan[3][2] = {{0.707f, 0.707f}, {0.95f, 0.31f}, {0.31f, 0.95f}};
        static const float uDet[3] = {0.0f, -1.0f, 1.0f};
        for (auto& v : voices_)
        {
            if (!v.active) continue;
            const Sound& sd = kSounds[v.sound];
            const PartSettings& s = ps_[v.part];
            PartState& st = parts_[v.part];
            if (sd.pitchMs > 0.0f) v.pe *= std::exp(-dtc / (sd.pitchMs * 0.001f));
            const float note = (float)v.note + s.tune + bend + vib + sd.pitchEnv * v.pe;
            const float base = 440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f) / (float)sr_;
            float inc[3];
            const int nu = mpc::clamp(sd.unison, 1, 3);
            for (int u = 0; u < nu; ++u) inc[u] = std::min(0.45f, base * std::pow(2.0f, uDet[u] * sd.spread / 1200.0f));
            const float inc2 = std::min(0.45f, base * std::pow(2.0f, (sd.semi2 * 100.0f + sd.detune) / 1200.0f));
            int lv[3]; for (int u = 0; u < nu; ++u) lv[u] = wr::levelFor(inc[u]);
            const int lv2 = wr::levelFor(inc2);
            // filter: cutoff follows key a little, the envelope, and velocity
            const float envL = v.amp.level();
            const float fc = mpc::clamp(s.cut * std::pow(2.0f, s.envAmt * envL * 6.0f + (v.vel - 0.7f) * 1.2f + ((float)v.note - 60.0f) / 48.0f), 20.0f, 0.45f * (float)sr_);
            v.flt.set(fc, s.reso * 0.95f);
            if (nu > 1) v.fltR.set(fc, s.reso * 0.95f);
            const int fmode = s.ftype == 0 ? 0 : s.ftype == 1 ? 1 : 2;
            const float vg = (0.4f + 0.6f * v.vel) * sd.gain * 0.4f / std::sqrt((float)nu);
            for (int i = 0; i < n; ++i)
            {
                float l = 0.0f, r = 0.0f;
                for (int u = 0; u < nu; ++u)
                {
                    const float o = readWave(sd.w1, lv[u], v.ph[u]);
                    if (nu == 1) { l += o; r += o; } else { l += o * uPan[u][0] * 1.414f; r += o * uPan[u][1] * 1.414f; }
                    v.ph[u] += inc[u]; if (v.ph[u] >= 1.0f) v.ph[u] -= 1.0f;
                }
                if (sd.mix > 0.0f)
                {
                    const float o2 = readWave(sd.w2, lv2, v.ph2) * sd.mix;
                    l += o2; r += o2;
                    v.ph2 += inc2; if (v.ph2 >= 1.0f) v.ph2 -= 1.0f;
                }
                if (sd.noise > 0.0f) { const float nz = (rnd01() * 2.0f - 1.0f) * sd.noise; l += nz; r += nz; }
                // unison sounds are stereo: filter each side; single-oscillator voices are mono
                l = v.flt.process(l, fmode);
                r = nu > 1 ? v.fltR.process(r, fmode) : l;
                const float a = v.amp.process() * vg;
                st.bufL[off + i] += l * a;
                st.bufR[off + i] += r * a;
            }
            if (!v.amp.isActive()) v.kill();
        }
    }

    // ---- presets (all original) ------------------------------------------------------------
    void makePrograms()
    {
        auto program = [&](const char* name, std::initializer_list<std::pair<int, float>> o)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& p : o) v[(size_t)p.first] = p.second;
            addProgram(name, std::move(v));
        };
        auto S = [](const char* n) { for (int i = 0; i < kNumSounds; ++i) if (std::string(kSounds[i].name) == n) return (float)i; return 0.0f; };
        const int A = 0, B = 1, C = 2, D = 3;
        program("Velvet Session", {{P(A, pSound), S("Velvet Keys")}, {P(A, pLevel), -5}, {P(A, pR), 900},
                                   {P(B, pSound), S("Warm Pad")}, {P(B, pLevel), -12}, {P(B, pA), 400}, {P(B, pR), 1500}, {P(B, pCutoff), 2500}, {P(B, pPan), -20},
                                   {P(C, pOn), 1}, {P(C, pSound), S("Sub Bass")}, {P(C, pKeyHi), 52}, {P(C, pLevel), -6}, {P(C, pSend2), 0},
                                   {P(A, pSend1), 15}});
        program("Bell Split", {{P(A, pSound), S("FM Bell")}, {P(A, pKeyLo), 60}, {P(A, pD), 1500}, {P(A, pS), 0}, {P(A, pR), 1200}, {P(A, pSend1), 30},
                               {P(B, pSound), S("Saw Bass")}, {P(B, pKeyHi), 59}, {P(B, pCutoff), 600}, {P(B, pReso), 30}, {P(B, pEnvAmt), 40}, {P(B, pD), 300},
                               {P(B, pS), 40}, {P(B, pSend2), 0}, {P(B, pComp), 40}});
        program("Choir Strings", {{P(A, pSound), S("Choir Ah")}, {P(A, pA), 300}, {P(A, pR), 1500}, {P(A, pSend2), 45},
                                  {P(B, pSound), S("Silk Strings")}, {P(B, pA), 500}, {P(B, pR), 1800}, {P(B, pCutoff), 4000}, {P(B, pLevel), -10}, {P(B, pSend2), 40},
                                  {kRevSize, 3.5f}});
        program("Trap Bells 808", {{P(A, pSound), S("Kalimba")}, {P(A, pKeyLo), 60}, {P(A, pD), 700}, {P(A, pS), 0}, {P(A, pR), 600}, {P(A, pSend1), 25},
                                   {P(B, pSound), S("Clip 808")}, {P(B, pKeyHi), 59}, {P(B, pD), 1800}, {P(B, pS), 0}, {P(B, pR), 300}, {P(B, pSend2), 0},
                                   {P(B, pLevel), -3}, {P(B, pComp), 30}, {P(B, pCutoff), 3000},
                                   {P(C, pOn), 1}, {P(C, pSound), S("Glass EP")}, {P(C, pKeyLo), 60}, {P(C, pLevel), -14}, {P(C, pSemi), 12}, {P(C, pD), 900}, {P(C, pS), 0}});
        program("Soul Brass Stack", {{P(A, pSound), S("Soul Brass")}, {P(A, pCutoff), 1200}, {P(A, pEnvAmt), 50}, {P(A, pA), 30}, {P(A, pD), 500}, {P(A, pS), 70},
                                     {P(B, pSound), S("Drawbar Organ")}, {P(B, pLevel), -14}, {P(B, pSemi), -12}, {P(B, pPan), 30}});
        program("Vox Lead", {{P(A, pSound), S("Digital Vox")}, {P(A, pSend1), 30}, {P(A, pSend2), 25},
                             {P(B, pSound), S("Square Lead")}, {P(B, pLevel), -14}, {P(B, pSemi), -12}, {P(B, pCutoff), 2000}});
        program("Four Layer Pad", {{P(A, pSound), S("Warm Pad")}, {P(A, pA), 600}, {P(A, pR), 2500}, {P(A, pPan), -40},
                                   {P(B, pSound), S("Comb Pad")}, {P(B, pA), 900}, {P(B, pR), 2500}, {P(B, pPan), 40}, {P(B, pLevel), -10},
                                   {P(C, pOn), 1}, {P(C, pSound), S("Air Vox")}, {P(C, pA), 1200}, {P(C, pR), 3000}, {P(C, pLevel), -12}, {P(C, pSemi), 12},
                                   {P(D, pOn), 1}, {P(D, pSound), S("Noise Air")}, {P(D, pFType), 1}, {P(D, pCutoff), 3000}, {P(D, pReso), 50}, {P(D, pA), 2000},
                                   {P(D, pR), 3000}, {P(D, pLevel), -20}, {kRevSize, 5.0f}, {kWidth, 140}});
        program("Pluck Keys", {{P(A, pSound), S("Pluck")}, {P(A, pD), 450}, {P(A, pS), 0}, {P(A, pR), 400}, {P(A, pCutoff), 3000}, {P(A, pEnvAmt), 40}, {P(A, pSend1), 35},
                               {P(B, pSound), S("Metal Keys")}, {P(B, pLevel), -16}, {P(B, pD), 700}, {P(B, pS), 0}});
        program("Init", {{P(A, pSound), S("Basic Saw")}, {P(A, pSend2), 0}, {P(B, pOn), 0}});
    }

    const std::vector<wr::Wave>& waves_;
    double sr_ = 44100.0;
    Voice voices_[kMaxVoices];
    PartSettings ps_[kParts];
    PartState parts_[kParts];
    unsigned age_ = 0;
    bool sustain_ = false;
    uint32_t rng_ = 0x2468ACEu;
    float vibPh_ = 0.0f, width_ = 1.0f, dlySamples_ = 10000.0f, dlyFb_ = 0.35f, dlyRet_ = 0.5f, revRet_ = 0.5f, dlyLp_[2] = {};
    float compAtk_ = 0.99f, compRel_ = 0.999f;
    DelayLine dlyL_, dlyR_;
    Reverb reverb_;
    Smoother master_;
    MidiControllers controllers_;
};

MPC_PLUGIN_ENTRY(MeltQuad)
