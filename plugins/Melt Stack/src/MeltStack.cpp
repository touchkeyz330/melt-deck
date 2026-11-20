// MeltStack.cpp - a 4-oscillator, 4-layer analog-modelling synth for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// An original design in the spirit of today's layered analog-modelling flagships. No code,
// sounds, presets or names are taken from any product. Per voice:
//   4 OSCILLATORS, each Classic (saw -> square -> narrow pulse), Shaper (wavefolder) or Digital
//   (32 band-limited waves, morphing); semi + level each; Spread detunes osc 2-4.
//   + Noise, Ring (osc1 x osc2), Sync (osc 2 to osc 1), X-Mod (osc 1 phase-modulates osc 2-4).
//   FILTER 1: Ladder 24 / MS 12 (screaming, saturating) / BP / HP; FILTER 2: Off / HP / LP / Notch
//   at an offset, in series. Filter + amp ADSR, key track, velocity.
//   MOTION SEQ: a per-voice step sequencer that starts with every note: 8 Pitch steps and
//   8 Shape steps (to cutoff, osc shape, level, pan or resonance), synced rate, 1-8 steps, smooth.
// LAYERS: four variations (A-D) of the program, each with its own transpose, tone and level,
//   played Single (A), Stack (all four), Rotate (each key press uses the next layer) or Random.
// KAOSS-STYLE PHYSICS: a ball on an X/Y surface. Kaoss X/Y (put them on Q-Links) pull it, Gravity
//   pulls it down, Friction slows it, walls bounce it. Its X and Y modulate any of Cutoff, Shape,
//   Pitch, Resonance, LFO rate - or VECTOR: blend the four stacked layers by where the ball is.
// LFO, chorus, synced delay, reverb.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/svf.h"
#include "dsp/adsr.h"
#include "dsp/smoother.h"
#include "dsp/delay_line.h"
#include "dsp/midi_cc.h"
#include "waves.h"
#include "reverb.h"

#include <algorithm>
#include <mutex>

using namespace mpc::dsp;
static constexpr float kPiF = 3.14159265358979f;

static const std::vector<wr::Wave>& stackWaves()
{
    static std::vector<wr::Wave> w;
    static std::once_flag once;
    std::call_once(once, [] { w = wr::builtinWaves(); });
    return w;
}

