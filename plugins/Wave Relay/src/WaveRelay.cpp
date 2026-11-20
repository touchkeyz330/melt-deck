// WaveRelay.cpp - a wave-sequencing synthesizer for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// An original design in the tradition of wave-sequencing synths: each note walks through a
// chain of up to 8 single-cycle waves, every step with its own wave, pitch and level, and
// crossfades from one to the next. A second "drone" layer and a vector-style sweep blend in
// a steady wave underneath; a filter, two envelopes and a stereo chorus finish the sound.
//
// Per voice (12 voices):
//   SEQUENCE: step wave (band-limited table) --crossfade--> next step wave, x step level
//   + LAYER B: drone wave (octave, detune)        mixed by Mix (+ Vector sweep over time)
//   -> SVF filter (cutoff, resonance, envelope, key track) -> amp ADSR x velocity
// Sum -> stereo chorus -> volume.
//
// Timing: steps follow the song tempo (Sync On, a note division) or a free time in ms.
// Key Sync On restarts the sequence on every note; Off runs one clock for all notes, locked
// to the song position while the MPC plays.
// Waves: 32 built in, plus any single-cycle WAVs in Content/Waves (frames of 2048-point
// wavetables become separate waves). The wave lists are fixed when the plugin loads.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/svf.h"
#include "dsp/adsr.h"
#include "dsp/smoother.h"
#include "dsp/tempo.h"
#include "dsp/delay_line.h"
#include "dsp/midi_cc.h"
#include "dsp/wav.h"
#include "waves.h"

#include <algorithm>
#include <mutex>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <dirent.h>
#endif

using namespace mpc::dsp;

static constexpr int kSteps = 8;

// ---------------------------------------------------------------------------
// Wave library (built once, shared by every instance)
// ---------------------------------------------------------------------------
static std::vector<std::string> listWavs(const std::string& dir)
{
    std::vector<std::string> names;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "/*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do { names.push_back(fd.cFileName); } while (FindNextFileA(h, &fd)); FindClose(h); }
#else
    if (DIR* d = opendir(dir.c_str())) { while (dirent* e = readdir(d)) names.push_back(e->d_name); closedir(d); }
#endif
    std::vector<std::string> keep;
    for (const auto& n : names)
        if (n.size() > 4 && n[0] != '.')
        {
            std::string ext = n.substr(n.size() - 4);
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext == ".wav") keep.push_back(n);
        }
    std::sort(keep.begin(), keep.end());
    return keep;
}

static const std::vector<wr::Wave>& waveLibrary()
{
    static std::vector<wr::Wave> lib;
    static std::once_flag once;
    std::call_once(once, []
    {
        lib = wr::builtinWaves();
        const std::string dir = mpc::contentPath("Waves");
        int added = 0;
        for (const auto& f : listWavs(dir))
        {
            dsp::WavData w;
            if (!dsp::loadWav(dir + "/" + f, w)) continue;
            const size_t n = w.numFrames();
            std::vector<float> mono(n);
            for (size_t i = 0; i < n; ++i) { float v = 0; for (int c = 0; c < w.numChannels(); ++c) v += w.channels[(size_t)c][i]; mono[i] = v / (float)w.numChannels(); }
            std::string base = f.substr(0, f.size() - 4);
            if (base.size() > 12) base = base.substr(0, 12);
            if (n > (size_t)wr::kTable && n % wr::kTable == 0)   // a wavetable: each 2048-point frame is a wave
            {
                const size_t frames = std::min<size_t>(n / wr::kTable, 16);
                for (size_t fr = 0; fr < frames && added < 96; ++fr, ++added)
                    lib.push_back(wr::makeWave(base + " " + std::to_string(fr + 1), wr::toCycle(mono.data() + fr * wr::kTable, wr::kTable)));
            }
            else if (n >= 16 && n <= 4096 && added < 96)        // a single cycle
            {
                lib.push_back(wr::makeWave(base, wr::toCycle(mono.data(), n)));
                ++added;
            }
            else mpc::log("WaveRelay: %s is not a single-cycle wave (16-4096 points) or a 2048-point wavetable; skipped", f.c_str());
        }
    });
    return lib;
}

