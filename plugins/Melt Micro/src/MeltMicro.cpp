// MeltMicro.cpp - a compact analog-modelling synth for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// An original design in the spirit of the small, much-loved 4-voice analog-modelling
// keyboards of the early 2000s. No code, waves, presets or names are taken from any product.
//
//   OSC 1: Saw / Square / Tri / Sine / VOX (vowel morph) / DIGITAL (32 waves) / CROSS (sine,
//          frequency-modulated by osc 2) / NOISE. Control 1 shapes the wave (see README),
//          Control 2 = how much LFO 1 moves Control 1.
//   OSC 2: Saw / Square / Tri, Semi, Tune, MOD: Off / Ring / Sync / Ring+Sync.
//   MIXER: osc 1, osc 2, noise.   FILTER: 24 LPF (saturating ladder), 12 LPF, 12 BPF, 12 HPF,
//   EG 1 amount, key track.  AMP: EG 2, level, pan, distortion.  LFO 1 / LFO 2 (free or synced).
//   VIRTUAL PATCH: 4 slots, source -> destination with amount.
//   ARP: Up, Down, Alt 1, Alt 2, Random, Trigger; 1-4 octaves; synced rate; gate.
//   FX: Mod FX (Chorus/Flanger, Ensemble, Phaser) -> Delay (Stereo or Cross) -> EQ (low/high shelf).
// Classic 4-voice polyphony by default (Voices goes up to 8).

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/svf.h"
#include "dsp/adsr.h"
#include "dsp/biquad.h"
#include "dsp/smoother.h"
#include "dsp/delay_line.h"
#include "dsp/midi_cc.h"
#include "waves.h"

#include <algorithm>
#include <mutex>

using namespace mpc::dsp;
static constexpr float kPiF = 3.14159265358979f;

static const std::vector<wr::Wave>& microWaves()
{
    static std::vector<wr::Wave> w;
    static std::once_flag once;
    std::call_once(once, [] { w = wr::builtinWaves(); });
    return w;
}