class MeltStack : public mpc::Plugin
{
public:
    enum Params
    {
        // OSC (4 x Type, Shape, Semi, Level)
        kOsc1 = 0,
        // MIX/FX
        kNoise = 16, kRing, kSync, kXMod, kKeyMode, kGlide, kVoices, kSpread, kVolume, kChorus, kDelay, kDelayTime, kReverb, kLfoRate, kLfoDest, kLfoAmt,
        // FILTER/ENV
        kF1Type, kCutoff, kReso, kF2Type, kF2Off, kEnvAmt, kKeyTrk, kFA, kFD, kFS, kFR, kA, kD, kS, kR,
        // LAYERS
        kLayerMode, kLayerSpread, kLayer1,                              // 4 x Semi, Tone, Level
        // SEQ STEPS
        kPitch1 = kLayer1 + 12, kShape1 = kPitch1 + 8,
        // SEQ / KAOSS
        kSeqOn = kShape1 + 8, kSeqRate, kSeqSteps, kSeqSmooth, kSeqDest, kSeqAmt,
        kKX, kKY, kGravity, kFriction, kKXDest, kKXAmt, kKYDest, kKYAmt,
        kBallX, kBallY,   // display only: where the ball is
        kNumParams
    };
    enum OscP { oType, oShape, oSemi, oLevel, kOscStride };
    enum KDest { kdOff, kdCutoff, kdShape, kdPitch, kdReso, kdLfo, kdVector };
    static int op(int o, int p) { return kOsc1 + o * kOscStride + p; }
    static int lp(int l, int p) { return kLayer1 + l * 3 + p; }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt Stack";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'n', 'j', 'R');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    MeltStack() : mpc::Plugin(makeInfo()), waves_(stackWaves())
    {
        using namespace mpc;
        for (int o = 0; o < 4; ++o)
        {
            const std::string n = std::to_string(o + 1);
            addParam(choiceParam(("o" + n + "t").c_str(), ("Osc" + n + " Type").c_str(), {"Classic", "Shaper", "Digital"}, 0));
            addParam(floatParam(("o" + n + "sh").c_str(), ("Osc" + n + " Shape").c_str(), 0.0f, 100.0f, 0.0f, "", 0));
            addParam(intParam(("o" + n + "se").c_str(), ("Osc" + n + " Semi").c_str(), -24, 24, o == 0 ? 0 : o == 1 ? 0 : o == 2 ? -12 : 7, "st"));
            addParam(floatParam(("o" + n + "l").c_str(), ("Osc" + n + " Level").c_str(), 0.0f, 100.0f, o == 0 ? 80.0f : o == 1 ? 70.0f : 0.0f, "%", 0));
        }
        addParam(floatParam("noise", "Noise", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("ring", "Ring 1x2", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(boolParam("sync", "Sync 2>1", false));
        addParam(floatParam("xmod", "X-Mod", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("keym", "Key Mode", {"Poly", "Mono", "Legato"}, 0));
        addParam(floatParam("glide", "Glide", 0.0f, 2000.0f, 0.0f, "ms", 0, 0.3f));
        addParam(intParam("voices", "Voices", 1, kMaxVoices, 12));
        addParam(floatParam("spread", "Spread", 0.0f, 100.0f, 20.0f, "%", 0));
        addParam(floatParam("vol", "Volume", -40.0f, 6.0f, -6.0f, "dB", 1));
        addParam(floatParam("cho", "Chorus", 0.0f, 100.0f, 20.0f, "%", 0));
        addParam(floatParam("dly", "Delay", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("dlyt", "Delay Time", {"1/16", "1/8", "3/16", "1/4", "3/8", "1/2"}, 2));
        addParam(floatParam("rev", "Reverb", 0.0f, 100.0f, 15.0f, "%", 0));
        addParam(logParam("lfor", "LFO Rate", 0.05f, 20.0f, 5.0f, "Hz", 2));
        addParam(choiceParam("lfod", "LFO Dest", {"Pitch", "Cutoff", "Shape", "Level", "Pan"}, 0));
        addParam(floatParam("lfoa", "LFO Amount", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("f1", "Filter 1", {"Ladder 24", "MS 12", "BP 12", "HP 12"}, 0));
        addParam(logParam("cut", "Cutoff", 20.0f, 20000.0f, 3500.0f, "Hz", 0));
        addParam(floatParam("res", "Resonance", 0.0f, 100.0f, 15.0f, "%", 0));
        addParam(choiceParam("f2", "Filter 2", {"Off", "HP", "LP", "Notch"}, 0));
        addParam(floatParam("f2o", "F2 Offset", -48.0f, 48.0f, 0.0f, "st", 1));
        addParam(floatParam("enva", "Env Amount", -100.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam("kbd", "Key Track", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(logParam("fa", "F Attack", 0.5f, 10000.0f, 2.0f, "ms", 0));
        addParam(logParam("fd", "F Decay", 1.0f, 10000.0f, 600.0f, "ms", 0));
        addParam(floatParam("fs", "F Sustain", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(logParam("fr", "F Release", 1.0f, 10000.0f, 400.0f, "ms", 0));
        addParam(logParam("a", "Attack", 0.5f, 10000.0f, 3.0f, "ms", 0));
        addParam(logParam("d", "Decay", 1.0f, 10000.0f, 800.0f, "ms", 0));
        addParam(floatParam("s", "Sustain", 0.0f, 100.0f, 85.0f, "%", 0));
        addParam(logParam("r", "Release", 1.0f, 10000.0f, 400.0f, "ms", 0));
        addParam(choiceParam("lmode", "Layer Mode", {"Single", "Stack", "Rotate", "Random"}, 0));
        addParam(floatParam("lspread", "Layer Pan", 0.0f, 100.0f, 50.0f, "%", 0));
        static const char* L = "ABCD";
        static const int defSemi[4] = {0, 12, -12, 7};
        static const float defTone[4] = {0.0f, 20.0f, -20.0f, 10.0f};
        for (int l = 0; l < 4; ++l)
        {
            const std::string n(1, L[l]);
            addParam(intParam(("l" + n + "s").c_str(), ("Layer" + n + " Semi").c_str(), -24, 24, defSemi[l], "st"));
            addParam(floatParam(("l" + n + "t").c_str(), ("Layer" + n + " Tone").c_str(), -100.0f, 100.0f, defTone[l], "", 0));
            addParam(floatParam(("l" + n + "v").c_str(), ("Layer" + n + " Level").c_str(), 0.0f, 100.0f, 80.0f, "%", 0));
        }
        static const int defPitch[8] = {0, 0, 12, 0, 7, 0, 12, 3};
        static const float defShape[8] = {100, 20, 60, 10, 80, 30, 50, 0};
        for (int s = 0; s < 8; ++s)
        {
            const std::string n = std::to_string(s + 1);
            addParam(intParam(("sp" + n).c_str(), ("Pitch " + n).c_str(), -24, 24, defPitch[s], "st"));
        }
        for (int s = 0; s < 8; ++s)
        {
            const std::string n = std::to_string(s + 1);
            addParam(floatParam(("ss" + n).c_str(), ("Shape " + n).c_str(), -100.0f, 100.0f, defShape[s], "", 0));
        }
        addParam(boolParam("seq", "Motion Seq", false));
        addParam(choiceParam("seqr", "Seq Rate", {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"}, 3));
        addParam(intParam("seqn", "Seq Steps", 1, 8, 8));
        addParam(floatParam("seqsm", "Seq Smooth", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("seqd", "Shape Dest", {"Cutoff", "Osc Shape", "Level", "Pan", "Resonance"}, 0));
        addParam(floatParam("seqa", "Shape Amount", 0.0f, 100.0f, 60.0f, "%", 0));
        addParam(floatParam("kx", "Kaoss X", -100.0f, 100.0f, 0.0f, "", 0));
        addParam(floatParam("ky", "Kaoss Y", -100.0f, 100.0f, 0.0f, "", 0));
        addParam(floatParam("grav", "Gravity", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("fric", "Friction", 0.0f, 100.0f, 40.0f, "%", 0));
        const std::vector<std::string> kd = {"Off", "Cutoff", "Shape", "Pitch", "Resonance", "LFO Rate", "Vector"};
        addParam(choiceParam("kxd", "Kaoss X Dest", kd, 1));
        addParam(floatParam("kxa", "Kaoss X Amt", -100.0f, 100.0f, 60.0f, "%", 0));
        addParam(choiceParam("kyd", "Kaoss Y Dest", kd, 2));
        addParam(floatParam("kya", "Kaoss Y Amt", -100.0f, 100.0f, 60.0f, "%", 0));
        { Param b = floatParam("bx", "Ball X", -100.0f, 100.0f, 0.0f, "", 0); b.automatable = false; addParam(b); }
        { Param b = floatParam("by", "Ball Y", -100.0f, 100.0f, 0.0f, "", 0); b.automatable = false; addParam(b); }
        if (numParams() != kNumParams) mpc::log("MeltStack: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);
        makePrograms();
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        for (auto& v : voices_) { v.fenv.setSampleRate(sampleRate); v.aenv.setSampleRate(sampleRate); for (auto& f : v.svf) f.setSampleRate(sampleRate); }
        choL_.prepare((int)(0.04 * sampleRate) + 4);
        dlyL_.prepare((int)(2.1 * sampleRate) + 4); dlyR_.prepare((int)(2.1 * sampleRate) + 4);
        reverb_.prepare(sampleRate);
        volume_.setTime(15.0f, sampleRate);
        controllers_.prepare(sampleRate / kCtl, 10.0f);
        reset();
    }

    void reset() override
    {
        for (auto& v : voices_) v.kill();
        choL_.reset(); dlyL_.reset(); dlyR_.reset(); reverb_.reset();
        choPh_ = lfoPh_ = 0.0f;
        bx_ = by_ = vx_ = vy_ = 0.0f;
        ballHold_ = (int)(0.5 * sr_);
        sustain_ = false; age_ = 0; held_ = 0; lastNote_ = -1.0f; rotate_ = 0;
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
        // show the ball (left alone briefly after a project/preset load so saved state reads back exactly)
        if (ballHold_ > 0) ballHold_ -= ctx.numFrames;
        else { set(kBallX, bx_ * 100.0f); set(kBallY, by_ * 100.0f); }
    }

private:
    static constexpr int kMaxVoices = 16, kCtl = 16, kMaxBlock = 4096;

    struct Voice
    {
        int note = -1, layer = 0;
        bool held = false, sustained = false, active = false;
        unsigned age = 0;
        float vel = 1.0f, pitch = 60.0f, target = 60.0f, time = 0.0f;
        float ph[4] = {}, o1last = 0.0f;
        float lad[2][4] = {};
        SVF svf[4];
        ADSR fenv, aenv;
        uint32_t rng = 1;
        double seqClock = 0.0; int step = 0;
        float seqPitch = 0.0f, seqShape = 0.0f;
        void kill() { active = false; note = -1; held = sustained = false; fenv.reset(); aenv.reset(); for (auto& s : svf) s.reset(); for (auto& a : lad) for (float& x : a) x = 0.0f; }
    };

    struct Settings
    {
        int type[4] = {}, keyMode = 0, voices = 12, f1 = 0, f2 = 0, layerMode = 0, lfoDest = 0, seqDest = 0, seqSteps = 8, kxd = 1, kyd = 2;
        float shape[4] = {}, semi[4] = {}, level[4] = {};
        float noise = 0, ring = 0, xmod = 0, glide = 0, spread = 0.2f, cho = 0.2f, dly = 0, rev = 0.15f, lfoHz = 5, lfoAmt = 0;
        bool sync = false, seq = false;
        float cutoff = 3500, reso = 0.15f, f2off = 0, envAmt = 0.3f, kbd = 0.5f, env[8] = {};
        float lSemi[4] = {}, lTone[4] = {}, lLevel[4] = {}, lPan = 0.5f;
        float sPitch[8] = {}, sShape[8] = {}, seqBeats = 0.25f, seqSmooth = 0, seqAmt = 0.6f;
        float kx = 0, ky = 0, grav = 0, fric = 0.4f, kxa = 0.6f, kya = 0.6f;
        float dlyBeats = 0.75f;
        double bpm = 120.0;
    } s_;

    void readSettings(mpc::ProcessContext& ctx)
    {
        static const float seqBeats[6] = {1.0f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f};
        static const float dBeats[6] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
        s_.bpm = ctx.transport.tempoValid && ctx.transport.bpm > 20.0 ? ctx.transport.bpm : 120.0;
        for (int o = 0; o < 4; ++o)
        {
            s_.type[o] = getInt(op(o, oType)); s_.shape[o] = get(op(o, oShape)) * 0.01f;
            s_.semi[o] = (float)getInt(op(o, oSemi)); s_.level[o] = get(op(o, oLevel)) * 0.01f;
        }
        s_.noise = get(kNoise) * 0.01f; s_.ring = get(kRing) * 0.01f; s_.sync = getBool(kSync); s_.xmod = get(kXMod) * 0.01f;
        s_.keyMode = getInt(kKeyMode); s_.glide = get(kGlide); s_.voices = getInt(kVoices); s_.spread = get(kSpread) * 0.01f;
        s_.cho = get(kChorus) * 0.01f; s_.dly = get(kDelay) * 0.01f; s_.dlyBeats = dBeats[mpc::clamp(getInt(kDelayTime), 0, 5)]; s_.rev = get(kReverb) * 0.01f;
        s_.lfoHz = get(kLfoRate); s_.lfoDest = getInt(kLfoDest); s_.lfoAmt = get(kLfoAmt) * 0.01f;
        s_.f1 = getInt(kF1Type); s_.cutoff = get(kCutoff); s_.reso = get(kReso) * 0.01f; s_.f2 = getInt(kF2Type); s_.f2off = get(kF2Off);
        s_.envAmt = get(kEnvAmt) * 0.01f; s_.kbd = get(kKeyTrk) * 0.01f;
        for (int k = 0; k < 8; ++k) s_.env[k] = get(kFA + k);
        s_.layerMode = getInt(kLayerMode); s_.lPan = get(kLayerSpread) * 0.01f;
        for (int l = 0; l < 4; ++l) { s_.lSemi[l] = (float)getInt(lp(l, 0)); s_.lTone[l] = get(lp(l, 1)) * 0.01f; s_.lLevel[l] = get(lp(l, 2)) * 0.01f; }
        for (int k = 0; k < 8; ++k) { s_.sPitch[k] = (float)getInt(kPitch1 + k); s_.sShape[k] = get(kShape1 + k) * 0.01f; }
        s_.seq = getBool(kSeqOn); s_.seqBeats = seqBeats[mpc::clamp(getInt(kSeqRate), 0, 5)]; s_.seqSteps = getInt(kSeqSteps);
        s_.seqSmooth = get(kSeqSmooth) * 0.01f; s_.seqDest = getInt(kSeqDest); s_.seqAmt = get(kSeqAmt) * 0.01f;
        s_.kx = get(kKX) * 0.01f; s_.ky = get(kKY) * 0.01f; s_.grav = get(kGravity) * 0.01f; s_.fric = get(kFriction) * 0.01f;
        s_.kxd = getInt(kKXDest); s_.kxa = get(kKXAmt) * 0.01f; s_.kyd = getInt(kKYDest); s_.kya = get(kKYAmt) * 0.01f;
        reverb_.set(2.8f, 0.35f);
        volume_.setTarget(dbToGain(get(kVolume)));
        for (auto& v : voices_)
            if (v.active) { v.fenv.set(s_.env[0], s_.env[1], s_.env[2] * 0.01f, s_.env[3]); v.aenv.set(s_.env[4], s_.env[5], s_.env[6] * 0.01f, s_.env[7]); }
    }

    // ---- notes ----------------------------------------------------------------------
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

    void noteOn(int note, int vel)
    {
        const float from = lastNote_ < 0 ? (float)note : lastNote_;
        lastNote_ = (float)note;
        // which layers play this note
        int layers[4]; int nl = 0;
        switch (s_.layerMode)
        {
            case 1: for (int l = 0; l < 4; ++l) layers[nl++] = l; break;
            case 2: layers[nl++] = rotate_; rotate_ = (rotate_ + 1) & 3; break;
            case 3: rngL_ = rngL_ * 1664525u + 1013904223u; layers[nl++] = (int)((rngL_ >> 8) & 3); break;
            default: layers[nl++] = 0; break;
        }
        if (s_.keyMode >= 1)
        {
            if (held_ < 16) heldKeys_[held_++] = note;
            for (int i = 0; i < nl; ++i)
            {
                Voice& v = voices_[i];   // mono: one voice per playing layer, fixed slots
                if (v.active && (v.held || v.sustained))
                {
                    v.note = note; v.held = true; v.target = (float)note; v.layer = layers[i];
                    if (s_.glide <= 0.0f) v.pitch = (float)note;
                    if (s_.keyMode == 1) { v.vel = (float)vel / 127.0f; v.fenv.noteOn(); v.aenv.noteOn(); restartSeq(v); }
                }
                else start(v, note, vel, layers[i], s_.glide > 0.0f ? from : (float)note);
            }
            return;
        }
        for (int i = 0; i < nl; ++i) start(allocate(), note, vel, layers[i], s_.glide > 0.0f ? from : (float)note);
    }

    void noteOff(int note)
    {
        if (s_.keyMode >= 1)
        {
            int w = 0;
            for (int i = 0; i < held_; ++i) if (heldKeys_[i] != note) heldKeys_[w++] = heldKeys_[i];
            held_ = w;
            if (held_ > 0)
            {
                bool moved = false;
                for (auto& v : voices_)
                    if (v.active && v.note == note) { v.note = heldKeys_[held_ - 1]; v.target = (float)v.note; if (s_.glide <= 0.0f) v.pitch = v.target; moved = true; }
                if (moved) { lastNote_ = (float)heldKeys_[held_ - 1]; return; }
            }
        }
        for (auto& v : voices_)
            if (v.held && v.note == note) { v.held = false; if (sustain_) v.sustained = true; else release(v); }
    }

    void restartSeq(Voice& v) { v.seqClock = 0.0; v.step = 0; v.seqPitch = s_.sPitch[0]; v.seqShape = s_.sShape[0]; }

    void start(Voice& v, int note, int vel, int layer, float from)
    {
        const bool was = v.active;
        v.active = true; v.note = note; v.layer = layer; v.held = true; v.sustained = false; v.age = ++age_;
        v.vel = (float)vel / 127.0f; v.pitch = from; v.target = (float)note; v.time = 0.0f;
        v.rng = 0x9E3779B9u * (age_ + 11);
        if (!was)
        {
            for (auto& s : v.svf) s.reset();
            for (auto& a : v.lad) for (float& x : a) x = 0.0f;
            for (int o = 0; o < 4; ++o) v.ph[o] = rnd01(v.rng);   // free-running analog-style phases
            v.fenv.reset(); v.aenv.reset();
        }
        restartSeq(v);
        v.fenv.set(s_.env[0], s_.env[1], s_.env[2] * 0.01f, s_.env[3]);
        v.aenv.set(s_.env[4], s_.env[5], s_.env[6] * 0.01f, s_.env[7]);
        v.fenv.noteOn(); v.aenv.noteOn();
    }

    void release(Voice& v) { v.held = v.sustained = false; v.fenv.noteOff(); v.aenv.noteOff(); }

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

    // ---- sound -----------------------------------------------------------------------------
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
    inline float osc(int type, float shape, float ph, float dt, int lv) const
    {
        switch (type)
        {
            case 1:   // Shaper: a triangle through a sine wavefolder; Shape = fold drive
            {
                const float t = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
                return std::sin(0.5f * kPiF * t * (1.0f + 5.0f * shape));
            }
            case 2:   // Digital: morph across the 32 waves
            {
                const float w = shape * 31.0f; const int a = std::min(30, (int)w); const float m = w - (float)a;
                auto rd = [&](int idx) { const float* tb = waves_[(size_t)idx].level(lv); const float p = ph * (float)wr::kTable; const int i = (int)p; const float f = p - (float)i; return tb[i] + f * (tb[i + 1] - tb[i]); };
                const float va = rd(a);
                return (va + m * (rd(a + 1) - va)) * 2.2f;
            }
            default:  // Classic: saw -> square (0..50 %) -> narrow pulse (50..100 %)
            {
                if (shape < 0.5f) { const float s = saw(ph, dt), q = pulse(ph, dt, 0.5f); return s + 2.0f * shape * (q - s); }
                return pulse(ph, dt, 0.5f - (shape - 0.5f) * 0.9f);
            }
        }
    }

    void physics(float dt)
    {
        // spring toward the Kaoss X/Y knobs, gravity down, friction, bouncy walls
        // more Gravity = a looser pull from the knobs, so the ball can really drop and bounce
        const float spring = 40.0f * (1.0f - 0.9f * s_.grav), damping = 0.3f + 20.0f * s_.fric * s_.fric;
        float ax = spring * (s_.kx - bx_) - damping * vx_;
        float ay = spring * (s_.ky - by_) - damping * vy_ - 30.0f * s_.grav;
        vx_ += ax * dt; vy_ += ay * dt;
        bx_ += vx_ * dt; by_ += vy_ * dt;
        const float bounce = 0.9f - 0.8f * s_.fric;   // friction also absorbs on impact
        if (bx_ > 1.0f) { bx_ = 1.0f; vx_ = -vx_ * bounce; } else if (bx_ < -1.0f) { bx_ = -1.0f; vx_ = -vx_ * bounce; }
        if (by_ > 1.0f) { by_ = 1.0f; vy_ = -vy_ * bounce; } else if (by_ < -1.0f) { by_ = -1.0f; vy_ = -vy_ * bounce; }
    }

    void render(int from, int to) { for (int c = from; c < to; c += kCtl) renderChunk(c, std::min(kCtl, to - c)); }

    void renderChunk(int off, int n)
    {
        controllers_.tick();
        const float dtc = (float)n / (float)sr_;
        physics(dtc);
        // Kaoss modulation (global)
        float kCut = 0, kShape = 0, kPitch = 0, kReso = 0, kLfo = 0;
        bool vector = false;
        auto applyK = [&](int dest, float val) {
            switch (dest)
            {
                case kdCutoff: kCut += val * 4.0f; break;     // octaves
                case kdShape: kShape += val * 0.5f; break;
                case kdPitch: kPitch += val * 12.0f; break;
                case kdReso: kReso += val * 0.6f; break;
                case kdLfo: kLfo += val * 2.0f; break;
                case kdVector: vector = true; break;
                default: break;
            } };
        applyK(s_.kxd, bx_ * s_.kxa);
        applyK(s_.kyd, by_ * s_.kya);
        float layerGain[4] = {1, 1, 1, 1};
        if (vector)
        {
            const float x = (bx_ + 1.0f) * 0.5f, y = (by_ + 1.0f) * 0.5f;
            layerGain[0] = (1 - x) * (1 - y); layerGain[1] = x * (1 - y); layerGain[2] = (1 - x) * y; layerGain[3] = x * y;
            for (float& g : layerGain) g = std::sqrt(g) * 1.0f;   // equal-power-ish
        }
        lfoPh_ += s_.lfoHz * std::pow(2.0f, kLfo) * dtc; if (lfoPh_ >= 1.0f) lfoPh_ -= std::floor(lfoPh_);
        const float lfo = std::sin(2.0f * kPiF * lfoPh_) * s_.lfoAmt;
        const float wheelVib = controllers_.modWheel() * 0.4f * std::sin(2.0f * kPiF * lfoPh_);
        const float bendSemis = controllers_.pitchBend() * 2.0f;
        const float glide = s_.glide > 0.0f ? std::exp(-dtc / (s_.glide * 0.001f * 0.4f)) : 0.0f;
        const double stepLen = s_.seqBeats * 60.0 / s_.bpm * sr_;
        static const float spreadCents[4] = {0.0f, 7.0f, -9.0f, 12.0f};

        for (auto& v : voices_)
        {
            if (!v.active) continue;
            v.pitch = v.target + (v.pitch - v.target) * glide;
            // ---- motion sequencer (per voice, from its own note-on)
            float sPitch = 0.0f, sShape = 0.0f;
            if (s_.seq)
            {
                const int steps = mpc::clamp(s_.seqSteps, 1, 8);
                v.seqClock += n;
                while (v.seqClock >= stepLen) { v.seqClock -= stepLen; v.step = (v.step + 1) % steps; }
                const float frac = (float)(v.seqClock / stepLen);
                const int nx = (v.step + 1) % steps;
                // Smooth: glide toward the next step over the last part of this one
                const float g = s_.seqSmooth > 0.0f ? mpc::clamp((frac - (1.0f - s_.seqSmooth)) / s_.seqSmooth, 0.0f, 1.0f) : 0.0f;
                sPitch = s_.sPitch[v.step] + g * (s_.sPitch[nx] - s_.sPitch[v.step]);
                sShape = (s_.sShape[v.step] + g * (s_.sShape[nx] - s_.sShape[v.step])) * s_.seqAmt;
            }
            const int L = v.layer;
            const float tone = s_.lTone[L];
            const float note = v.pitch + s_.lSemi[L] + bendSemis + kPitch + sPitch + wheelVib + (s_.lfoDest == 0 ? lfo * 1.0f : 0.0f);
            float dts[4]; int lvs[4];
            for (int o = 0; o < 4; ++o)
            {
                const float semis = note + s_.semi[o] + spreadCents[o] * s_.spread * 0.01f * 3.0f;
                dts[o] = std::min(0.45f, 440.0f * std::pow(2.0f, (semis - 69.0f) / 12.0f) / (float)sr_);
                lvs[o] = wr::levelFor(dts[o]);
            }
            const float shapeMod = kShape + tone * 0.3f + (s_.seqDest == 1 ? sShape * 0.5f : 0.0f) + (s_.lfoDest == 2 ? lfo * 0.4f : 0.0f);
            float sh[4];
            for (int o = 0; o < 4; ++o) sh[o] = mpc::clamp(s_.shape[o] + shapeMod, 0.0f, 1.0f);
            const float oct = s_.envAmt * v.fenv.level() * 6.0f + s_.kbd * (v.pitch - 60.0f) / 12.0f + kCut + tone * 2.0f
                            + (s_.seqDest == 0 ? sShape * 4.0f : 0.0f) + (s_.lfoDest == 1 ? lfo * 3.0f : 0.0f) + (v.vel - 1.0f) * 1.0f;
            const float fc = mpc::clamp(s_.cutoff * std::pow(2.0f, mpc::clamp(oct, -10.0f, 10.0f)), 20.0f, 0.45f * (float)sr_);
            const float fc2 = mpc::clamp(fc * std::pow(2.0f, s_.f2off / 12.0f), 20.0f, 0.45f * (float)sr_);
            const float reso = mpc::clamp(s_.reso + kReso + (s_.seqDest == 4 ? sShape * 0.5f : 0.0f), 0.0f, 1.0f);
            const float g = std::tan(kPiF * fc / (float)sr_), G = g / (1.0f + g), ia = 1.0f / (1.0f + g), G4 = G * G * G * G, kq = reso * 4.1f;
            if (s_.f1 > 0) v.svf[0].set(fc, s_.f1 == 1 ? std::min(0.98f, reso * 1.05f) : reso);
            if (s_.f2 > 0) v.svf[2].set(fc2, 0.2f);
            // level and pan for this voice's layer
            static const float layerPan[4] = {-1.0f, 1.0f, -0.33f, 0.33f};
            float pan = s_.layerMode == 1 ? layerPan[L] * s_.lPan : 0.0f;
            if (s_.seqDest == 3) pan += sShape;
            if (s_.lfoDest == 4) pan += lfo;
            pan = mpc::clamp(pan, -1.0f, 1.0f);
            const float pa = (pan + 1.0f) * 0.25f * kPiF;
            float lev = s_.lLevel[L] * (s_.layerMode == 1 ? layerGain[L] : 1.0f) * (0.5f + 0.5f * v.vel);
            if (s_.seqDest == 2) lev *= mpc::clamp(1.0f + sShape, 0.0f, 2.0f);
            if (s_.lfoDest == 3) lev *= 1.0f - 0.5f * s_.lfoAmt * (1.0f - std::sin(2.0f * kPiF * lfoPh_));
            const float gl = std::cos(pa) * 1.4142f * lev * 0.6f, gr = std::sin(pa) * 1.4142f * lev * 0.6f;

            float* bl = bufL_ + off;
            float* br = bufR_ + off;
            for (int i = 0; i < n; ++i)
            {
                // osc 1 first (sync, ring, x-mod all listen to it)
                const float p1 = v.ph[0];
                const float o1 = osc(s_.type[0], sh[0], p1, dts[0], lvs[0]);
                v.ph[0] += dts[0];
                bool wrapped = false; float wf = 0.0f;
                if (v.ph[0] >= 1.0f) { v.ph[0] -= 1.0f; wrapped = true; wf = v.ph[0] / dts[0]; }
                float x = o1 * s_.level[0];
                float o2 = 0.0f;
                for (int o = 1; o < 4; ++o)
                {
                    if (o == 1 && s_.sync && wrapped) v.ph[1] = wf * dts[1];
                    float ph = v.ph[o];
                    if (s_.xmod > 0.0f) { ph += s_.xmod * o1 * 0.6f; ph -= std::floor(ph); }
                    const float y = (s_.level[o] > 0.0f || (o == 1 && s_.ring > 0.0f)) ? osc(s_.type[o], sh[o], ph, dts[o], lvs[o]) : 0.0f;
                    if (o == 1) o2 = y;
                    x += y * s_.level[o];
                    v.ph[o] += dts[o]; if (v.ph[o] >= 1.0f) v.ph[o] -= 1.0f;
                }
                x += o1 * o2 * s_.ring;
                if (s_.noise > 0.0f) x += (rnd01(v.rng) * 2.0f - 1.0f) * s_.noise * 0.7f;
                x *= 0.45f;
                // the voice is mono until it is panned, so one filter path is enough
                float a1;
                if (s_.f1 == 0)
                {
                    float* st = v.lad[0];
                    const float S = G * G * G * ia * st[0] + G * G * ia * st[1] + G * ia * st[2] + ia * st[3];
                    const float y4 = (G4 * x + S) / (1.0f + kq * G4);
                    float u = fastTanh(x - kq * y4);
                    for (int s = 0; s < 4; ++s) { const float vv = (u - st[s]) * G; const float yy = vv + st[s]; st[s] = yy + vv; u = yy; }
                    a1 = u * (1.0f + kq * 0.35f);
                }
                else if (s_.f1 == 1) a1 = fastTanh(v.svf[0].process(x * 1.5f, 0)) * 0.8f;   // MS-style: screams and saturates
                else a1 = v.svf[0].process(x, s_.f1 == 2 ? 1 : 2);
                if (s_.f2 > 0) a1 = v.svf[2].process(a1, s_.f2 == 1 ? 2 : s_.f2 == 2 ? 0 : 3);
                const float y[2] = {a1, a1};
                v.fenv.process();
                const float a = v.aenv.process();
                bl[i] += y[0] * a * gl;
                br[i] += y[1] * a * gr;
            }
            v.time += dtc;
            if (!v.aenv.isActive() || (v.aenv.stage() == ADSR::Stage::Sustain && v.aenv.level() < 1.0e-4f)) v.kill();
        }
    }

    void effects(float* L, float* R, int n)
    {
        const float choInc = 0.35f / (float)sr_;
        const float base = 0.012f * (float)sr_, depth = 0.005f * (float)sr_;
        const float dlen = std::min((float)(s_.dlyBeats * 60.0 / s_.bpm * sr_), 2.0f * (float)sr_);
        for (int i = 0; i < n; ++i)
        {
            float l = bufL_[i], r = bufR_[i];
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
            dlyL_.write((s_.dly > 0.0f ? (l + r) * 0.5f : 0.0f) + tr * 0.4f);
            dlyR_.write(tl * 0.4f);
            l += tl * s_.dly; r += tr * s_.dly;
            float rl, rr;
            reverb_.process(s_.rev > 0.0f ? l : 0.0f, s_.rev > 0.0f ? r : 0.0f, rl, rr);
            l += rl * s_.rev; r += rr * s_.rev;
            const float gv = volume_.process();
            l = softClip(l * gv); r = softClip(r * gv);
            if (std::fabs(l) < 1.0e-20f) l = 0.0f;
            if (std::fabs(r) < 1.0e-20f) r = 0.0f;
            L[i] = sanitize(l);
            if (R) R[i] = sanitize(r);
        }
    }

    static float softClip(float x)
    {
        const float a = std::fabs(x);
        if (a < 0.7f) return x;
        const float y = 0.7f + 0.3f * fastTanh((a - 0.7f) / 0.3f);
        return x < 0 ? -y : y;
    }

    // ---- presets (all original) ---------------------------------------------------------
    void makePrograms()
    {
        auto program = [&](const char* name, std::initializer_list<std::pair<int, float>> o)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& p : o) v[(size_t)p.first] = p.second;
            addProgram(name, std::move(v));
        };
        auto O = [](int o, int p) { return op(o, p); };
        program("Four Osc Wall", {{O(0, oLevel), 80}, {O(1, oLevel), 75}, {O(1, oSemi), 0}, {O(2, oLevel), 60}, {O(2, oSemi), -12}, {O(3, oLevel), 45}, {O(3, oSemi), 7},
                                  {kSpread, 45}, {kCutoff, 2500}, {kReso, 15}, {kEnvAmt, 20}, {kA, 15}, {kS, 90}, {kR, 700}, {kChorus, 35}, {kReverb, 25}});
        program("Stack Vector Pad", {{kLayerMode, 1}, {kKXDest, kdVector}, {kKYDest, kdVector}, {O(0, oType), 2}, {O(0, oShape), 45}, {O(1, oType), 0}, {O(1, oShape), 30}, {O(1, oLevel), 50},
                                     {kCutoff, 3000}, {kEnvAmt, 0}, {kA, 900}, {kR, 2000}, {kChorus, 40}, {kReverb, 40}, {kVoices, 16}, {kVolume, -10}});
        program("Rotate Keys", {{kVolume, 0}, {kLayerMode, 2}, {O(0, oType), 0}, {O(0, oShape), 50}, {O(1, oLevel), 40}, {O(1, oSemi), 12}, {kCutoff, 1500}, {kReso, 25}, {kEnvAmt, 45},
                                {kFD, 300}, {kFS, 0}, {kD, 900}, {kS, 0}, {kR, 500}, {kDelay, 25}, {kReverb, 20}});
        program("Motion Seq Pluck", {{kSeqOn, 1}, {kSeqRate, 3}, {kSeqSteps, 8}, {kSeqDest, 0}, {kSeqAmt, 70}, {O(0, oShape), 20}, {O(1, oLevel), 50}, {O(1, oSemi), 12},
                                     {kCutoff, 900}, {kReso, 40}, {kEnvAmt, 20}, {kS, 100}, {kR, 300}, {kDelay, 25}, {kReverb, 20}});
        program("Seq Melody Bass", {{kVolume, 0}, {kSeqOn, 1}, {kSeqRate, 3}, {kSeqSteps, 8}, {kSeqDest, 0}, {kSeqAmt, 50}, {kKeyMode, 2}, {O(0, oShape), 60}, {O(2, oLevel), 60}, {O(2, oSemi), -12},
                                    {O(1, oLevel), 0}, {kCutoff, 500}, {kReso, 35}, {kEnvAmt, 30}, {kFD, 200}, {kChorus, 0}, {kReverb, 0}});
        program("Kaoss Bounce", {{kGravity, 100}, {kFriction, 8}, {kKY, 100}, {kKXDest, kdShape}, {kKYDest, kdCutoff}, {kKYAmt, 70}, {O(0, oType), 1}, {O(0, oShape), 40},
                                 {O(1, oType), 1}, {O(1, oSemi), 7}, {kCutoff, 1500}, {kReso, 35}, {kS, 100}, {kReverb, 25}});
        program("Shaper Fold Lead", {{O(0, oType), 1}, {O(0, oShape), 55}, {O(1, oType), 0}, {O(1, oLevel), 40}, {O(1, oSemi), -12}, {kKeyMode, 2}, {kGlide, 50}, {kF1Type, 1},
                                     {kCutoff, 3000}, {kReso, 40}, {kLfoDest, 2}, {kLfoAmt, 30}, {kLfoRate, 0.5f}, {kDelay, 25}});
        program("Sync X-Mod Lead", {{kVolume, -1}, {kSync, 1}, {kXMod, 30}, {O(0, oLevel), 30}, {O(1, oLevel), 90}, {O(1, oSemi), 12}, {kKXDest, kdPitch}, {kKXAmt, 0}, {kKeyMode, 2}, {kGlide, 30},
                                    {kCutoff, 4500}, {kEnvAmt, 10}, {kDelay, 20}, {kReverb, 15}});
        program("MS Acid", {{kF1Type, 1}, {O(0, oShape), 0}, {O(1, oLevel), 0}, {kCutoff, 400}, {kReso, 85}, {kEnvAmt, 60}, {kFD, 200}, {kFS, 0}, {kKeyMode, 2}, {kGlide, 60},
                            {kS, 100}, {kR, 60}, {kChorus, 0}, {kReverb, 5}});
        program("Ring Bells", {{kVolume, -2}, {kRing, 70}, {O(0, oType), 0}, {O(0, oShape), 50}, {O(1, oSemi), 19}, {O(1, oLevel), 20}, {kCutoff, 8000}, {kEnvAmt, 0}, {kA, 1}, {kD, 2500}, {kS, 0},
                               {kR, 2000}, {kReverb, 35}, {kChorus, 30}});
        program("Digital Morph Pad", {{kVolume, -12}, {O(0, oType), 2}, {O(0, oShape), 20}, {O(1, oType), 2}, {O(1, oShape), 60}, {O(1, oSemi), 0}, {kLfoDest, 2}, {kLfoAmt, 40}, {kLfoRate, 0.15f},
                                      {kF1Type, 0}, {kCutoff, 5000}, {kEnvAmt, 0}, {kA, 700}, {kR, 2000}, {kChorus, 40}, {kReverb, 45}});
        program("Init", {{O(1, oLevel), 0}, {kCutoff, 20000}, {kReso, 0}, {kEnvAmt, 0}, {kChorus, 0}, {kReverb, 0}, {kSpread, 0}});
    }

    const std::vector<wr::Wave>& waves_;
    double sr_ = 44100.0;
    Voice voices_[kMaxVoices];
    unsigned age_ = 0;
    bool sustain_ = false;
    int heldKeys_[16] = {}, held_ = 0, rotate_ = 0;
    uint32_t rngL_ = 4321u;
    float lastNote_ = -1.0f, lfoPh_ = 0.0f;
    float bx_ = 0.0f, by_ = 0.0f, vx_ = 0.0f, vy_ = 0.0f;
    int ballHold_ = 0;
    float bufL_[kMaxBlock], bufR_[kMaxBlock];
    DelayLine choL_, dlyL_, dlyR_;
    Reverb reverb_;
    float choPh_ = 0.0f;
    Smoother volume_;
    MidiControllers controllers_;
};

MPC_PLUGIN_ENTRY(MeltStack)