// ---------------------------------------------------------------------------
// The plugin
// ---------------------------------------------------------------------------
class WaveRelay : public mpc::Plugin
{
public:
    enum Params
    {
        kVolume,
        kNumSteps, kSync, kDiv, kStepMs, kXfade, kMode, kKeySync,
        kWave1, kPitch1 = kWave1 + kSteps, kLevel1 = kPitch1 + kSteps,
        kWaveB = kLevel1 + kSteps, kOctB, kDetuneB, kMix, kVecAmount, kVecTime, kBendRange,
        kCutoff, kReso, kEnvAmount, kKeyTrack, kFAttack, kFDecay, kFSustain, kFRelease,
        kAttack, kDecay, kSustain, kRelease, kVelSens, kChorus,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Wave Relay";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'H', 'w', 'X');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    WaveRelay() : mpc::Plugin(makeInfo()), waves_(waveLibrary())
    {
        using namespace mpc;
        std::vector<std::string> names;
        for (const auto& w : waves_) names.push_back(w.name);

        addParam(floatParam("vol", "Volume", -40.0f, 6.0f, -6.0f, "dB", 1));
        // SEQ
        addParam(intParam  ("steps", "Steps", 1, kSteps, 4));
        addParam(boolParam ("sync",  "Sync", true));
        addParam(choiceParam("div",  "Step Div", divisionNames(), kDivisionEighth));
        addParam(logParam  ("stepms", "Step Time", 10.0f, 4000.0f, 250.0f, "ms", 0));
        addParam(floatParam("xfade", "Crossfade", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(choiceParam("mode", "Order", {"Forward", "PingPong", "Random", "Once"}, 0));
        addParam(boolParam ("keysync", "Key Sync", true));
        // per step
        static const char* dflt[kSteps] = {"Saw", "Vox Ah", "Glass", "Hollow", "FM Bright", "Choir", "Square", "Bell Tone"};
        for (int s = 0; s < kSteps; ++s)
        {
            int d = 0;
            for (size_t k = 0; k < names.size(); ++k) if (names[k] == dflt[s]) d = (int)k;
            const std::string id = "w" + std::to_string(s + 1), nm = "Wave " + std::to_string(s + 1);
            addParam(choiceParam(id.c_str(), nm.c_str(), names, d));
        }
        for (int s = 0; s < kSteps; ++s)
        {
            const std::string id = "p" + std::to_string(s + 1), nm = "Pitch " + std::to_string(s + 1);
            addParam(intParam(id.c_str(), nm.c_str(), -24, 24, 0, "st"));
        }
        for (int s = 0; s < kSteps; ++s)
        {
            const std::string id = "l" + std::to_string(s + 1), nm = "Level " + std::to_string(s + 1);
            addParam(floatParam(id.c_str(), nm.c_str(), 0.0f, 100.0f, 100.0f, "%", 0));
        }
        // LAYER B / VECTOR
        addParam(choiceParam("waveb", "Drone Wave", names, 0));
        addParam(intParam  ("octb",   "Drone Oct", -2, 2, -1));
        addParam(floatParam("detb",   "Drone Det", -50.0f, 50.0f, 6.0f, "ct", 0));
        addParam(floatParam("mix",    "Mix", 0.0f, 100.0f, 20.0f, "%", 0));
        addParam(floatParam("vecamt", "Vector", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam  ("vectime", "Vec Time", 10.0f, 10000.0f, 2000.0f, "ms", 0));
        addParam(intParam  ("bend",   "Bend Range", 0, 24, 2, "st"));
        // FILTER
        addParam(logParam  ("cutoff", "Cutoff", 20.0f, 20000.0f, 6000.0f, "Hz", 0));
        addParam(floatParam("reso",   "Resonance", 0.0f, 100.0f, 15.0f, "%", 0));
        addParam(floatParam("envamt", "Env Amount", -100.0f, 100.0f, 20.0f, "%", 0));
        addParam(floatParam("keytrk", "Key Track", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(logParam  ("fatk",   "Filt Attack", 1.0f, 8000.0f, 300.0f, "ms", 0));
        addParam(logParam  ("fdec",   "Filt Decay", 1.0f, 8000.0f, 1500.0f, "ms", 0));
        addParam(floatParam("fsus",   "Filt Sustain", 0.0f, 100.0f, 40.0f, "%", 0));
        addParam(logParam  ("frel",   "Filt Release", 1.0f, 8000.0f, 1500.0f, "ms", 0));
        // AMP
        addParam(logParam  ("atk",    "Attack", 1.0f, 8000.0f, 150.0f, "ms", 0));
        addParam(logParam  ("dec",    "Decay", 1.0f, 8000.0f, 1500.0f, "ms", 0));
        addParam(floatParam("sus",    "Sustain", 0.0f, 100.0f, 80.0f, "%", 0));
        addParam(logParam  ("rel",    "Release", 1.0f, 8000.0f, 1200.0f, "ms", 0));
        addParam(floatParam("velsens", "Velocity", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam("chorus", "Chorus", 0.0f, 100.0f, 40.0f, "%", 0));
        if (numParams() != kNumParams) mpc::log("WaveRelay: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        makePrograms(names);
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        for (auto& v : voices_) { v.filter.setSampleRate(sampleRate); v.amp.setSampleRate(sampleRate); v.fenv.setSampleRate(sampleRate); }
        chorus_.prepare((int)(0.05 * sampleRate) + 4);
        volume_.setTime(15.0f, sampleRate);
        volume_.snap(dbToGain(get(kVolume)));
        controllers_.prepare(sampleRate, 10.0f);
        const float w = 2.0f * 3.14159265f * 0.37f / (float)sampleRate;   // chorus LFO 0.37 Hz
        rotC_ = std::cos(w); rotS_ = std::sin(w);
    }

    void reset() override
    {
        for (auto& v : voices_) { v.amp.reset(); v.fenv.reset(); v.filter.reset(); v.note = -1; }
        chorus_.reset();
        lfoX_ = 1.0f; lfoY_ = 0.0f;
        gpos_ = 0.0; bend_ = 0.0f; sustain_ = false; age_ = 0;
        volume_.snap(dbToGain(get(kVolume)));
        controllers_.reset();
    }

    void process(mpc::ProcessContext& ctx) override
    {
        readSettings(ctx);
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        for (int i = 0; i < ctx.numFrames; ++i) mono_[i] = 0.0f;

        int pos = 0;
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            if (ev.sampleOffset > pos) { render(pos, ev.sampleOffset); pos = ev.sampleOffset; }
            handleMidi(ev);
        }
        render(pos, ctx.numFrames);

        // stereo chorus: one delay line, two modulated taps in quadrature
        const float mix = s_.chorus;
        const float base = 0.012f * (float)sr_, depth = 0.004f * (float)sr_;
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            const float x = mono_[i];
            chorus_.write(x);
            float l = x, r = x;
            if (mix > 0.0f)
            {
                const float nx = lfoX_ * rotC_ - lfoY_ * rotS_, ny = lfoX_ * rotS_ + lfoY_ * rotC_;
                lfoX_ = nx; lfoY_ = ny;
                const float tl = chorus_.readLinear(base + depth * lfoX_), tr = chorus_.readLinear(base + depth * lfoY_);
                l = x * (1.0f - 0.5f * mix) + tl * mix * 0.8f;
                r = x * (1.0f - 0.5f * mix) + tr * mix * 0.8f;
            }
            const float g = volume_.process();
            L[i] = sanitize(l * g);
            if (R) R[i] = sanitize(r * g);
        }
        if ((++renorm_ & 255) == 0) { const float m = 1.0f / std::sqrt(lfoX_ * lfoX_ + lfoY_ * lfoY_); lfoX_ *= m; lfoY_ *= m; }
    }

private:
    static constexpr int kVoices = 12;
    static constexpr int kCtl = 16;
    static constexpr int kMaxBlock = 4096;

    struct Voice
    {
        int note = -1;
        bool held = false, sustained = false;
        unsigned age = 0;
        float velGain = 1.0f;
        double pos = 0.0;             // position in steps (Key Sync)
        long lastK = -1;
        uint32_t seed = 1;
        float phA = 0, phN = 0, phB = 0;     // oscillator phases (cycles)
        float incA = 0, incN = 0, incB = 0;
        int lvA = 0, lvN = 0, lvB = 0;       // mip levels
        int stepA = 0, stepN = 0;
        float vecT = 0.0f;
        int ctl = 0;
        SVF filter;
        ADSR amp, fenv;
        bool active() const { return amp.isActive(); }
    };

    struct Settings
    {
        int steps = 4, mode = 0, waveB = 0;
        bool sync = true, keySync = true;
        double stepsPerSample = 0.0;
        float xfade = 0.5f, tune = 0, detB = 0, octB = 0, mix = 0.2f, vecAmt = 0, vecTime = 2.0f;
        float cutoff = 6000, reso = 0.15f, envAmt = 0.2f, keyTrack = 0.5f, velSens = 0.5f, chorus = 0.4f;
        int wave[kSteps] = {0}; float pitch[kSteps] = {0}; float level[kSteps] = {1, 1, 1, 1, 1, 1, 1, 1};
    } s_;

    void readSettings(mpc::ProcessContext& ctx)
    {
        const int nw = (int)waves_.size();
        s_.steps = mpc::clamp(getInt(kNumSteps), 1, kSteps);
        s_.sync = getBool(kSync);
        s_.mode = getInt(kMode);
        s_.keySync = getBool(kKeySync);
        s_.xfade = get(kXfade) * 0.01f;
        const double bpm = ctx.transport.tempoValid && ctx.transport.bpm > 1.0 ? ctx.transport.bpm : 120.0;
        const double stepSec = s_.sync ? divisionBeats(getInt(kDiv)) * 60.0 / bpm : get(kStepMs) * 0.001;
        s_.stepsPerSample = 1.0 / std::max(1.0, stepSec * sr_);
        for (int k = 0; k < kSteps; ++k)
        {
            s_.wave[k] = mpc::clamp(getInt(kWave1 + k), 0, nw - 1);
            s_.pitch[k] = (float)getInt(kPitch1 + k);
            s_.level[k] = get(kLevel1 + k) * 0.01f;
        }
        s_.waveB = mpc::clamp(getInt(kWaveB), 0, nw - 1);
        s_.octB = (float)getInt(kOctB) * 12.0f;
        s_.detB = get(kDetuneB) * 0.01f;
        s_.mix = get(kMix) * 0.01f;
        s_.vecAmt = get(kVecAmount) * 0.01f;
        s_.vecTime = get(kVecTime) * 0.001f;
        bendRange_ = (float)getInt(kBendRange);
        s_.cutoff = get(kCutoff); s_.reso = get(kReso) * 0.01f; s_.envAmt = get(kEnvAmount) * 0.01f; s_.keyTrack = get(kKeyTrack) * 0.01f;
        s_.velSens = get(kVelSens) * 0.01f;
        s_.chorus = get(kChorus) * 0.01f;
        const float fa = get(kFAttack), fd = get(kFDecay), fs = get(kFSustain) * 0.01f, fr = get(kFRelease);
        const float aa = get(kAttack), ad = get(kDecay), as = get(kSustain) * 0.01f, ar = get(kRelease);
        for (auto& v : voices_) { v.amp.set(aa, ad, as, ar); v.fenv.set(fa, fd, fs, fr); }
        volume_.setTarget(dbToGain(get(kVolume)));
        // shared clock: locked to the song while it plays, free otherwise
        if (s_.sync && ctx.transport.playing && ctx.transport.ppqValid)
            gpos_ = ctx.transport.ppqPos / std::max(1.0e-3, (double)divisionBeats(getInt(kDiv)));
    }

    // Which step plays at sequence position k (k counts steps since the start).
    int stepAt(long k, uint32_t seed) const
    {
        const int n = s_.steps;
        if (k < 0) k = 0;
        switch (s_.mode)
        {
            case 1: { if (n == 1) return 0; const long period = 2L * (n - 1); const long m = k % period; return (int)(m < n ? m : period - m); }
            case 2: { uint32_t h = (uint32_t)k * 2654435761u ^ seed; h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; return (int)(h % (uint32_t)n); }
            case 3: return (int)std::min<long>(k, n - 1);
            default: return (int)(k % n);
        }
    }

    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn())
        {
            Voice& v = allocate();
            v.note = ev.note(); v.held = true; v.sustained = false; v.age = ++age_;
            v.velGain = lerp(1.0f, ev.velocity() / 127.0f, s_.velSens);
            v.pos = 0.0; v.lastK = -1; v.seed = 0x9E3779B9u * (uint32_t)(age_ + 1);
            v.phA = v.phN = 0.0f; v.phB = 0.25f;
            v.vecT = 0.0f; v.ctl = 0;
            v.filter.reset();
            v.amp.reset(); v.fenv.reset(); v.amp.noteOn(); v.fenv.noteOn();
        }
        else if (ev.isNoteOff())
        {
            for (auto& v : voices_)
                if (v.held && v.note == ev.note() && v.active())
                {
                    v.held = false;
                    if (sustain_) v.sustained = true; else { v.amp.noteOff(); v.fenv.noteOff(); }
                }
        }
        else if (ev.isAllNotesOff()) { for (auto& v : voices_) { v.held = v.sustained = false; v.amp.noteOff(); v.fenv.noteOff(); } }
        else if (ev.isController() && ev.controller() == cc::Sustain)
        {
            sustain_ = ev.controllerValue() >= 64;
            if (!sustain_) for (auto& v : voices_) if (v.sustained) { v.sustained = false; v.amp.noteOff(); v.fenv.noteOff(); }
        }
        else if (ev.isPitchBend()) bend_ = (float)ev.pitchBend() / 8192.0f;
        else controllers_.handle(ev);
    }

    Voice& allocate()
    {
        for (auto& v : voices_) if (!v.active()) return v;
        Voice* best = &voices_[0];
        for (auto& v : voices_)
        {
            const bool rel = !v.held && !v.sustained, bestRel = !best->held && !best->sustained;
            if ((rel && !bestRel) || (rel == bestRel && v.age < best->age)) best = &v;
        }
        return *best;
    }

    void render(int from, int to)
    {
        if (to <= from) return;
        const int n = to - from;
        // shared clock positions for this span
        for (int i = 0; i < n; ++i) { gbuf_[i] = gpos_; gpos_ += s_.stepsPerSample; }
        const float bendSemis = bend_ * bendRange_;
        const float dt = 1.0f / (float)sr_;
        for (auto& v : voices_)
        {
            if (!v.active()) continue;
            const float noteHz = 440.0f * std::pow(2.0f, ((float)v.note - 69.0f) / 12.0f) / (float)sr_;
            for (int i = 0; i < n; ++i)
            {
                // where in the sequence are we?
                const double p = s_.keySync ? v.pos : gbuf_[i];
                const long k = (long)std::floor(p);
                const float frac = (float)(p - (double)k);
                const bool changed = k != v.lastK;
                if (changed)
                {
                    const bool contiguous = v.lastK >= 0 && k == v.lastK + 1;
                    v.stepA = stepAt(k, v.seed);
                    v.stepN = stepAt(k + 1, v.seed);
                    v.phA = contiguous ? v.phN : v.phA;   // the incoming wave keeps the phase it already had
                    v.phN = v.phA;
                    v.lastK = k;
                }
                if (changed || v.ctl == 0)
                {
                    const float semisA = s_.pitch[v.stepA] + s_.tune + bendSemis;
                    const float semisN = s_.pitch[v.stepN] + s_.tune + bendSemis;
                    v.incA = std::min(0.49f, noteHz * semitonesToRatio(semisA));
                    v.incN = std::min(0.49f, noteHz * semitonesToRatio(semisN));
                    v.incB = std::min(0.49f, noteHz * semitonesToRatio(s_.octB + s_.detB + s_.tune + bendSemis));
                    v.lvA = wr::levelFor(v.incA); v.lvN = wr::levelFor(v.incN); v.lvB = wr::levelFor(v.incB);
                    if (v.ctl == 0)
                    {
                        const float oct = s_.envAmt * v.fenv.level() * 7.0f + s_.keyTrack * ((float)v.note - 60.0f) / 12.0f;
                        v.filter.set(mpc::clamp(s_.cutoff * std::pow(2.0f, mpc::clamp(oct, -10.0f, 10.0f)), 20.0f, 20000.0f), s_.reso);
                    }
                }
                v.ctl = (v.ctl + 1) % kCtl;

                // crossfade over the last part of each step
                float x = 0.0f;
                if (s_.xfade > 0.0f && v.stepN != v.stepA && frac > 1.0f - s_.xfade) x = (frac - (1.0f - s_.xfade)) / s_.xfade;
                float seq = read(waves_[(size_t)s_.wave[v.stepA]], v.lvA, v.phA) * s_.level[v.stepA];
                if (x > 0.0f) seq += x * (read(waves_[(size_t)s_.wave[v.stepN]], v.lvN, v.phN) * s_.level[v.stepN] - seq);
                const float drone = read(waves_[(size_t)s_.waveB], v.lvB, v.phB);
                const float m = mpc::clamp(s_.mix + s_.vecAmt * std::min(1.0f, v.vecT / std::max(0.01f, s_.vecTime)), 0.0f, 1.0f);
                float y = seq + m * (drone - seq);

                v.fenv.process();
                y = v.filter.process(y, 0) * v.amp.process() * v.velGain;
                mono_[from + i] += y * 0.35f;

                v.phA += v.incA; if (v.phA >= 1.0f) v.phA -= 1.0f;
                v.phN += v.incN; if (v.phN >= 1.0f) v.phN -= 1.0f;
                v.phB += v.incB; if (v.phB >= 1.0f) v.phB -= 1.0f;
                v.pos += s_.stepsPerSample;
                v.vecT += dt;
                if (!v.active()) break;
            }
        }
    }

    static inline float read(const wr::Wave& w, int level, float phase)
    {
        const float* t = w.level(level);
        const float p = phase * (float)wr::kTable;
        const int i = (int)p;
        const float f = p - (float)i;
        return t[i] + f * (t[i + 1] - t[i]);
    }

    void makePrograms(const std::vector<std::string>& names)
    {
        auto idx = [&](const char* n) { for (size_t k = 0; k < names.size(); ++k) if (names[k] == n) return (float)k; return 0.0f; };
        auto program = [&](const char* name, std::initializer_list<const char*> waves, std::initializer_list<float> pitches,
                           std::initializer_list<std::pair<int, float>> o)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            int s = 0; for (const char* w : waves) v[(size_t)(kWave1 + s++)] = idx(w);
            s = 0; for (float p : pitches) v[(size_t)(kPitch1 + s++)] = p;
            for (const auto& p : o) v[(size_t)p.first] = p.second;
            addProgram(name, std::move(v));
        };
        program("Relay Pad", {"Saw", "Vox Ah", "Glass", "Hollow", "FM Bright", "Choir", "Square", "Bell Tone"}, {}, {});
        program("Vox Morph", {"Vox Ah", "Vox Oo", "Vox Ee", "Choir"}, {}, {{kSync, 0}, {kStepMs, 700}, {kXfade, 100}, {kMode, 1}, {kWaveB, idx("Choir")}, {kMix, 35}, {kChorus, 60}});
        program("Glass Steps", {"Glass", "Bell Tone", "Glass", "FM Metal", "Glass", "Bell Tone", "Sine", "FM Bright"}, {0, 12, 7, 19, 0, 24, 12, 5},
                {{kNumSteps, 8}, {kDiv, kDivisionSixteenth}, {kXfade, 10}, {kAttack, 5}, {kDecay, 900}, {kSustain, 30}, {kRelease, 600}, {kKeySync, 0}, {kMix, 0}});
        program("Pulse Arp", {"Pulse 25", "Saw", "Pulse 10", "Square"}, {0, 7, 12, 3},
                {{kDiv, kDivisionSixteenth}, {kXfade, 0}, {kAttack, 2}, {kSustain, 60}, {kRelease, 200}, {kCutoff, 2500}, {kReso, 35}, {kEnvAmount, 40}, {kFAttack, 1}, {kFDecay, 250}, {kMix, 0}, {kChorus, 20}});
        program("Evolving Drone", {"Hollow", "Choir", "Comb Saw", "Soft Organ", "Odd Saw", "Vox Oo"}, {},
                {{kNumSteps, 6}, {kSync, 0}, {kStepMs, 1800}, {kXfade, 100}, {kMode, 2}, {kWaveB, idx("Sine")}, {kOctB, -1}, {kMix, 30}, {kVecAmount, 40}, {kVecTime, 6000}, {kAttack, 1200}, {kRelease, 3000}, {kChorus, 70}});
        program("Digital Bells", {"Bell Tone", "Digital", "Metal", "Glass"}, {0, 12, 0, 19},
                {{kDiv, kDivisionEighth}, {kXfade, 20}, {kAttack, 3}, {kDecay, 1600}, {kSustain, 0}, {kRelease, 1200}, {kMix, 15}, {kWaveB, idx("Sine")}, {kOctB, 0}});
        program("Sync Bass", {"Sync 2.5", "Saw", "Sync 4.3", "Fold"}, {0, 0, 12, 0},
                {{kDiv, kDivisionEighth}, {kXfade, 15}, {kAttack, 1}, {kDecay, 400}, {kSustain, 50}, {kRelease, 120}, {kCutoff, 1200}, {kReso, 30}, {kEnvAmount, 60}, {kFAttack, 1}, {kFDecay, 300}, {kFSustain, 10}, {kWaveB, idx("Sine")}, {kOctB, -1}, {kMix, 35}, {kChorus, 0}});
        program("Choir Sweep", {"Choir", "Vox Ah"}, {},
                {{kNumSteps, 2}, {kSync, 0}, {kStepMs, 3000}, {kXfade, 100}, {kMode, 1}, {kVecAmount, -20}, {kVecTime, 4000}, {kWaveB, idx("Vox Oo")}, {kMix, 40}, {kAttack, 900}, {kRelease, 2500}, {kChorus, 80}, {kCutoff, 4000}});
    }

    const std::vector<wr::Wave>& waves_;
    double sr_ = 44100.0;
    Voice voices_[kVoices];
    unsigned age_ = 0;
    bool sustain_ = false;
    float bend_ = 0.0f, bendRange_ = 2.0f;
    double gpos_ = 0.0;
    double gbuf_[kMaxBlock];
    float mono_[kMaxBlock];
    DelayLine chorus_;
    float lfoX_ = 1.0f, lfoY_ = 0.0f, rotC_ = 1.0f, rotS_ = 0.0f;
    unsigned renorm_ = 0;
    Smoother volume_;
    MidiControllers controllers_;
};

MPC_PLUGIN_ENTRY(WaveRelay)