class MeltMicro : public mpc::Plugin
{
public:
    enum Params
    {
        kO1Wave = 0, kO1C1, kO1C2, kO2Wave, kO2Mod, kO2Semi, kO2Tune, kO1Lev, kO2Lev, kNoiseLev, kPorta,
        kFType, kCutoff, kReso, kEg1Int, kKeyTrk, kLevel, kPan, kDist, kMode, kVoices, kBend,
        kE1A, kE1D, kE1S, kE1R, kE2A, kE2D, kE2S, kE2R, kL1Wave, kL1Rate, kL1Sync, kL2Wave, kL2Rate, kL2Sync,
        kP1Src, kP1Dst, kP1Amt, kP2Src, kP2Dst, kP2Amt, kP3Src, kP3Dst, kP3Amt, kP4Src, kP4Dst, kP4Amt,
        kArpOn, kArpType, kArpRate, kArpRange, kArpGate,
        kModType, kModSpeed, kModDepth, kDlyType, kDlyTime, kDlyDepth, kDlyMix, kEqLow, kEqHigh,
        kNumParams
    };
    enum Src { sEG1, sEG2, sLFO1, sLFO2, sVel, sKey, sWheel };
    enum Dst { dPitch, dOsc2, dCtrl1, dNoise, dCutoff, dAmp, dPan, dLfo2 };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt Micro";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'r', 'C', '9');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    MeltMicro() : mpc::Plugin(makeInfo()), waves_(microWaves())
    {
        using namespace mpc;
        std::vector<std::string> sync = {"Free"};
        for (const char* d : {"1/16", "1/8", "1/4", "1/2", "1 Bar", "2 Bars", "4 Bars"}) sync.push_back(d);
        const std::vector<std::string> srcs = {"EG1", "EG2", "LFO1", "LFO2", "Velocity", "Key Track", "Mod Wheel"};
        const std::vector<std::string> dsts = {"Pitch", "Osc2 Tune", "Osc1 Ctrl1", "Noise", "Cutoff", "Amp", "Pan", "LFO2 Rate"};
        addParam(choiceParam("o1w", "Osc1 Wave", {"Saw", "Square", "Tri", "Sine", "Vox", "Digital", "Cross", "Noise"}, 0));
        addParam(floatParam("o1c1", "Osc1 Ctrl1", 0.0f, 100.0f, 0.0f, "", 0));
        addParam(floatParam("o1c2", "Osc1 Ctrl2", 0.0f, 100.0f, 0.0f, "", 0));
        addParam(choiceParam("o2w", "Osc2 Wave", {"Saw", "Square", "Tri"}, 0));
        addParam(choiceParam("o2m", "Osc Mod", {"Off", "Ring", "Sync", "Ring+Sync"}, 0));
        addParam(intParam("o2semi", "Osc2 Semi", -24, 24, 0, "st"));
        addParam(floatParam("o2tune", "Osc2 Tune", -63.0f, 63.0f, 0.0f, "ct", 0));
        addParam(floatParam("o1lev", "Osc1 Level", 0.0f, 127.0f, 127.0f, "", 0));
        addParam(floatParam("o2lev", "Osc2 Level", 0.0f, 127.0f, 0.0f, "", 0));
        addParam(floatParam("nlev", "Noise Level", 0.0f, 127.0f, 0.0f, "", 0));
        addParam(floatParam("porta", "Portamento", 0.0f, 2000.0f, 0.0f, "ms", 0, 0.3f));
        addParam(choiceParam("ft", "Filter Type", {"24 LPF", "12 LPF", "12 BPF", "12 HPF"}, 0));
        addParam(logParam("cut", "Cutoff", 20.0f, 20000.0f, 3000.0f, "Hz", 0));
        addParam(floatParam("res", "Resonance", 0.0f, 100.0f, 15.0f, "%", 0));
        addParam(floatParam("eg1i", "EG1 Int", -63.0f, 63.0f, 20.0f, "", 0));
        addParam(floatParam("kbd", "Key Track", -63.0f, 63.0f, 32.0f, "", 0));
        addParam(floatParam("lev", "Level", 0.0f, 127.0f, 100.0f, "", 0));
        addParam(floatParam("pan", "Pan", -63.0f, 63.0f, 0.0f, "", 0));
        addParam(boolParam("dist", "Distortion", false));
        addParam(choiceParam("mode", "Voice Mode", {"Poly", "Mono", "Legato"}, 0));
        addParam(intParam("voices", "Voices", 1, kMaxVoices, 4));
        addParam(intParam("bend", "Bend Range", 0, 12, 2, "st"));
        addParam(logParam("e1a", "EG1 Attack", 0.5f, 10000.0f, 2.0f, "ms", 0));
        addParam(logParam("e1d", "EG1 Decay", 1.0f, 10000.0f, 500.0f, "ms", 0));
        addParam(floatParam("e1s", "EG1 Sustain", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(logParam("e1r", "EG1 Release", 1.0f, 10000.0f, 300.0f, "ms", 0));
        addParam(logParam("e2a", "EG2 Attack", 0.5f, 10000.0f, 2.0f, "ms", 0));
        addParam(logParam("e2d", "EG2 Decay", 1.0f, 10000.0f, 800.0f, "ms", 0));
        addParam(floatParam("e2s", "EG2 Sustain", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(logParam("e2r", "EG2 Release", 1.0f, 10000.0f, 200.0f, "ms", 0));
        addParam(choiceParam("l1w", "LFO1 Wave", {"Saw", "Square", "Tri", "S&H"}, 2));
        addParam(logParam("l1r", "LFO1 Rate", 0.05f, 30.0f, 4.0f, "Hz", 2));
        addParam(choiceParam("l1s", "LFO1 Sync", sync, 0));
        addParam(choiceParam("l2w", "LFO2 Wave", {"Saw", "Square", "Sine", "S&H"}, 2));
        addParam(logParam("l2r", "LFO2 Rate", 0.05f, 30.0f, 5.5f, "Hz", 2));
        addParam(choiceParam("l2s", "LFO2 Sync", sync, 0));
        static const int defSrc[4] = {sLFO2, sLFO1, sVel, sWheel}, defDst[4] = {dPitch, dCutoff, dAmp, dCutoff};
        for (int p = 0; p < 4; ++p)
        {
            const std::string n = std::to_string(p + 1);
            addParam(choiceParam(("p" + n + "s").c_str(), ("Patch" + n + " Src").c_str(), srcs, defSrc[p]));
            addParam(choiceParam(("p" + n + "d").c_str(), ("Patch" + n + " Dst").c_str(), dsts, defDst[p]));
            addParam(floatParam(("p" + n + "a").c_str(), ("Patch" + n + " Amt").c_str(), -63.0f, 63.0f, 0.0f, "", 0));
        }
        addParam(boolParam("arp", "Arp", false));
        addParam(choiceParam("arpt", "Arp Type", {"Up", "Down", "Alt 1", "Alt 2", "Random", "Trigger"}, 0));
        addParam(choiceParam("arpr", "Arp Rate", {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"}, 3));
        addParam(intParam("arpo", "Arp Range", 1, 4, 1, "oct"));
        addParam(floatParam("arpg", "Arp Gate", 5.0f, 100.0f, 70.0f, "%", 0));
        addParam(choiceParam("modt", "Mod FX", {"Off", "Cho/Flg", "Ensemble", "Phaser"}, 1));
        addParam(floatParam("mods", "Mod Speed", 0.0f, 127.0f, 30.0f, "", 0));
        addParam(floatParam("modd", "Mod Depth", 0.0f, 127.0f, 40.0f, "", 0));
        addParam(choiceParam("dlyt", "Delay Type", {"Off", "Stereo", "Cross"}, 0));
        addParam(choiceParam("dlytm", "Delay Time", {"1/16", "1/8", "3/16", "1/4", "3/8", "1/2"}, 2));
        addParam(floatParam("dlyd", "Delay Depth", 0.0f, 127.0f, 40.0f, "", 0));
        addParam(floatParam("dlym", "Delay Mix", 0.0f, 100.0f, 25.0f, "%", 0));
        addParam(floatParam("eql", "EQ Low", -12.0f, 12.0f, 0.0f, "dB", 1));
        addParam(floatParam("eqh", "EQ High", -12.0f, 12.0f, 0.0f, "dB", 1));
        if (numParams() != kNumParams) mpc::log("MeltMicro: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);
        makePrograms();
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        for (auto& v : voices_) { v.eg1.setSampleRate(sampleRate); v.eg2.setSampleRate(sampleRate); v.svf.setSampleRate(sampleRate); }
        for (auto& d : mod_) d.prepare((int)(0.05 * sampleRate) + 4);
        dlyL_.prepare((int)(2.1 * sampleRate) + 4); dlyR_.prepare((int)(2.1 * sampleRate) + 4);
        lastEq_[0] = lastEq_[1] = 99.0f;
        controllers_.prepare(sampleRate / kCtl, 10.0f);
        reset();
    }

    void reset() override
    {
        for (auto& v : voices_) v.kill();
        for (auto& d : mod_) d.reset();
        dlyL_.reset(); dlyR_.reset(); eqLo_.reset(); eqHi_.reset();
        for (float& z : ap_) z = 0.0f;
        phFb_[0] = phFb_[1] = 0.0f; modPh_ = 0.0f;
        sustain_ = false; age_ = 0; held_ = 0; lastNote_ = -1.0f;
        arpHeld_ = 0; arpPos_ = -1; arpDir_ = 1; arpPlaying_ = 0; arpClock_ = 0.0; arpOffAt_ = -1.0; arpWasOn_ = false;
        controllers_.reset();
    }

    void process(mpc::ProcessContext& ctx) override
    {
        readSettings(ctx);
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        const int nf = std::min(ctx.numFrames, kMaxBlock);
        std::fill(bufL_, bufL_ + nf, 0.0f);
        std::fill(bufR_, bufR_ + nf, 0.0f);
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
    static constexpr int kMaxVoices = 8, kCtl = 16, kMaxBlock = 4096;

    struct Voice
    {
        int note = -1;
        bool held = false, sustained = false, active = false;
        unsigned age = 0;
        float vel = 1.0f, pitch = 60.0f, target = 60.0f;
        float ph1 = 0.0f, ph1b = 0.0f, ph2 = 0.0f, o2last = 0.0f;
        float lad[2][4] = {};
        SVF svf;
        ADSR eg1, eg2;
        uint32_t rng = 1;
        void kill() { active = false; note = -1; held = sustained = false; eg1.reset(); eg2.reset(); svf.reset(); for (auto& a : lad) for (float& x : a) x = 0.0f; }
    };

    struct Settings
    {
        int w1 = 0, w2 = 0, omod = 0, ft = 0, mode = 0, voices = 4, l1w = 2, l2w = 2;
        float c1 = 0, c2 = 0, semi2 = 0, tune2 = 0, lev1 = 1, lev2 = 0, levN = 0, porta = 0;
        float cutoff = 3000, reso = 0.15f, eg1i = 0.3f, kbd = 0.5f, level = 0.8f, pan = 0, bend = 2;
        bool dist = false;
        float env[8] = {};
        float l1Hz = 4, l2Hz = 5.5f;
        int pSrc[4] = {}, pDst[4] = {}; float pAmt[4] = {};
        bool arp = false; int arpType = 0, arpRange = 1; float arpBeats = 0.25f, arpGate = 0.7f;
        int modType = 1; float modSpeed = 0.3f, modDepth = 0.3f;
        int dlyType = 0; float dlyBeats = 0.75f, dlyFb = 0.3f, dlyMix = 0.25f;
        double bpm = 120.0;
    } s_;

    void readSettings(mpc::ProcessContext& ctx)
    {
        static const float lfoBeats[8] = {0, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f};
        static const float arpBeats[6] = {1.0f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f};
        static const float dBeats[6] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
        s_.bpm = ctx.transport.tempoValid && ctx.transport.bpm > 20.0 ? ctx.transport.bpm : 120.0;
        s_.w1 = getInt(kO1Wave); s_.c1 = get(kO1C1) * 0.01f; s_.c2 = get(kO1C2) * 0.01f;
        s_.w2 = getInt(kO2Wave); s_.omod = getInt(kO2Mod); s_.semi2 = (float)getInt(kO2Semi); s_.tune2 = get(kO2Tune) * 0.01f;
        s_.lev1 = get(kO1Lev) / 127.0f; s_.lev2 = get(kO2Lev) / 127.0f; s_.levN = get(kNoiseLev) / 127.0f; s_.porta = get(kPorta);
        s_.ft = getInt(kFType); s_.cutoff = get(kCutoff); s_.reso = get(kReso) * 0.01f;
        s_.eg1i = get(kEg1Int) / 63.0f; s_.kbd = get(kKeyTrk) / 63.0f;
        s_.level = get(kLevel) / 127.0f; s_.pan = get(kPan) / 63.0f; s_.dist = getBool(kDist);
        s_.mode = getInt(kMode); s_.voices = getInt(kVoices); s_.bend = (float)getInt(kBend);
        for (int k = 0; k < 8; ++k) s_.env[k] = get(kE1A + k);
        s_.l1w = getInt(kL1Wave); s_.l2w = getInt(kL2Wave);
        const int l1s = getInt(kL1Sync), l2s = getInt(kL2Sync);
        s_.l1Hz = l1s > 0 ? (float)(s_.bpm / 60.0 / lfoBeats[l1s]) : get(kL1Rate);
        s_.l2Hz = l2s > 0 ? (float)(s_.bpm / 60.0 / lfoBeats[l2s]) : get(kL2Rate);
        for (int p = 0; p < 4; ++p) { s_.pSrc[p] = getInt(kP1Src + 3 * p); s_.pDst[p] = getInt(kP1Dst + 3 * p); s_.pAmt[p] = get(kP1Amt + 3 * p) / 63.0f; }
        s_.arp = getBool(kArpOn); s_.arpType = getInt(kArpType); s_.arpBeats = arpBeats[mpc::clamp(getInt(kArpRate), 0, 5)];
        s_.arpRange = getInt(kArpRange); s_.arpGate = get(kArpGate) * 0.01f;
        s_.modType = getInt(kModType); s_.modSpeed = get(kModSpeed) / 127.0f; s_.modDepth = get(kModDepth) / 127.0f;
        s_.dlyType = getInt(kDlyType); s_.dlyBeats = dBeats[mpc::clamp(getInt(kDlyTime), 0, 5)];
        s_.dlyFb = get(kDlyDepth) / 127.0f * 0.85f; s_.dlyMix = get(kDlyMix) * 0.01f;
        const float eql = get(kEqLow), eqh = get(kEqHigh);
        if (eql != lastEq_[0]) { eqLo_.setCoeffs(BiquadCoeffs::lowShelf(sr_, 200.0f, 0.7f, eql)); lastEq_[0] = eql; }
        if (eqh != lastEq_[1]) { eqHi_.setCoeffs(BiquadCoeffs::highShelf(sr_, 5000.0f, 0.7f, eqh)); lastEq_[1] = eqh; }
        for (auto& v : voices_)
            if (v.active) { v.eg1.set(s_.env[0], s_.env[1], s_.env[2] * 0.01f, s_.env[3]); v.eg2.set(s_.env[4], s_.env[5], s_.env[6] * 0.01f, s_.env[7]); }
        if (arpWasOn_ && !s_.arp) arpRelease();
        arpWasOn_ = s_.arp;
    }

    // ---- notes ---------------------------------------------------------------------
    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn()) { if (s_.arp) arpAdd(ev.note(), ev.velocity()); else voiceOn(ev.note(), ev.velocity()); }
        else if (ev.isNoteOff()) { if (s_.arp) arpRemove(ev.note()); else voiceOff(ev.note()); }
        else if (ev.isAllNotesOff()) { arpHeld_ = 0; arpRelease(); held_ = 0; for (auto& v : voices_) release(v); }
        else if (ev.isController() && ev.controller() == cc::Sustain)
        {
            sustain_ = ev.controllerValue() >= 64;
            if (!sustain_) for (auto& v : voices_) if (v.sustained) release(v);
        }
        else controllers_.handle(ev);
    }

    void voiceOn(int note, int vel)
    {
        const float from = lastNote_ < 0 ? (float)note : lastNote_;
        lastNote_ = (float)note;
        if (s_.mode >= 1)
        {
            if (held_ < 16) heldKeys_[held_++] = note;
            Voice& v = voices_[0];
            if (v.active && (v.held || v.sustained))
            {
                v.note = note; v.held = true; v.target = (float)note;
                if (s_.porta <= 0.0f) v.pitch = (float)note;
                if (s_.mode == 1) { v.vel = (float)vel / 127.0f; v.eg1.noteOn(); v.eg2.noteOn(); }
                return;
            }
            start(v, note, vel, s_.porta > 0.0f ? from : (float)note);
            return;
        }
        start(allocate(), note, vel, s_.porta > 0.0f ? from : (float)note);
    }

    void voiceOff(int note)
    {
        if (s_.mode >= 1)
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

    void start(Voice& v, int note, int vel, float from)
    {
        const bool was = v.active;
        v.active = true; v.note = note; v.held = true; v.sustained = false; v.age = ++age_;
        v.vel = (float)vel / 127.0f; v.pitch = from; v.target = (float)note;
        v.rng = 0x9E3779B9u * (age_ + 3);
        if (!was) { v.svf.reset(); for (auto& a : v.lad) for (float& x : a) x = 0.0f; v.ph1 = v.ph2 = v.ph1b = 0.0f; v.eg1.reset(); v.eg2.reset(); }
        v.eg1.set(s_.env[0], s_.env[1], s_.env[2] * 0.01f, s_.env[3]);
        v.eg2.set(s_.env[4], s_.env[5], s_.env[6] * 0.01f, s_.env[7]);
        v.eg1.noteOn(); v.eg2.noteOn();
    }

    void release(Voice& v) { v.held = v.sustained = false; v.eg1.noteOff(); v.eg2.noteOff(); }

    Voice& allocate()
    {
        const int lim = mpc::clamp(s_.voices, 1, kMaxVoices);
        for (int i = 0; i < lim; ++i) if (!voices_[i].active) return voices_[i];
        Voice* best = &voices_[0];
        for (int i = 0; i < lim; ++i)
        {
            Voice& v = voices_[i];
            const bool rel = !v.held && !v.sustained, bestRel = !best->held && !best->sustained;
            if ((rel && !bestRel) || (rel == bestRel && v.age < best->age)) best = &v;
        }
        return *best;
    }

    // ---- arpeggiator -------------------------------------------------------------------
    void arpAdd(int note, int vel)
    {
        for (int i = 0; i < arpHeld_; ++i) if (arpKeys_[i] == note) return;
        if (arpHeld_ < 16) { arpKeys_[arpHeld_++] = note; arpVel_ = vel; }
        if (arpHeld_ == 1) { arpClock_ = 0.0; arpPos_ = -1; arpDir_ = 1; arpStepNow_ = true; }
    }
    void arpRemove(int note)
    {
        int w = 0;
        for (int i = 0; i < arpHeld_; ++i) if (arpKeys_[i] != note) arpKeys_[w++] = arpKeys_[i];
        arpHeld_ = w;
        if (arpHeld_ == 0) arpRelease();
    }
    void arpRelease() { for (int i = 0; i < arpPlaying_; ++i) voiceOff(arpPlay_[i]); arpPlaying_ = 0; arpOffAt_ = -1.0; }
    void arpTick(int samples)
    {
        if (!s_.arp || arpHeld_ == 0) return;
        const double step = s_.arpBeats * 60.0 / s_.bpm * sr_;
        arpClock_ += samples;
        if (arpPlaying_ > 0 && arpOffAt_ >= 0.0 && arpClock_ >= arpOffAt_) arpRelease();
        if (!(arpStepNow_ || arpClock_ >= step)) return;
        if (!arpStepNow_) arpClock_ -= step;
        arpStepNow_ = false;
        arpRelease();
        int base[16]; const int h = arpHeld_;
        for (int i = 0; i < h; ++i) base[i] = arpKeys_[i];
        std::sort(base, base + h);
        const int range = mpc::clamp(s_.arpRange, 1, 4);
        if (s_.arpType == 5)   // Trigger: every held note at once, each octave in turn
        {
            arpPos_ = (arpPos_ + 1) % range;
            for (int i = 0; i < h; ++i) { arpPlay_[arpPlaying_++] = std::min(127, base[i] + 12 * arpPos_); voiceOn(arpPlay_[arpPlaying_ - 1], arpVel_); }
        }
        else
        {
            int seq[64]; int n = 0;
            for (int o = 0; o < range; ++o) for (int i = 0; i < h && n < 64; ++i) seq[n++] = std::min(127, base[i] + 12 * o);
            int next = 0;
            switch (s_.arpType)
            {
                case 1: arpPos_ = arpPos_ <= 0 ? n - 1 : arpPos_ - 1; next = arpPos_; break;
                case 2:   // Alt 1: up and down, the ends play once
                case 3:   // Alt 2: up and down, the ends play twice
                {
                    if (n == 1) { arpPos_ = 0; next = 0; break; }
                    const int period = s_.arpType == 2 ? 2 * (n - 1) : 2 * n;
                    arpPos_ = (arpPos_ + 1) % period;
                    if (s_.arpType == 2) next = arpPos_ < n ? arpPos_ : period - arpPos_;
                    else next = arpPos_ < n ? arpPos_ : period - 1 - arpPos_;
                    break;
                }
                case 4: rngArp_ = rngArp_ * 1664525u + 1013904223u; next = (int)((rngArp_ >> 8) % (uint32_t)n); break;
                default: arpPos_ = (arpPos_ + 1) % n; next = arpPos_; break;
            }
            arpPlay_[arpPlaying_++] = seq[mpc::clamp(next, 0, n - 1)];
            voiceOn(arpPlay_[0], arpVel_);
        }
        arpOffAt_ = step * s_.arpGate;
    }

    // ---- sound ---------------------------------------------------------------------------
    static inline float rnd01(uint32_t& r) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return (float)(r >> 8) / 16777216.0f; }
    static inline float blep(float t, float dt)
    {
        if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
        if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
        return 0.0f;
    }
    static inline float saw(float ph, float dt) { return 2.0f * ph - 1.0f - blep(ph, dt); }
    static inline float square(float ph, float dt, float pw)
    {
        float p2 = ph + 1.0f - pw; if (p2 >= 1.0f) p2 -= 1.0f;
        return (saw(ph, dt) - saw(p2, dt)) * 0.5f;
    }
    static inline float tri(float ph) { return ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph; }
    inline float table(int idx, float ph, int lv) const
    {
        const float* t = waves_[(size_t)idx].level(lv);
        const float p = ph * (float)wr::kTable; const int i = (int)p; const float f = p - (float)i;
        return (t[i] + f * (t[i + 1] - t[i])) * 2.2f;
    }

    float lfo(float& ph, float& sh, int shape, float inc, bool second)
    {
        ph += inc;
        bool wrapped = false;
        if (ph >= 1.0f) { ph -= std::floor(ph); wrapped = true; }
        switch (shape)
        {
            case 0: return 1.0f - 2.0f * ph;                                    // saw
            case 1: return second ? (ph < 0.5f ? 1.0f : 0.0f) : (ph < 0.5f ? 1.0f : -1.0f);   // LFO2: unipolar square
            case 2: return second ? std::sin(2.0f * kPiF * ph) : tri(ph);
            default: if (wrapped || sh == 0.0f) sh = rnd01(lfoRng_) * 2.0f - 1.0f + 1.0e-6f; return sh;
        }
    }

    void render(int from, int to) { for (int c = from; c < to; c += kCtl) renderChunk(c, std::min(kCtl, to - c)); }

    void renderChunk(int off, int n)
    {
        arpTick(n);
        controllers_.tick();
        const float dtc = (float)n / (float)sr_;
        const float wheel = controllers_.modWheel();
        // LFO2 rate can be patched; compute the patch sums for it from LFO1 / wheel only (per voice sources ignored)
        float l2rate = s_.l2Hz;
        const float l1 = lfo(l1Ph_, sh1_, s_.l1w, s_.l1Hz * dtc, false);
        for (int p = 0; p < 4; ++p) if (s_.pDst[p] == dLfo2 && s_.pAmt[p] != 0.0f)
        {
            const float sv = s_.pSrc[p] == sLFO1 ? l1 : s_.pSrc[p] == sWheel ? wheel : 0.0f;
            l2rate *= std::pow(4.0f, sv * s_.pAmt[p]);
        }
        const float l2 = lfo(l2Ph_, sh2_, s_.l2w, l2rate * dtc, true);
        const float bendSemis = controllers_.pitchBend() * s_.bend;
        const float glide = s_.porta > 0.0f ? std::exp(-dtc / (s_.porta * 0.001f * 0.4f)) : 0.0f;
        static const int voxTab[4] = {12, 10, 11, 15};   // Oo -> Ah -> Ee -> Choir

        for (auto& v : voices_)
        {
            if (!v.active) continue;
            v.pitch = v.target + (v.pitch - v.target) * glide;
            // virtual patch
            float mPitch = 0, mOsc2 = 0, mCtrl = 0, mNoise = 0, mCut = 0, mAmp = 0, mPan = 0;
            const float src[7] = {v.eg1.level(), v.eg2.level(), l1, l2, v.vel, (v.pitch - 60.0f) / 60.0f, wheel};
            for (int p = 0; p < 4; ++p)
            {
                if (s_.pAmt[p] == 0.0f) continue;
                const float x = src[mpc::clamp(s_.pSrc[p], 0, 6)] * s_.pAmt[p];
                switch (s_.pDst[p])
                {
                    case dPitch: mPitch += x * 12.0f; break;      // +-1 octave at full amount
                    case dOsc2: mOsc2 += x * 24.0f; break;
                    case dCtrl1: mCtrl += x; break;
                    case dNoise: mNoise += x; break;
                    case dCutoff: mCut += x * 5.0f; break;       // octaves
                    case dAmp: mAmp += x; break;
                    case dPan: mPan += x; break;
                    default: break;
                }
            }
            const float note = v.pitch + bendSemis + mPitch;
            const float d1 = std::min(0.45f, 440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f) / (float)sr_);
            const float d2 = std::min(0.45f, 440.0f * std::pow(2.0f, (note + s_.semi2 + s_.tune2 + mOsc2 - 69.0f) / 12.0f) / (float)sr_);
            const int lv1 = wr::levelFor(d1);
            const float c1 = mpc::clamp(s_.c1 + l1 * s_.c2 * 0.5f + mCtrl, 0.0f, 1.0f);
            const float oct = s_.eg1i * v.eg1.level() * 6.0f + s_.kbd * (v.pitch - 60.0f) / 12.0f + mCut;
            const float fc = mpc::clamp(s_.cutoff * std::pow(2.0f, mpc::clamp(oct, -10.0f, 10.0f)), 20.0f, 0.45f * (float)sr_);
            const float g = std::tan(kPiF * fc / (float)sr_), G = g / (1.0f + g), ia = 1.0f / (1.0f + g), G4 = G * G * G * G, k = s_.reso * 4.1f;
            if (s_.ft > 0) v.svf.set(fc, s_.reso);
            const int svfMode = s_.ft == 1 ? 0 : s_.ft == 2 ? 1 : 2;
            const float pan = mpc::clamp(s_.pan + mPan, -1.0f, 1.0f);
            const float pa = (pan + 1.0f) * 0.25f * kPiF;
            const float gl = std::cos(pa) * 1.4142f, gr = std::sin(pa) * 1.4142f;
            const float amp = s_.level * mpc::clamp(1.0f + mAmp, 0.0f, 2.0f) * 0.3f;
            const float noiseLev = mpc::clamp(s_.levN + mNoise, 0.0f, 1.0f);
            const bool ringOn = s_.omod == 1 || s_.omod == 3, syncOn = s_.omod == 2 || s_.omod == 3;
            // Vox / Digital selections
            const float vp = c1 * 3.0f; const int va = std::min(2, (int)vp); const float vm = vp - (float)va;
            const int dig = mpc::clamp((int)std::lround(c1 * 31.0f), 0, 31);

            float* bl = bufL_ + off;
            float* br = bufR_ + off;
            for (int i = 0; i < n; ++i)
            {
                // osc 1 phase first (sync needs its wrap)
                const float p1 = v.ph1;
                v.ph1 += d1;
                bool wrapped = false; float wrapFrac = 0.0f;
                if (v.ph1 >= 1.0f) { v.ph1 -= 1.0f; wrapped = true; wrapFrac = v.ph1 / d1; }
                // osc 2
                if (syncOn && wrapped) v.ph2 = wrapFrac * d2;
                float o2 = s_.w2 == 1 ? square(v.ph2, d2, 0.5f) : s_.w2 == 2 ? tri(v.ph2) : saw(v.ph2, d2);
                v.ph2 += d2; if (v.ph2 >= 1.0f) v.ph2 -= 1.0f;
                // osc 1
                float o1;
                switch (s_.w1)
                {
                    case 1: o1 = square(p1, d1, 0.5f - c1 * 0.45f); break;                       // Ctrl1 = pulse width
                    case 2: { const float t = tri(p1); o1 = t + c1 * (std::sin(kPiF * 2.0f * t) - t); break; }   // Ctrl1 = fold
                    case 3: o1 = std::sin(2.0f * kPiF * p1 + c1 * 2.0f * kPiF * std::sin(2.0f * kPiF * p1 * 2.0f) * 0.25f); break; // Ctrl1 = brightness
                    case 4: { const float a = table(voxTab[va], p1, lv1), b = table(voxTab[va + 1], p1, lv1); o1 = a + vm * (b - a); break; }
                    case 5: o1 = table(dig, p1, lv1); break;
                    case 6: { float ph = p1 + c1 * 1.5f * v.o2last; ph -= std::floor(ph); o1 = std::sin(2.0f * kPiF * ph); break; }   // cross mod
                    case 7: o1 = rnd01(v.rng) * 2.0f - 1.0f; break;
                    default:
                    {   // Ctrl1 = saw waveform: blends in a second saw an octave up (brighter, thinner)
                        v.ph1b += d1 * 2.0f; if (v.ph1b >= 1.0f) v.ph1b -= 1.0f;
                        o1 = saw(p1, d1) * (1.0f - 0.5f * c1) + c1 * 0.6f * saw(v.ph1b, std::min(0.45f, d1 * 2.0f));
                        break;
                    }
                }
                v.o2last = o2;
                if (ringOn) o2 *= o1 * 1.5f;
                float x = o1 * s_.lev1 + o2 * s_.lev2;
                if (noiseLev > 0.0f) x += (rnd01(v.rng) * 2.0f - 1.0f) * noiseLev * 0.6f;
                x *= 0.5f;
                // filter
                float y;
                if (s_.ft == 0)
                {
                    float* st = v.lad[0];
                    const float S = G * G * G * ia * st[0] + G * G * ia * st[1] + G * ia * st[2] + ia * st[3];
                    const float y4 = (G4 * x + S) / (1.0f + k * G4);
                    float u = fastTanh(x - k * y4);
                    for (int s = 0; s < 4; ++s) { const float vv = (u - st[s]) * G; const float yy = vv + st[s]; st[s] = yy + vv; u = yy; }
                    y = u * (1.0f + k * 0.35f);
                }
                else y = v.svf.process(x, svfMode);
                if (s_.dist) y = fastTanh(y * 4.0f) * 0.5f;
                v.eg1.process();
                const float a = v.eg2.process() * amp;
                bl[i] += y * a * gl;
                br[i] += y * a * gr;
            }
            if (!v.eg2.isActive() || (v.eg2.stage() == ADSR::Stage::Sustain && v.eg2.level() < 1.0e-4f)) v.kill();
        }
    }

    void effects(float* L, float* R, int n)
    {
        const float speed = 0.05f + s_.modSpeed * s_.modSpeed * 8.0f;   // Hz
        const float depth = s_.modDepth;
        const float dlen = std::min((float)(s_.dlyBeats * 60.0 / s_.bpm * sr_), 2.0f * (float)sr_);
        for (int i = 0; i < n; ++i)
        {
            float l = bufL_[i], r = bufR_[i];
            // ---- Mod FX
            if (s_.modType == 1 || s_.modType == 2)
            {
                const float m = (l + r) * 0.5f;
                modPh_ += speed / (float)sr_; if (modPh_ >= 1.0f) modPh_ -= 1.0f;
                if (s_.modType == 1)   // chorus/flanger: low depth = flanger-short delay with feedback, high = chorus
                {
                    const float base = (0.6f + 14.0f * depth) * 0.001f * (float)sr_;
                    const float sw = (0.4f + 4.0f * depth) * 0.001f * (float)sr_;
                    const float fb = (1.0f - depth) * 0.6f;
                    const float tl = mod_[0].readLinear(base + sw * std::sin(2.0f * kPiF * modPh_));
                    const float tr = mod_[0].readLinear(base + sw * std::cos(2.0f * kPiF * modPh_));
                    mod_[0].write(m + fb * (tl + tr) * 0.5f);
                    l = l * 0.7f + tl * 0.7f; r = r * 0.7f + tr * 0.7f;
                }
                else                   // ensemble: three voices 120 degrees apart, plus a fast shimmer
                {
                    mod_[0].write(m);
                    float el = 0.0f, er = 0.0f;
                    for (int t = 0; t < 3; ++t)
                    {
                        const float ph = modPh_ + (float)t / 3.0f;
                        const float dly = (8.0f + 4.0f * depth * std::sin(2.0f * kPiF * ph) + 0.4f * std::sin(2.0f * kPiF * ph * 7.0f)) * 0.001f * (float)sr_;
                        const float tap = mod_[0].readLinear(dly);
                        el += tap * (t == 0 ? 0.8f : t == 1 ? 0.2f : 0.5f);
                        er += tap * (t == 0 ? 0.2f : t == 1 ? 0.8f : 0.5f);
                    }
                    l = l * 0.6f + el * 0.55f; r = r * 0.6f + er * 0.55f;
                }
            }
            else if (s_.modType == 3)   // phaser: 6 all-pass stages per side, LFO swept, with feedback
            {
                modPh_ += speed / (float)sr_; if (modPh_ >= 1.0f) modPh_ -= 1.0f;
                for (int c = 0; c < 2; ++c)
                {
                    const float mm = 0.5f + 0.5f * std::sin(2.0f * kPiF * (modPh_ + 0.25f * c));
                    const float hz = 150.0f * std::pow(1.0f + 30.0f * depth, mm);
                    const float t = std::tan(kPiF * std::min(hz, 0.45f * (float)sr_) / (float)sr_);
                    const float a = (t - 1.0f) / (t + 1.0f);
                    float& io = c ? r : l;
                    float x = io + phFb_[c] * 0.5f;
                    for (int s = 0; s < 6; ++s) { float& z = ap_[c * 6 + s]; const float y = a * x + z; z = x - a * y; x = y; }
                    phFb_[c] = x;
                    io = 0.5f * (io + x);
                }
            }
            // ---- Delay
            if (s_.dlyType > 0)
            {
                const float tl = dlyL_.readLinear(dlen), tr = dlyR_.readLinear(dlen);
                if (s_.dlyType == 1) { dlyL_.write(l + tl * s_.dlyFb); dlyR_.write(r + tr * s_.dlyFb); }
                else { dlyL_.write((l + r) * 0.5f + tr * s_.dlyFb); dlyR_.write(tl * s_.dlyFb); }   // cross: ping-pong
                l += tl * s_.dlyMix; r += tr * s_.dlyMix;
            }
            // ---- EQ
            eqLo_.process(l, r); eqHi_.process(l, r);
            if (std::fabs(l) < 1.0e-20f) l = 0.0f;   // tiny filter/delay tails
            if (std::fabs(r) < 1.0e-20f) r = 0.0f;
            L[i] = sanitize(softClip(l));
            if (R) R[i] = sanitize(softClip(r));
        }
    }

    static float softClip(float x)
    {
        const float a = std::fabs(x);
        if (a < 0.7f) return x;
        const float y = 0.7f + 0.3f * fastTanh((a - 0.7f) / 0.3f);
        return x < 0 ? -y : y;
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
        // osc1: 0 Saw 1 Square 2 Tri 3 Sine 4 Vox 5 Digital 6 Cross 7 Noise | patch src: 0 EG1 1 EG2 2 LFO1 3 LFO2 4 Vel 5 Key 6 Wheel
        // patch dst: 0 Pitch 1 Osc2 2 Ctrl1 3 Noise 4 Cutoff 5 Amp 6 Pan 7 LFO2 rate
        program("Vintage Lead", {{kO1Wave, 0}, {kO2Wave, 0}, {kO2Tune, 8}, {kO2Lev, 100}, {kMode, 2}, {kPorta, 60}, {kCutoff, 2500}, {kReso, 25}, {kEg1Int, 25},
                                 {kP1Amt, 6}, {kP4Amt, 30}, {kModType, 1}, {kModDepth, 60}, {kDlyType, 2}, {kDlyMix, 25}});
        program("Brassy Saws", {{kO1Wave, 0}, {kO1C1, 30}, {kO2Wave, 0}, {kO2Tune, -9}, {kO2Lev, 110}, {kCutoff, 700}, {kReso, 10}, {kEg1Int, 40}, {kE1A, 60}, {kE1D, 600},
                                {kE1S, 50}, {kE2A, 30}, {kE2S, 90}, {kE2R, 300}, {kP3Amt, 20}, {kModType, 2}, {kModDepth, 70}});
        program("Vox Choir", {{kO1Wave, 4}, {kO1C1, 35}, {kO1C2, 40}, {kL1Rate, 0.3f}, {kL1Wave, 2}, {kFType, 1}, {kCutoff, 4000}, {kEg1Int, 0}, {kE2A, 600}, {kE2R, 1500},
                              {kP1Amt, 5}, {kModType, 2}, {kModDepth, 90}, {kDlyType, 1}, {kDlyMix, 20}, {kVoices, 6}});
        program("Digital Keys", {{kO1Wave, 5}, {kO1C1, 45}, {kFType, 1}, {kCutoff, 2500}, {kEg1Int, 35}, {kE1D, 400}, {kE1S, 10}, {kE2A, 1}, {kE2D, 1500}, {kE2S, 20},
                                 {kE2R, 500}, {kP3Amt, 30}, {kModType, 1}, {kModDepth, 70}, {kDlyType, 2}, {kDlyMix, 20}});
        program("Cross Bell", {{kO1Wave, 6}, {kO1C1, 55}, {kO2Wave, 2}, {kO2Semi, 19}, {kO2Lev, 0}, {kCutoff, 9000}, {kEg1Int, 0}, {kE2A, 1}, {kE2D, 2200}, {kE2S, 0},
                               {kE2R, 1500}, {kP1Src, 0}, {kP1Dst, 2}, {kP1Amt, 30}, {kModType, 1}, {kModDepth, 80}, {kDlyType, 2}, {kDlyMix, 25}});
        program("Ring Bass", {{kLevel, 127}, {kDist, 1}, {kO1Wave, 1}, {kO2Wave, 0}, {kO2Mod, 1}, {kO2Semi, 7}, {kO2Lev, 90}, {kO1Lev, 127}, {kCutoff, 800}, {kReso, 30}, {kEg1Int, 45},
                              {kE1D, 250}, {kE1S, 0}, {kE2S, 80}, {kE2R, 100}, {kMode, 2}, {kModType, 0}});
        program("Sync Lead", {{kO1Wave, 0}, {kO1Lev, 60}, {kO2Wave, 0}, {kO2Mod, 2}, {kO2Lev, 120}, {kO2Semi, 5}, {kP1Src, 0}, {kP1Dst, 1}, {kP1Amt, 40},
                              {kE1D, 900}, {kE1S, 20}, {kCutoff, 5000}, {kEg1Int, 5}, {kMode, 2}, {kPorta, 40}, {kDlyType, 2}, {kDlyMix, 25}});
        program("Phaser Pad", {{kO1Wave, 0}, {kO2Wave, 1}, {kO2Tune, 12}, {kO2Lev, 90}, {kFType, 1}, {kCutoff, 1800}, {kEg1Int, 10}, {kE2A, 800}, {kE2R, 2000},
                               {kModType, 3}, {kModSpeed, 20}, {kModDepth, 90}, {kVoices, 6}});
        program("Ensemble Strings", {{kO1Wave, 0}, {kO1C1, 20}, {kO2Wave, 0}, {kO2Tune, 6}, {kO2Lev, 110}, {kFType, 1}, {kCutoff, 3000}, {kE2A, 350}, {kE2R, 900},
                                     {kModType, 2}, {kModSpeed, 25}, {kModDepth, 110}, {kEqHigh, 2}, {kVoices, 8}});
        program("Trigger Arp", {{kLevel, 127}, {kArpOn, 1}, {kArpType, 5}, {kArpRate, 3}, {kArpRange, 2}, {kArpGate, 40}, {kO1Wave, 1}, {kO1C1, 30}, {kCutoff, 1500}, {kReso, 35},
                                {kEg1Int, 40}, {kE1D, 150}, {kE1S, 0}, {kE2D, 250}, {kE2S, 0}, {kDlyType, 2}, {kDlyMix, 30}, {kVoices, 8}});
        program("Alt Arp Pluck", {{kLevel, 127}, {kArpOn, 1}, {kArpType, 2}, {kArpRate, 3}, {kArpRange, 2}, {kArpGate, 50}, {kO1Wave, 0}, {kCutoff, 1200}, {kReso, 40}, {kEg1Int, 45},
                                  {kE1D, 160}, {kE1S, 0}, {kE2D, 300}, {kE2S, 0}, {kDlyType, 2}, {kDlyMix, 25}});
        program("Wobble Square", {{kLevel, 127}, {kO1Wave, 1}, {kO1C1, 20}, {kO2Wave, 1}, {kO2Semi, -12}, {kO2Lev, 90}, {kCutoff, 300}, {kReso, 50}, {kEg1Int, 0}, {kL1Sync, 2},
                                  {kP2Src, 2}, {kP2Dst, 4}, {kP2Amt, 45}, {kMode, 2}, {kDist, 1}, {kModType, 0}});
        program("Noise Snare Hit", {{kO1Wave, 7}, {kO2Wave, 2}, {kO2Semi, -5}, {kO2Lev, 60}, {kFType, 2}, {kCutoff, 2500}, {kReso, 20}, {kEg1Int, 0}, {kE2A, 1},
                                    {kE2D, 180}, {kE2S, 0}, {kE2R, 150}, {kModType, 0}});
        program("Init", {{kModType, 0}, {kEg1Int, 0}, {kCutoff, 20000}, {kReso, 0}});
    }

    const std::vector<wr::Wave>& waves_;
    double sr_ = 44100.0;
    Voice voices_[kMaxVoices];
    unsigned age_ = 0;
    bool sustain_ = false;
    int heldKeys_[16] = {}, held_ = 0;
    float lastNote_ = -1.0f;
    int arpKeys_[16] = {}, arpHeld_ = 0, arpPos_ = -1, arpDir_ = 1, arpVel_ = 100, arpPlay_[16] = {}, arpPlaying_ = 0;
    double arpClock_ = 0.0, arpOffAt_ = -1.0;
    bool arpStepNow_ = false, arpWasOn_ = false;
    uint32_t rngArp_ = 999u, lfoRng_ = 3131u;
    float l1Ph_ = 0.0f, l2Ph_ = 0.0f, sh1_ = 0.0f, sh2_ = 0.0f;
    float bufL_[kMaxBlock], bufR_[kMaxBlock];
    DelayLine mod_[1], dlyL_, dlyR_;
    float modPh_ = 0.0f, ap_[12] = {}, phFb_[2] = {};
    StereoBiquad eqLo_, eqHi_;
    float lastEq_[2] = {99.0f, 99.0f};
    MidiControllers controllers_;
};

MPC_PLUGIN_ENTRY(MeltMicro)
