// MeltVenom.cpp - a virtual-analog powerhouse synth for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// An original design in the spirit of the big German virtual-analog synths of the 2000s.
// No code, sounds, wavetables or names are taken from any product. Per voice:
//
//   OSC 1, OSC 2: Saw / Pulse (PW) / Tri / Sine / HYPER (up to 9 detuned saws) / WAVE (32
//                 band-limited waves with morph). Osc 2: semi, fine, hard SYNC to osc 1,
//                 FM (osc 1 phase-modulates osc 2), Env->Osc2 pitch for sync sweeps.
//   + SUB (square/tri, -1 oct), NOISE, RING (osc1 x osc2)            [anti-aliased: polyBLEP]
//   UNISON 1-4 copies, detuned and spread across the stereo field.
//   FILTER 1: 24 dB ladder (zero-delay feedback, saturating) or LP12 / BP / HP
//   FILTER 2: Off / LP / HP / BP / Notch at an offset from filter 1; Serial or Parallel.
//   Filter env (also -> osc 2 pitch), amp env, velocity, key track, drive.
//   LFO 1 (-> pitch, PW, wave morph, cutoff) and LFO 2 (-> cutoff, pan, amp), free or synced.
// Global: Poly / Mono / Legato, glide, ARPEGGIATOR (Up, Down, Up-Down, Random, Played; 1-3 oct,
// synced rate, gate), FX: distortion -> chorus -> tempo delay -> reverb.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/svf.h"
#include "dsp/adsr.h"
#include "dsp/smoother.h"
#include "dsp/delay_line.h"
#include "dsp/midi_cc.h"
#include "dsp/tempo.h"
#include "waves.h"
#include "reverb.h"

#include <algorithm>
#include <mutex>

using namespace mpc::dsp;
static constexpr float kPiF = 3.14159265358979f;

static const std::vector<wr::Wave>& venomWaves()
{
    static std::vector<wr::Wave> w;
    static std::once_flag once;
    std::call_once(once, [] { w = wr::builtinWaves(); });
    return w;
}

class MeltVenom : public mpc::Plugin
{
public:
    enum Params
    {
        // OSC
        kO1Wave = 0, kO1Pw, kO1Wt, kO1Den, kO1Det,
        kO2Wave, kO2Pw, kO2Wt, kO2Den, kO2Det, kO2Semi, kO2Fine, kO2Sync, kFm, kEnvPitch,
        // MIX
        kBalance, kSub, kSubWave, kNoise, kRing, kUniVoices, kUniDetune, kUniSpread, kMode, kGlide, kBend, kPoly, kVolume,
        // FILTER
        kF1Type, kCutoff, kReso, kF2Type, kF2Offset, kF2Reso, kRouting, kDrive, kEnvAmt, kKeyTrack, kVelFilter,
        // ENV
        kFA, kFD, kFS, kFR, kA, kD, kS, kR, kVelAmp,
        // LFO
        kL1Rate, kL1Sync, kL1Shape, kL1Pitch, kL1Pw, kL1Wave, kL1Cut, kL2Rate, kL2Sync, kL2Shape, kL2Cut, kL2Pan, kL2Amp,
        // ARP + FX
        kArpOn, kArpMode, kArpRate, kArpOct, kArpGate,
        kDist, kChorus, kChoRate, kDelayMix, kDelayTime, kDelayFb, kRevMix, kRevTime,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt Venom";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'P', 'f', 'y');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    MeltVenom() : mpc::Plugin(makeInfo()), waves_(venomWaves())
    {
        using namespace mpc;
        const std::vector<std::string> oscW = {"Saw", "Pulse", "Tri", "Sine", "Hyper", "Wave"};
        const float nw = (float)waves_.size() - 1.0f;
        std::vector<std::string> lfoSync = {"Free"};
        for (const char* d : {"1/16", "1/8", "1/4", "1/2", "1 Bar", "2 Bars", "4 Bars"}) lfoSync.push_back(d);
        // OSC
        addParam(choiceParam("o1w", "Osc1 Wave", oscW, 4));
        addParam(floatParam("o1pw", "Osc1 PW", 5.0f, 95.0f, 50.0f, "%", 0));
        addParam(floatParam("o1wt", "Osc1 Table", 0.0f, nw, 10.0f, "", 1));
        addParam(intParam("o1den", "Osc1 Density", 1, 9, 7));
        addParam(floatParam("o1det", "Osc1 Detune", 0.0f, 100.0f, 35.0f, "%", 0));
        addParam(choiceParam("o2w", "Osc2 Wave", oscW, 0));
        addParam(floatParam("o2pw", "Osc2 PW", 5.0f, 95.0f, 50.0f, "%", 0));
        addParam(floatParam("o2wt", "Osc2 Table", 0.0f, nw, 13.0f, "", 1));
        addParam(intParam("o2den", "Osc2 Density", 1, 9, 5));
        addParam(floatParam("o2det", "Osc2 Detune", 0.0f, 100.0f, 25.0f, "%", 0));
        addParam(intParam("o2semi", "Osc2 Semi", -24, 24, 0, "st"));
        addParam(floatParam("o2fine", "Osc2 Fine", -50.0f, 50.0f, 7.0f, "ct", 0));
        addParam(boolParam("sync", "Sync", false));
        addParam(floatParam("fm", "FM", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("envp", "Env>Osc2", -48.0f, 48.0f, 0.0f, "st", 1));
        // MIX
        addParam(floatParam("bal", "Osc Bal", 0.0f, 100.0f, 35.0f, "%", 0));
        addParam(floatParam("sub", "Sub", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("subw", "Sub Wave", {"Square", "Tri"}, 0));
        addParam(floatParam("noise", "Noise", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("ring", "Ring", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(intParam("uni", "Unison", 1, 4, 1));
        addParam(floatParam("unidet", "Uni Detune", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam("unispr", "Uni Spread", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(choiceParam("mode", "Key Mode", {"Poly", "Mono", "Legato"}, 0));
        addParam(floatParam("glide", "Glide", 0.0f, 2000.0f, 0.0f, "ms", 0, 0.3f));
        addParam(intParam("bend", "Bend Range", 0, 24, 2, "st"));
        addParam(intParam("poly", "Voices", 1, kVoices, 8));
        addParam(floatParam("vol", "Volume", -40.0f, 6.0f, -6.0f, "dB", 1));
        // FILTER
        addParam(choiceParam("f1", "Filter 1", {"Ladder 24", "LP 12", "BP", "HP"}, 0));
        addParam(logParam("cut", "Cutoff", 20.0f, 20000.0f, 4000.0f, "Hz", 0));
        addParam(floatParam("res", "Resonance", 0.0f, 100.0f, 20.0f, "%", 0));
        addParam(choiceParam("f2", "Filter 2", {"Off", "LP", "HP", "BP", "Notch"}, 0));
        addParam(floatParam("f2off", "F2 Offset", -48.0f, 48.0f, 0.0f, "st", 1));
        addParam(floatParam("f2res", "F2 Reso", 0.0f, 100.0f, 10.0f, "%", 0));
        addParam(choiceParam("route", "Routing", {"Serial", "Parallel"}, 0));
        addParam(floatParam("drive", "Drive", 0.0f, 100.0f, 10.0f, "%", 0));
        addParam(floatParam("envamt", "Env Amount", -100.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam("keytrk", "Key Track", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam("velf", "Vel>Filter", 0.0f, 100.0f, 20.0f, "%", 0));
        // ENV
        addParam(logParam("fa", "F Attack", 0.5f, 10000.0f, 2.0f, "ms", 0));
        addParam(logParam("fd", "F Decay", 1.0f, 10000.0f, 500.0f, "ms", 0));
        addParam(floatParam("fs", "F Sustain", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(logParam("fr", "F Release", 1.0f, 10000.0f, 300.0f, "ms", 0));
        addParam(logParam("a", "Attack", 0.5f, 10000.0f, 2.0f, "ms", 0));
        addParam(logParam("d", "Decay", 1.0f, 10000.0f, 800.0f, "ms", 0));
        addParam(floatParam("s", "Sustain", 0.0f, 100.0f, 80.0f, "%", 0));
        addParam(logParam("r", "Release", 1.0f, 10000.0f, 300.0f, "ms", 0));
        addParam(floatParam("vela", "Vel>Amp", 0.0f, 100.0f, 50.0f, "%", 0));
        // LFO
        addParam(logParam("l1r", "LFO1 Rate", 0.05f, 30.0f, 5.0f, "Hz", 2));
        addParam(choiceParam("l1s", "LFO1 Sync", lfoSync, 0));
        addParam(choiceParam("l1w", "LFO1 Shape", {"Tri", "Sine", "Saw", "Square", "S&H"}, 0));
        addParam(floatParam("l1p", "LFO1>Pitch", 0.0f, 100.0f, 0.0f, "ct", 0));
        addParam(floatParam("l1pw", "LFO1>PW", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("l1wt", "LFO1>Table", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("l1c", "LFO1>Cutoff", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam("l2r", "LFO2 Rate", 0.05f, 30.0f, 0.5f, "Hz", 2));
        addParam(choiceParam("l2s", "LFO2 Sync", lfoSync, 0));
        addParam(choiceParam("l2w", "LFO2 Shape", {"Tri", "Sine", "Saw", "Square", "S&H"}, 1));
        addParam(floatParam("l2c", "LFO2>Cutoff", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("l2pan", "LFO2>Pan", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("l2amp", "LFO2>Amp", 0.0f, 100.0f, 0.0f, "%", 0));
        // ARP + FX
        addParam(boolParam("arp", "Arp", false));
        addParam(choiceParam("arpm", "Arp Mode", {"Up", "Down", "Up-Down", "Random", "Played"}, 0));
        addParam(choiceParam("arpr", "Arp Rate", {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"}, 3));
        addParam(intParam("arpo", "Arp Octave", 1, 3, 1));
        addParam(floatParam("arpg", "Arp Gate", 10.0f, 100.0f, 60.0f, "%", 0));
        addParam(floatParam("dist", "Distortion", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("cho", "Chorus", 0.0f, 100.0f, 20.0f, "%", 0));
        addParam(logParam("chor", "Chorus Rate", 0.05f, 5.0f, 0.4f, "Hz", 2));
        addParam(floatParam("dmix", "Delay", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("dtime", "Delay Time", {"1/16", "1/8", "3/16", "1/4", "3/8", "1/2"}, 2));
        addParam(floatParam("dfb", "Delay Fdbk", 0.0f, 90.0f, 35.0f, "%", 0));
        addParam(floatParam("rmix", "Reverb", 0.0f, 100.0f, 15.0f, "%", 0));
        addParam(logParam("rtime", "Reverb Time", 0.2f, 12.0f, 2.5f, "s", 1));
        if (numParams() != kNumParams) mpc::log("MeltVenom: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);
        makePrograms();
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        for (auto& v : voices_) { v.fenv.setSampleRate(sampleRate); v.aenv.setSampleRate(sampleRate); for (auto& f : v.svf) f.setSampleRate(sampleRate); }
        choL_.prepare((int)(0.04 * sampleRate) + 4);
        dlyL_.prepare((int)(2.1 * sampleRate) + 4);
        dlyR_.prepare((int)(2.1 * sampleRate) + 4);
        reverb_.prepare(sampleRate);
        volume_.setTime(15.0f, sampleRate);
        controllers_.prepare(sampleRate / kCtl, 10.0f);
        reset();
    }

    void reset() override
    {
        for (auto& v : voices_) v.kill();
        choL_.reset(); dlyL_.reset(); dlyR_.reset(); reverb_.reset();
        choPh_ = 0.0f; lfo1Ph_ = lfo2Ph_ = 0.0f; sh1_ = sh2_ = 0.0f;
        sustain_ = false; age_ = 0; held_ = 0; lastNote_ = -1.0f;
        arpHeld_ = 0; arpPos_ = 0; arpDir_ = 1; arpNote_ = -1; arpClock_ = 0.0; arpOffAt_ = -1.0; arpWasOn_ = false;
        volume_.snap(dbToGain(get(kVolume)));
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
    static constexpr int kVoices = 12, kUni = 4, kHyper = 9, kCtl = 16, kMaxBlock = 4096;

    struct Uni
    {
        float ph1[kHyper] = {}, ph2[kHyper] = {}, sub = 0.0f;
        float gl = 0.7f, gr = 0.7f;
    };
    struct Voice
    {
        int note = -1;
        bool held = false, sustained = false, active = false;
        unsigned age = 0;
        float vel = 1.0f, pitch = 60.0f, target = 60.0f;
        Uni u[kUni];
        float lad[2][4] = {};        // ladder states, L/R
        SVF svf[4];                  // [0,1] filter-1 SVF L/R, [2,3] filter 2 L/R
        ADSR fenv, aenv;
        uint32_t rng = 1;
        void kill() { active = false; note = -1; held = sustained = false; aenv.reset(); fenv.reset(); for (auto& s : svf) s.reset(); for (auto& a : lad) for (float& x : a) x = 0.0f; }
    };

    struct Settings
    {
        int w1 = 4, w2 = 0, den1 = 7, den2 = 5, subW = 0, uni = 1, mode = 0, poly = 8, f1 = 0, f2 = 0, route = 0, l1w = 0, l2w = 1;
        bool sync = false;
        float pw1 = 0.5f, pw2 = 0.5f, wt1 = 10, wt2 = 13, det1 = 0.35f, det2 = 0.25f, semi2 = 0, fine2 = 0.07f, fm = 0, envPitch = 0;
        float bal = 0.35f, sub = 0, noise = 0, ring = 0, uniDet = 0.3f, uniSpread = 0.7f, glide = 0, bend = 2;
        float cutoff = 4000, reso = 0.2f, f2off = 0, f2reso = 0.1f, drive = 0.1f, envAmt = 0.3f, keyTrk = 0.5f, velF = 0.2f, velA = 0.5f;
        float l1Hz = 5, l2Hz = 0.5f, l1p = 0, l1pw = 0, l1wt = 0, l1c = 0, l2c = 0, l2pan = 0, l2amp = 0;
        float env[8] = {};
        float dist = 0, cho = 0.2f, choRate = 0.4f, dMix = 0, dFb = 0.35f, rMix = 0.15f;
        double bpm = 120.0;
        float dBeats = 0.75f;
        bool arp = false; int arpMode = 0, arpOct = 1; float arpBeats = 0.25f, arpGate = 0.6f;
    } s_;

    void readSettings(mpc::ProcessContext& ctx)
    {
        static const float lfoBeats[8] = {0, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f};
        static const float arpBeats[6] = {1.0f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f};
        static const float dBeats[6] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
        s_.bpm = ctx.transport.tempoValid && ctx.transport.bpm > 20.0 ? ctx.transport.bpm : 120.0;
        s_.w1 = getInt(kO1Wave); s_.w2 = getInt(kO2Wave);
        s_.pw1 = get(kO1Pw) * 0.01f; s_.pw2 = get(kO2Pw) * 0.01f;
        s_.wt1 = get(kO1Wt); s_.wt2 = get(kO2Wt);
        s_.den1 = getInt(kO1Den); s_.den2 = getInt(kO2Den);
        s_.det1 = get(kO1Det) * 0.01f; s_.det2 = get(kO2Det) * 0.01f;
        s_.semi2 = (float)getInt(kO2Semi); s_.fine2 = get(kO2Fine) * 0.01f;
        s_.sync = getBool(kO2Sync); s_.fm = get(kFm) * 0.01f; s_.envPitch = get(kEnvPitch);
        s_.bal = get(kBalance) * 0.01f; s_.sub = get(kSub) * 0.01f; s_.subW = getInt(kSubWave);
        s_.noise = get(kNoise) * 0.01f; s_.ring = get(kRing) * 0.01f;
        s_.uni = getInt(kUniVoices); s_.uniDet = get(kUniDetune) * 0.01f; s_.uniSpread = get(kUniSpread) * 0.01f;
        s_.mode = getInt(kMode); s_.glide = get(kGlide); s_.bend = (float)getInt(kBend); s_.poly = getInt(kPoly);
        s_.f1 = getInt(kF1Type); s_.cutoff = get(kCutoff); s_.reso = get(kReso) * 0.01f;
        s_.f2 = getInt(kF2Type); s_.f2off = get(kF2Offset); s_.f2reso = get(kF2Reso) * 0.01f; s_.route = getInt(kRouting);
        s_.drive = get(kDrive) * 0.01f; s_.envAmt = get(kEnvAmt) * 0.01f; s_.keyTrk = get(kKeyTrack) * 0.01f; s_.velF = get(kVelFilter) * 0.01f;
        for (int k = 0; k < 8; ++k) s_.env[k] = get(kFA + k);
        s_.velA = get(kVelAmp) * 0.01f;
        const int l1s = getInt(kL1Sync), l2s = getInt(kL2Sync);
        s_.l1Hz = l1s > 0 ? (float)(s_.bpm / 60.0 / lfoBeats[l1s]) : get(kL1Rate);
        s_.l2Hz = l2s > 0 ? (float)(s_.bpm / 60.0 / lfoBeats[l2s]) : get(kL2Rate);
        s_.l1w = getInt(kL1Shape); s_.l2w = getInt(kL2Shape);
        s_.l1p = get(kL1Pitch) * 0.01f; s_.l1pw = get(kL1Pw) * 0.01f; s_.l1wt = get(kL1Wave) * 0.01f; s_.l1c = get(kL1Cut) * 0.01f;
        s_.l2c = get(kL2Cut) * 0.01f; s_.l2pan = get(kL2Pan) * 0.01f; s_.l2amp = get(kL2Amp) * 0.01f;
        s_.arp = getBool(kArpOn); s_.arpMode = getInt(kArpMode); s_.arpBeats = arpBeats[mpc::clamp(getInt(kArpRate), 0, 5)];
        s_.arpOct = getInt(kArpOct); s_.arpGate = get(kArpGate) * 0.01f;
        s_.dist = get(kDist) * 0.01f; s_.cho = get(kChorus) * 0.01f; s_.choRate = get(kChoRate);
        s_.dMix = get(kDelayMix) * 0.01f; s_.dBeats = dBeats[mpc::clamp(getInt(kDelayTime), 0, 5)]; s_.dFb = get(kDelayFb) * 0.01f;
        s_.rMix = get(kRevMix) * 0.01f;
        reverb_.set(get(kRevTime), 0.35f);
        volume_.setTarget(dbToGain(get(kVolume)));
        for (auto& v : voices_)
            if (v.active) { v.fenv.set(s_.env[0], s_.env[1], s_.env[2] * 0.01f, s_.env[3]); v.aenv.set(s_.env[4], s_.env[5], s_.env[6] * 0.01f, s_.env[7]); }
        // arp switched off: release whatever it was playing
        if (arpWasOn_ && !s_.arp && arpNote_ >= 0) { voiceOff(arpNote_); arpNote_ = -1; }
        arpWasOn_ = s_.arp;
    }

    // ---- notes -------------------------------------------------------------
    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn())
        {
            if (s_.arp) { arpAdd(ev.note(), ev.velocity()); return; }
            voiceOn(ev.note(), ev.velocity());
        }
        else if (ev.isNoteOff())
        {
            if (s_.arp) { arpRemove(ev.note()); return; }
            voiceOff(ev.note());
        }
        else if (ev.isAllNotesOff()) { arpHeld_ = 0; held_ = 0; for (auto& v : voices_) release(v); }
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
                if (s_.glide <= 0.0f) v.pitch = (float)note;
                if (s_.mode == 1) { v.vel = (float)vel / 127.0f; v.fenv.noteOn(); v.aenv.noteOn(); }   // Mono: retrigger
                return;
            }
            start(v, note, vel, s_.glide > 0.0f ? from : (float)note);
            return;
        }
        start(allocate(), note, vel, s_.glide > 0.0f ? from : (float)note);
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
                if (s_.glide <= 0.0f) v.pitch = v.target;
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
        v.rng = 0x9E3779B9u * (age_ + 7);
        if (!was)
        {
            for (auto& s : v.svf) s.reset();
            for (auto& a : v.lad) for (float& x : a) x = 0.0f;
            // free-running oscillators: random start phases (that is what makes stacks sound big)
            for (auto& u : v.u) { for (int k = 0; k < kHyper; ++k) { u.ph1[k] = rnd01(v.rng); u.ph2[k] = rnd01(v.rng); } u.sub = 0.0f; }
            v.aenv.reset(); v.fenv.reset();
        }
        v.fenv.set(s_.env[0], s_.env[1], s_.env[2] * 0.01f, s_.env[3]);
        v.aenv.set(s_.env[4], s_.env[5], s_.env[6] * 0.01f, s_.env[7]);
        v.fenv.noteOn(); v.aenv.noteOn();
    }

    void release(Voice& v) { v.held = v.sustained = false; v.fenv.noteOff(); v.aenv.noteOff(); }

    Voice& allocate()
    {
        const int lim = mpc::clamp(s_.poly, 1, kVoices);
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

    // ---- arpeggiator ------------------------------------------------------------
    void arpAdd(int note, int vel)
    {
        for (int i = 0; i < arpHeld_; ++i) if (arpKeys_[i] == note) return;
        if (arpHeld_ < 16) { arpKeys_[arpHeld_] = note; arpOrder_[arpHeld_] = note; arpVel_ = vel; ++arpHeld_; }
        if (arpHeld_ == 1) { arpClock_ = 0.0; arpPos_ = -1; arpDir_ = 1; arpStepNow_ = true; }   // first key: start on it
    }
    void arpRemove(int note)
    {
        int w = 0;
        for (int i = 0; i < arpHeld_; ++i) if (arpKeys_[i] != note) { arpKeys_[w] = arpKeys_[i]; arpOrder_[w] = arpOrder_[i]; ++w; }
        arpHeld_ = w;
        if (arpHeld_ == 0 && arpNote_ >= 0) { voiceOff(arpNote_); arpNote_ = -1; }
    }
    void arpTick(int samples)
    {
        if (!s_.arp || arpHeld_ == 0) return;
        const double stepLen = s_.arpBeats * 60.0 / s_.bpm * sr_;
        arpClock_ += samples;
        if (arpNote_ >= 0 && arpOffAt_ >= 0.0 && arpClock_ >= arpOffAt_) { voiceOff(arpNote_); arpNote_ = -1; arpOffAt_ = -1.0; }
        if (arpStepNow_ || arpClock_ >= stepLen)
        {
            if (!arpStepNow_) arpClock_ -= stepLen;
            arpStepNow_ = false;
            // build the sequence: sorted (or played) notes x octaves
            int seq[64]; int n = 0;
            int base[16]; const int h = arpHeld_;
            for (int i = 0; i < h; ++i) base[i] = s_.arpMode == 4 ? arpOrder_[i] : arpKeys_[i];
            if (s_.arpMode != 4) std::sort(base, base + h);
            for (int o = 0; o < s_.arpOct; ++o) for (int i = 0; i < h && n < 64; ++i) seq[n++] = std::min(127, base[i] + 12 * o);
            int next = 0;
            switch (s_.arpMode)
            {
                case 1: arpPos_ = arpPos_ <= 0 ? n - 1 : arpPos_ - 1; next = arpPos_; break;
                case 2:
                    if (n == 1) { arpPos_ = 0; }
                    else { arpPos_ += arpDir_; if (arpPos_ >= n) { arpPos_ = n - 2; arpDir_ = -1; } else if (arpPos_ < 0) { arpPos_ = 1; arpDir_ = 1; } }
                    next = arpPos_; break;
                case 3: { uint32_t r = rngArp_ = rngArp_ * 1664525u + 1013904223u; next = (int)((r >> 8) % (uint32_t)n); break; }
                default: arpPos_ = (arpPos_ + 1) % n; next = arpPos_; break;
            }
            if (arpNote_ >= 0) voiceOff(arpNote_);
            arpNote_ = seq[mpc::clamp(next, 0, n - 1)];
            voiceOn(arpNote_, arpVel_);
            arpOffAt_ = stepLen * s_.arpGate;
        }
    }

    // ---- oscillators ------------------------------------------------------------
    static inline float rnd01(uint32_t& r) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return (float)(r >> 8) / 16777216.0f; }
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
    inline float tableRead(float wt, float ph, int lv) const
    {
        const int nw = (int)waves_.size();
        const float w = mpc::clamp(wt, 0.0f, (float)(nw - 1));
        const int a = (int)w, b = std::min(nw - 1, a + 1);
        const float m = w - (float)a;
        auto rd = [&](int idx) {
            const float* t = waves_[(size_t)idx].level(lv);
            const float p = ph * (float)wr::kTable; const int i = (int)p; const float f = p - (float)i;
            return t[i] + f * (t[i + 1] - t[i]); };
        const float va = rd(a);
        return m > 0.001f ? va + m * (rd(b) - va) : va;
    }
    // one oscillator sample for a single phase (non-Hyper modes); advances nothing
    inline float oscOne(int wave, float ph, float dt, float pw, float wt, int lv) const
    {
        switch (wave)
        {
            case 1: return pulse(ph, dt, pw);
            case 2: return ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
            case 3: return std::sin(2.0f * kPiF * ph);
            case 5: return tableRead(wt, ph, lv) * 2.2f;
            default: return saw(ph, dt);
        }
    }

    // ---- rendering ----------------------------------------------------------------
    float lfoValue(float& ph, float& sh, int shape, float inc)
    {
        ph += inc;
        bool wrapped = false;
        if (ph >= 1.0f) { ph -= std::floor(ph); wrapped = true; }
        switch (shape)
        {
            case 1: return std::sin(2.0f * kPiF * ph);
            case 2: return 1.0f - 2.0f * ph;
            case 3: return ph < 0.5f ? 1.0f : -1.0f;
            case 4: if (wrapped || sh == 0.0f) sh = rnd01(lfoRng_) * 2.0f - 1.0f + 1.0e-6f; return sh;
            default: return ph < 0.25f ? 4.0f * ph : ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f;
        }
    }

    void render(int from, int to)
    {
        for (int c = from; c < to; c += kCtl) renderChunk(c, std::min(kCtl, to - c));
    }

    void renderChunk(int off, int n)
    {
        arpTick(n);
        controllers_.tick();
        const float dtc = (float)n / (float)sr_;
        const float l1 = lfoValue(lfo1Ph_, sh1_, s_.l1w, s_.l1Hz * dtc);
        const float l2 = lfoValue(lfo2Ph_, sh2_, s_.l2w, s_.l2Hz * dtc);
        const float bendSemis = controllers_.pitchBend() * s_.bend + controllers_.modWheel() * 0.5f * l1;   // wheel adds vibrato
        const float glide = s_.glide > 0.0f ? std::exp(-dtc / (s_.glide * 0.001f * 0.4f)) : 0.0f;
        const int U = mpc::clamp(s_.uni, 1, kUni);
        const float uniNorm = 1.0f / std::sqrt((float)U);
        const float pw1 = mpc::clamp(s_.pw1 + l1 * s_.l1pw * 0.45f, 0.05f, 0.95f);
        const float pw2 = mpc::clamp(s_.pw2 + l1 * s_.l1pw * 0.45f, 0.05f, 0.95f);
        const float wt1 = s_.wt1 + l1 * s_.l1wt * 8.0f, wt2 = s_.wt2 + l1 * s_.l1wt * 8.0f;

        for (auto& v : voices_)
        {
            if (!v.active) continue;
            v.pitch = v.target + (v.pitch - v.target) * glide;
            // ---- control-rate values
            const float fenvNow = v.fenv.level();
            const float base = v.pitch + bendSemis + l1 * s_.l1p;
            float freq1[kUni], freq2[kUni];
            for (int u = 0; u < U; ++u)
            {
                const float uoff = U > 1 ? ((float)u / (float)(U - 1) - 0.5f) * 2.0f * s_.uniDet * 0.35f : 0.0f;   // +-35 cents
                freq1[u] = 440.0f * std::pow(2.0f, (base + uoff - 69.0f) / 12.0f) / (float)sr_;
                freq2[u] = 440.0f * std::pow(2.0f, (base + uoff + s_.semi2 + s_.fine2 + s_.envPitch * fenvNow - 69.0f) / 12.0f) / (float)sr_;
                const float pan = U > 1 ? ((float)u / (float)(U - 1) - 0.5f) * s_.uniSpread : 0.0f;
                const float a = (pan + 1.0f) * 0.25f * kPiF;
                v.u[u].gl = std::cos(a) * 1.4142f; v.u[u].gr = std::sin(a) * 1.4142f;
            }
            const float oct = s_.envAmt * fenvNow * 6.0f + s_.keyTrk * (v.pitch - 60.0f) / 12.0f + s_.velF * (v.vel - 1.0f) * 3.0f
                            + l1 * s_.l1c * 3.0f + l2 * s_.l2c * 3.0f;
            const float fc1 = mpc::clamp(s_.cutoff * std::pow(2.0f, mpc::clamp(oct, -10.0f, 10.0f)), 20.0f, 0.45f * (float)sr_);
            const float fc2 = mpc::clamp(fc1 * std::pow(2.0f, s_.f2off / 12.0f), 20.0f, 0.45f * (float)sr_);
            // ladder coefficients
            const float g = std::tan(kPiF * fc1 / (float)sr_);
            const float G = g / (1.0f + g), ia = 1.0f / (1.0f + g);
            const float k = s_.reso * 4.1f;
            const float G4 = G * G * G * G;
            if (s_.f1 > 0) { v.svf[0].set(fc1, s_.reso); v.svf[1].set(fc1, s_.reso); }
            if (s_.f2 > 0) { v.svf[2].set(fc2, s_.f2reso); v.svf[3].set(fc2, s_.f2reso); }
            const int lv1 = wr::levelFor(freq1[0]), lv2 = wr::levelFor(freq2[0]);
            const float drive = 1.0f + s_.drive * 6.0f, dnorm = 1.0f / fastTanh(drive);
            const float velGain = lerp(1.0f, v.vel * v.vel, s_.velA);
            const float pan2 = l2 * s_.l2pan;
            const float pl = 1.0f - std::max(0.0f, pan2), pr = 1.0f + std::min(0.0f, pan2);
            const float trem = 1.0f - s_.l2amp * 0.5f * (1.0f - l2);
            const float ampBase = velGain * trem * 0.22f * uniNorm;
            const int n1 = s_.w1 == 4 ? mpc::clamp(s_.den1, 1, kHyper) : 1;
            const int n2 = s_.w2 == 4 ? mpc::clamp(s_.den2, 1, kHyper) : 1;
            float hd1[kHyper], hd2[kHyper];
            for (int h = 0; h < n1; ++h) hd1[h] = n1 > 1 ? std::pow(2.0f, ((float)h / (float)(n1 - 1) - 0.5f) * s_.det1 * 0.9f / 12.0f) : 1.0f;   // up to +-45 ct
            for (int h = 0; h < n2; ++h) hd2[h] = n2 > 1 ? std::pow(2.0f, ((float)h / (float)(n2 - 1) - 0.5f) * s_.det2 * 0.9f / 12.0f) : 1.0f;
            const float hn1 = 1.0f / std::sqrt((float)n1), hn2 = 1.0f / std::sqrt((float)n2);
            const float bal1 = std::cos(s_.bal * 0.5f * kPiF), bal2 = std::sin(s_.bal * 0.5f * kPiF);

            float* bl = bufL_ + off;
            float* br = bufR_ + off;
            for (int i = 0; i < n; ++i)
            {
                float sl = 0.0f, sr = 0.0f;
                for (int u = 0; u < U; ++u)
                {
                    Uni& un = v.u[u];
                    const float d1 = freq1[u], d2 = freq2[u];
                    // ---- osc 1
                    float o1 = 0.0f;
                    bool wrapped = false; float wrapFrac = 0.0f;
                    for (int h = 0; h < n1; ++h)
                    {
                        const float dt = std::min(0.45f, d1 * hd1[h]);
                        o1 += s_.w1 == 4 ? saw(un.ph1[h], dt) : oscOne(s_.w1, un.ph1[h], dt, pw1, wt1, lv1);
                        un.ph1[h] += dt;
                        if (un.ph1[h] >= 1.0f) { un.ph1[h] -= 1.0f; if (h == 0) { wrapped = true; wrapFrac = un.ph1[h] / dt; } }
                    }
                    o1 *= hn1;
                    // sub: one octave down, follows osc 1's main phase
                    if (wrapped) { un.sub += 0.5f; if (un.sub >= 1.0f) un.sub -= 1.0f; }
                    float subPh = un.sub + un.ph1[0] * 0.5f; if (subPh >= 1.0f) subPh -= 1.0f;
                    const float sub = s_.subW == 0 ? (subPh < 0.5f ? 1.0f : -1.0f) - 0.0f : (subPh < 0.5f ? 4.0f * subPh - 1.0f : 3.0f - 4.0f * subPh);
                    // ---- osc 2 (sync, FM)
                    float o2 = 0.0f;
                    for (int h = 0; h < n2; ++h)
                    {
                        const float dt = std::min(0.45f, d2 * hd2[h]);
                        if (s_.sync && wrapped) un.ph2[h] = wrapFrac * dt;
                        float ph = un.ph2[h];
                        if (s_.fm > 0.0f) { ph += s_.fm * o1 * 0.5f; ph -= std::floor(ph); }
                        o2 += s_.w2 == 4 ? saw(ph, dt) : oscOne(s_.w2, ph, dt, pw2, wt2, lv2);
                        un.ph2[h] += dt; if (un.ph2[h] >= 1.0f) un.ph2[h] -= 1.0f;
                    }
                    o2 *= hn2;
                    float x = o1 * bal1 + o2 * bal2 + sub * s_.sub * 0.8f + o1 * o2 * s_.ring;
                    if (s_.noise > 0.0f) x += (rnd01(v.rng) * 2.0f - 1.0f) * s_.noise * 0.6f;
                    sl += x * un.gl; sr += x * un.gr;
                }
                // ---- filters (stereo)
                float y[2] = {sl, sr};
                for (int c = 0; c < 2; ++c)
                {
                    float in = y[c] * 0.5f;
                    if (s_.drive > 0.0f) in = fastTanh(in * drive) * dnorm;
                    float a1;
                    if (s_.f1 == 0)
                    {
                        float* st = v.lad[c];
                        const float S = G * G * G * ia * st[0] + G * G * ia * st[1] + G * ia * st[2] + ia * st[3];
                        const float y4 = (G4 * in + S) / (1.0f + k * G4);
                        float uu = in - k * y4;
                        uu = fastTanh(uu);                               // the ladder's input stage saturates
                        for (int s = 0; s < 4; ++s)
                        {
                            const float vv = (uu - st[s]) * G;
                            const float yy = vv + st[s];
                            st[s] = yy + vv;
                            uu = yy;
                        }
                        a1 = uu * (1.0f + k * 0.35f);                    // keep the level as resonance rises
                    }
                    else a1 = v.svf[c].process(in, s_.f1 == 1 ? 0 : s_.f1 == 2 ? 1 : 2);
                    float out = a1;
                    if (s_.f2 > 0)
                    {
                        const int m2 = s_.f2 == 1 ? 0 : s_.f2 == 2 ? 2 : s_.f2 == 3 ? 1 : 3;
                        if (s_.route == 0) out = v.svf[2 + c].process(a1, m2);
                        else out = 0.5f * (a1 + v.svf[2 + c].process(in, m2));
                    }
                    y[c] = out;
                }
                v.fenv.process();
                const float a = v.aenv.process() * ampBase * 2.0f;
                bl[i] += y[0] * a * pl;
                br[i] += y[1] * a * pr;
            }
            if (!v.aenv.isActive() || (v.aenv.stage() == ADSR::Stage::Sustain && v.aenv.level() < 1.0e-4f)) v.kill();
        }
    }

    void effects(float* L, float* R, int n)
    {
        const float dg = 1.0f + s_.dist * 12.0f, dn = 1.0f / fastTanh(dg);
        const float choInc = s_.choRate / (float)sr_;
        const float base = 0.012f * (float)sr_, depth = 0.005f * (float)sr_;
        const float dlen = std::min((float)(s_.dBeats * 60.0 / s_.bpm * sr_), 2.0f * (float)sr_);
        for (int i = 0; i < n; ++i)
        {
            float l = bufL_[i], r = bufR_[i];
            if (s_.dist > 0.0f) { l = l + s_.dist * (fastTanh(l * dg) * dn * 0.6f - l); r = r + s_.dist * (fastTanh(r * dg) * dn * 0.6f - r); }
            choL_.write((l + r) * 0.5f);
            if (s_.cho > 0.0f)
            {
                choPh_ += choInc; if (choPh_ >= 1.0f) choPh_ -= 1.0f;
                const float wl = choL_.readLinear(base + depth * std::sin(2.0f * kPiF * choPh_));
                const float wr = choL_.readLinear(base + depth * std::cos(2.0f * kPiF * choPh_));
                l = l * (1.0f - 0.35f * s_.cho) + wl * s_.cho * 0.8f;
                r = r * (1.0f - 0.35f * s_.cho) + wr * s_.cho * 0.8f;
            }
            const float tl = dlyL_.readLinear(dlen), tr = dlyR_.readLinear(dlen);
            dlyL_.write((s_.dMix > 0.0f ? (l + r) * 0.5f : 0.0f) + tr * s_.dFb);
            dlyR_.write(tl * s_.dFb);
            l += tl * s_.dMix; r += tr * s_.dMix;
            float rl, rr;
            reverb_.process(s_.rMix > 0.0f ? l : 0.0f, s_.rMix > 0.0f ? r : 0.0f, rl, rr);
            l += rl * s_.rMix; r += rr * s_.rMix;
            const float gv = volume_.process();
            L[i] = sanitize(softClip(l * gv));
            if (R) R[i] = sanitize(softClip(r * gv));
        }
    }

    static float softClip(float x)
    {
        const float a = std::fabs(x);
        if (a < 0.7f) return x;
        const float y = 0.7f + 0.3f * fastTanh((a - 0.7f) / 0.3f);
        return x < 0 ? -y : y;
    }

    // ---- presets (all original) --------------------------------------------------------
    void makePrograms()
    {
        auto program = [&](const char* name, std::initializer_list<std::pair<int, float>> o)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& p : o) v[(size_t)p.first] = p.second;
            addProgram(name, std::move(v));
        };
        // wave indices: 0 Saw 1 Pulse 2 Tri 3 Sine 4 Hyper 5 Wave
        program("Hyper Lead", {{kO1Wave, 4}, {kO1Den, 7}, {kO1Det, 45}, {kO2Wave, 4}, {kO2Den, 5}, {kO2Semi, 12}, {kO2Det, 30}, {kBalance, 30},
                               {kUniVoices, 2}, {kCutoff, 7000}, {kReso, 10}, {kEnvAmt, 15}, {kA, 3}, {kS, 90}, {kR, 400}, {kMode, 2}, {kGlide, 40},
                               {kL1Pitch, 12}, {kL1Rate, 5.5f}, {kDelayMix, 25}, {kDelayTime, 2}, {kRevMix, 25}, {kChorus, 10}});
        program("Venom Bass", {{kO1Wave, 0}, {kO2Wave, 1}, {kO2Semi, -12}, {kO2Fine, 0}, {kBalance, 45}, {kSub, 50}, {kCutoff, 380}, {kReso, 30}, {kEnvAmt, 55},
                               {kFD, 260}, {kFS, 10}, {kA, 1}, {kD, 600}, {kS, 80}, {kR, 120}, {kMode, 2}, {kGlide, 35}, {kDrive, 35}, {kChorus, 0}, {kRevMix, 0}});
        program("Acid Line", {{kO1Wave, 0}, {kBalance, 0}, {kCutoff, 260}, {kReso, 78}, {kEnvAmt, 62}, {kFD, 180}, {kFS, 0}, {kA, 1}, {kS, 100}, {kR, 60},
                              {kMode, 2}, {kGlide, 60}, {kDrive, 45}, {kDist, 20}, {kChorus, 0}, {kRevMix, 5}, {kDelayMix, 15}});
        program("Trance Pluck", {{kO1Wave, 4}, {kO1Den, 5}, {kO1Det, 30}, {kO2Wave, 0}, {kO2Semi, 12}, {kBalance, 25}, {kUniVoices, 2}, {kCutoff, 900}, {kReso, 25},
                                 {kEnvAmt, 60}, {kFD, 240}, {kFS, 0}, {kA, 1}, {kD, 420}, {kS, 0}, {kR, 300}, {kDelayMix, 30}, {kDelayTime, 2}, {kRevMix, 30}, {kVolume, -2}});
        program("Super Pad", {{kO1Wave, 4}, {kO1Den, 9}, {kO1Det, 50}, {kO2Wave, 4}, {kO2Den, 9}, {kO2Det, 40}, {kO2Semi, 7}, {kBalance, 40}, {kUniVoices, 3},
                              {kF1Type, 1}, {kCutoff, 2200}, {kReso, 10}, {kEnvAmt, 10}, {kA, 900}, {kD, 2000}, {kS, 90}, {kR, 2200}, {kL2Cut, 15}, {kL2Rate, 0.15f},
                              {kChorus, 45}, {kRevMix, 45}, {kRevTime, 5.0f}, {kPoly, 6}, {kVolume, -11}});
        program("Sync Scream", {{kO1Wave, 0}, {kO2Wave, 0}, {kO2Sync, 1}, {kO2Semi, 7}, {kEnvPitch, 24}, {kBalance, 80}, {kFD, 700}, {kFS, 20}, {kCutoff, 5000},
                                {kEnvAmt, 10}, {kMode, 2}, {kGlide, 30}, {kDist, 25}, {kDelayMix, 20}, {kRevMix, 15}});
        program("FM Bell", {{kO1Wave, 3}, {kO2Wave, 3}, {kO2Semi, 19}, {kO2Fine, 0}, {kFm, 0}, {kBalance, 0}, {kRing, 45}, {kCutoff, 12000}, {kEnvAmt, 0},
                            {kA, 1}, {kD, 2200}, {kS, 0}, {kR, 1800}, {kChorus, 25}, {kRevMix, 35}});
        program("Wobble Sub", {{kO1Wave, 0}, {kO2Wave, 1}, {kO2Fine, 0}, {kBalance, 40}, {kSub, 60}, {kCutoff, 240}, {kReso, 45}, {kEnvAmt, 0}, {kL1Sync, 2},
                               {kL1Cut, 75}, {kL1Shape, 1}, {kMode, 2}, {kDrive, 50}, {kDist, 30}, {kChorus, 0}, {kRevMix, 0}, {kS, 100}});
        program("Ring Metal", {{kO1Wave, 0}, {kO2Wave, 1}, {kO2Semi, 5}, {kO2Fine, 13}, {kRing, 80}, {kBalance, 20}, {kCutoff, 3000}, {kReso, 35}, {kEnvAmt, 30},
                               {kFD, 400}, {kA, 2}, {kD, 900}, {kS, 40}, {kRevMix, 25}});
        program("Arp Pluck 16ths", {{kArpOn, 1}, {kArpMode, 0}, {kArpRate, 3}, {kArpOct, 2}, {kArpGate, 50}, {kO1Wave, 1}, {kO1Pw, 35}, {kO2Wave, 0}, {kO2Semi, 12},
                                    {kCutoff, 1200}, {kReso, 30}, {kEnvAmt, 50}, {kFD, 180}, {kFS, 0}, {kA, 1}, {kD, 300}, {kS, 0}, {kDelayMix, 25}, {kRevMix, 20}});
        program("Arp Trance Gate", {{kArpOn, 1}, {kArpMode, 2}, {kArpRate, 3}, {kArpOct, 1}, {kArpGate, 45}, {kO1Wave, 4}, {kO1Den, 7}, {kO1Det, 40}, {kUniVoices, 2},
                                    {kCutoff, 3500}, {kEnvAmt, 20}, {kA, 1}, {kS, 100}, {kR, 60}, {kDelayMix, 20}, {kRevMix, 30}});
        program("Morph Pad", {{kO1Wave, 5}, {kO1Wt, 10}, {kO2Wave, 5}, {kO2Wt, 15}, {kO2Fine, 9}, {kBalance, 50}, {kL1Wave, 60}, {kL1Rate, 0.12f}, {kL1Shape, 1},
                              {kF1Type, 1}, {kCutoff, 4000}, {kA, 600}, {kS, 90}, {kR, 1800}, {kChorus, 40}, {kRevMix, 40}, {kVolume, -11}});
        program("Noise Sweep FX", {{kO1Wave, 0}, {kBalance, 0}, {kNoise, 90}, {kF1Type, 2}, {kCutoff, 800}, {kReso, 60}, {kL1Rate, 0.2f}, {kL1Cut, 90}, {kL1Shape, 1},
                                   {kA, 400}, {kS, 100}, {kR, 2000}, {kRevMix, 50}, {kRevTime, 6.0f}, {kVolume, -10}});
        program("Square Lead", {{kO1Wave, 1}, {kO1Pw, 50}, {kL1Pw, 40}, {kL1Rate, 0.8f}, {kO2Wave, 1}, {kO2Fine, 10}, {kBalance, 45}, {kCutoff, 3500}, {kReso, 25},
                                {kEnvAmt, 20}, {kMode, 2}, {kGlide, 50}, {kA, 3}, {kS, 90}, {kDelayMix, 25}, {kRevMix, 20}});
        program("Reese Bass", {{kO1Wave, 0}, {kO2Wave, 0}, {kO2Fine, 22}, {kBalance, 50}, {kUniVoices, 2}, {kUniDetune, 25}, {kUniSpread, 40}, {kCutoff, 600},
                               {kReso, 20}, {kEnvAmt, 10}, {kDrive, 55}, {kMode, 2}, {kSub, 30}, {kS, 100}, {kR, 150}, {kChorus, 0}, {kRevMix, 0}});
        program("Init", {{kO1Wave, 0}, {kBalance, 0}, {kO2Fine, 0}, {kCutoff, 20000}, {kReso, 0}, {kEnvAmt, 0}, {kChorus, 0}, {kRevMix, 0}, {kDrive, 0}});
    }

    const std::vector<wr::Wave>& waves_;
    double sr_ = 44100.0;
    Voice voices_[kVoices];
    unsigned age_ = 0;
    bool sustain_ = false;
    int heldKeys_[16] = {}, held_ = 0;
    float lastNote_ = -1.0f;
    // arp
    int arpKeys_[16] = {}, arpOrder_[16] = {}, arpHeld_ = 0, arpPos_ = 0, arpDir_ = 1, arpNote_ = -1, arpVel_ = 100;
    double arpClock_ = 0.0, arpOffAt_ = -1.0;
    bool arpStepNow_ = false, arpWasOn_ = false;
    uint32_t rngArp_ = 777u, lfoRng_ = 4242u;
    float lfo1Ph_ = 0.0f, lfo2Ph_ = 0.0f, sh1_ = 0.0f, sh2_ = 0.0f;
    float bufL_[kMaxBlock], bufR_[kMaxBlock];
    DelayLine choL_, dlyL_, dlyR_;
    Reverb reverb_;
    float choPh_ = 0.0f;
    Smoother volume_;
    MidiControllers controllers_;
};

MPC_PLUGIN_ENTRY(MeltVenom)
