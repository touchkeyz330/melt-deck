// MeltRack.cpp - a 4-tone rack synthesizer for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// An original design in the spirit of the classic 90s rack synths: every patch stacks up to
// four TONES, and each tone is a complete little synth of its own:
//
//   WAVE (band-limited table, noise or your own sample)
//     -> pitch: coarse / fine / pitch envelope / LFO vibrato / glide / bend
//     -> TVF filter (LPF, BPF, HPF, Peak) with its own ADSR, key follow, LFO
//     -> TVA amp with its own ADSR, velocity, LFO tremolo, pan + auto-pan
//   Tones 1&2 and 3&4 can be ring-modulated (Structure). Each tone has a key range, so a patch
//   can be a split.
// Patch: Poly or Mono (legato) with portamento, mod wheel vibrato, sustain pedal.
// Effects: EFX (Overdrive, Phaser, Delay, Enhancer) -> stereo Chorus -> Reverb.
//
// No ROM samples from any other instrument: the waves are generated from maths, and you can
// add your own WAVs in Content/Waves (single cycles become waves, longer files become
// samples; root note and loop are read from the file's sampler chunk, default root C3 = 60).

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/svf.h"
#include "dsp/adsr.h"
#include "dsp/smoother.h"
#include "dsp/delay_line.h"
#include "dsp/midi_cc.h"
#include "dsp/wav.h"
#include "waves.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <dirent.h>
#endif

using namespace mpc::dsp;

static constexpr int kTones = 4;
static constexpr float kPiF = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Sound sources (built once, shared by every instance)
// ---------------------------------------------------------------------------
struct Source
{
    enum Kind { Table, Noise, Sample } kind = Table;
    std::string name;
    wr::Wave wave;                 // Table
    std::vector<float> data;       // Sample (mono)
    double rate = 44100.0;
    int root = 60;
    bool looped = false;
    double loopStart = 0, loopEnd = 0;
};

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

// Reads root note and the first loop from a WAV 'smpl' chunk, if there is one.
static void readSamplerChunk(const std::vector<uint8_t>& d, int& root, bool& looped, double& ls, double& le)
{
    auto u32 = [&](size_t p) { return (uint32_t)d[p] | ((uint32_t)d[p + 1] << 8) | ((uint32_t)d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24); };
    if (d.size() < 12) return;
    for (size_t p = 12; p + 8 <= d.size();)
    {
        const uint32_t len = u32(p + 4);
        const size_t body = p + 8;
        if (body + len > d.size() || len > d.size()) break;
        if (std::memcmp(d.data() + p, "smpl", 4) == 0 && len >= 36)
        {
            const uint32_t unity = u32(body + 12);
            if (unity <= 127) root = (int)unity;
            const uint32_t loops = u32(body + 28);
            if (loops >= 1 && len >= 36 + 24)
            {
                const uint32_t s = u32(body + 36 + 8), e = u32(body + 36 + 12);
                if (e > s + 8) { looped = true; ls = s; le = (double)e + 1.0; }
            }
            return;
        }
        p = body + len + (len & 1u);
    }
}

static const std::vector<Source>& sourceLibrary()
{
    static std::vector<Source> lib;
    static std::once_flag once;
    std::call_once(once, []
    {
        for (auto& w : wr::builtinWaves()) { Source s; s.kind = Source::Table; s.name = w.name; s.wave = std::move(w); lib.push_back(std::move(s)); }
        { Source s; s.kind = Source::Noise; s.name = "Noise"; lib.push_back(std::move(s)); }

        const std::string dir = mpc::contentPath("Waves");
        size_t budget = 24u * 1024u * 1024u;   // at most 24M sample frames (~96 MB) of user samples
        int added = 0;
        for (const auto& f : listWavs(dir))
        {
            if (added >= 96) break;
            std::vector<uint8_t> bytes;
            {
                std::ifstream in(dir + "/" + f, std::ios::binary);
                if (!in) continue;
                in.seekg(0, std::ios::end);
                const std::streamoff sz = in.tellg();
                if (sz <= 44 || sz > (std::streamoff)512 * 1024 * 1024) continue;
                bytes.resize((size_t)sz);
                in.seekg(0);
                in.read((char*)bytes.data(), sz);
                if (!in) continue;
            }
            dsp::WavData w;
            if (!dsp::parseWav(bytes, w) || w.numFrames() < 16) { mpc::log("MeltRack: could not read %s", f.c_str()); continue; }
            const size_t n = w.numFrames();
            std::vector<float> mono(n);
            for (size_t i = 0; i < n; ++i) { float v = 0; for (int c = 0; c < w.numChannels(); ++c) v += w.channels[(size_t)c][i]; mono[i] = v / (float)w.numChannels(); }
            std::string base = f.substr(0, f.size() - 4);
            if (base.size() > 12) base = base.substr(0, 12);

            Source s;
            s.name = base;
            readSamplerChunk(bytes, s.root, s.looped, s.loopStart, s.loopEnd);
            if (n <= 4096 && !s.looped)
            {
                s.kind = Source::Table;
                s.wave = wr::makeWave(base, wr::toCycle(mono.data(), n));
            }
            else
            {
                if (n > budget) { mpc::log("MeltRack: %s skipped (sample memory full)", f.c_str()); continue; }
                budget -= n;
                s.kind = Source::Sample;
                s.rate = w.sampleRate;
                if (s.loopEnd > (double)n) s.loopEnd = (double)n;
                if (s.looped && s.loopEnd - s.loopStart < 8) s.looped = false;
                mono.push_back(mono.back()); mono.push_back(mono.back()); mono.push_back(mono.back());   // guard points
                s.data = std::move(mono);
            }
            lib.push_back(std::move(s));
            ++added;
        }
    });
    return lib;
}

// ---------------------------------------------------------------------------
// Small effects
// ---------------------------------------------------------------------------
// 8-line feedback delay network reverb.
class Reverb
{
public:
    void prepare(double sr)
    {
        sr_ = sr;
        static const float ms[8] = {29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.7f, 73.1f};
        for (int i = 0; i < 8; ++i) { len_[i] = (int)(ms[i] * 0.001 * sr); line_[i].assign((size_t)len_[i], 0.0f); }
        static const float ap[2] = {4.7f, 11.3f};
        for (int i = 0; i < 2; ++i) { apLen_[i] = (int)(ap[i] * 0.001 * sr); apBuf_[i].assign((size_t)apLen_[i], 0.0f); }
        reset();
    }
    void reset()
    {
        for (int i = 0; i < 8; ++i) { std::fill(line_[i].begin(), line_[i].end(), 0.0f); pos_[i] = 0; lp_[i] = 0; }
        for (int i = 0; i < 2; ++i) { std::fill(apBuf_[i].begin(), apBuf_[i].end(), 0.0f); apPos_[i] = 0; }
    }
    void set(float seconds, float damp)
    {
        for (int i = 0; i < 8; ++i) g_[i] = std::pow(10.0f, -3.0f * (float)len_[i] / (std::max(0.1f, seconds) * (float)sr_));
        damp_ = mpc::clamp(damp, 0.0f, 0.95f);
    }
    void process(float inL, float inR, float& outL, float& outR)
    {
        float x = (inL + inR) * 0.5f;
        for (int a = 0; a < 2; ++a)   // input diffusion
        {
            float& d = apBuf_[a][(size_t)apPos_[a]];
            const float y = d - 0.6f * x;
            d = x + 0.6f * y;
            x = y;
            if (++apPos_[a] >= apLen_[a]) apPos_[a] = 0;
        }
        float o[8], sum = 0.0f;
        for (int i = 0; i < 8; ++i) { o[i] = line_[i][(size_t)pos_[i]]; sum += o[i]; }
        const float hh = sum * 0.25f;   // Householder: o - 2/N * sum
        outL = (o[0] - o[2] + o[4] - o[6]) * 0.5f;
        outR = (o[1] - o[3] + o[5] - o[7]) * 0.5f;
        for (int i = 0; i < 8; ++i)
        {
            float fb = (o[i] - hh) * g_[i];
            lp_[i] = fb + damp_ * (lp_[i] - fb);
            line_[i][(size_t)pos_[i]] = lp_[i] + x;
            if (++pos_[i] >= len_[i]) pos_[i] = 0;
        }
    }
private:
    double sr_ = 44100.0;
    std::vector<float> line_[8], apBuf_[2];
    int len_[8] = {}, pos_[8] = {}, apLen_[2] = {}, apPos_[2] = {};
    float g_[8] = {}, lp_[8] = {}, damp_ = 0.3f;
};

// ---------------------------------------------------------------------------
// The plugin
// ---------------------------------------------------------------------------
class MeltRack : public mpc::Plugin
{
public:
    // per-tone parameter offsets
    enum ToneParam
    {
        tOn, tWave, tLevel, tPan, tCoarse, tFine, tKeyLo, tKeyHi, tVelSens,
        tFType, tCutoff, tReso, tKeyF, tFEnv, tFA, tFD, tFS, tFR,
        tA, tD, tS, tR,
        tLfoRate, tLfoShape, tLfoFade, tLfoPitch, tLfoFilter, tLfoAmp, tLfoPan,
        tPEnv, tPTime,
        kToneStride
    };
    enum Common
    {
        kVolume = 0, kMode, kPorta, kBend, kStruct12, kStruct34, kWheel,
        kEfxType, kEfxAmt, kEfxRate, kEfxMix, kChoRate, kChoDepth, kChoLevel, kRevTime, kRevLevel, kRevDamp,
        kTone1,
        kNumParams = kTone1 + kTones * kToneStride
    };
    static int tp(int tone, int p) { return kTone1 + tone * kToneStride + p; }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt Rack";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'u', 'y', 'o');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    MeltRack() : mpc::Plugin(makeInfo()), src_(sourceLibrary())
    {
        using namespace mpc;
        for (const auto& s : src_) names_.push_back(s.name);

        addParam(floatParam("vol", "Volume", -40.0f, 6.0f, -4.0f, "dB", 1));
        addParam(choiceParam("mode", "Key Mode", {"Poly", "Mono"}, 0));
        addParam(floatParam("porta", "Portamento", 0.0f, 2000.0f, 0.0f, "ms", 0, 0.3f));
        addParam(intParam("bend", "Bend Range", 0, 24, 2, "st"));
        addParam(choiceParam("s12", "Struct 1-2", {"Mix", "Ring"}, 0));
        addParam(choiceParam("s34", "Struct 3-4", {"Mix", "Ring"}, 0));
        addParam(floatParam("wheel", "Wheel Vib", 0.0f, 100.0f, 40.0f, "ct", 0));
        addParam(choiceParam("efx", "EFX Type", {"Off", "Overdrive", "Phaser", "Delay", "Enhancer"}, 0));
        addParam(floatParam("efxamt", "EFX Amount", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam("efxrate", "EFX Rate", 0.0f, 100.0f, 40.0f, "%", 0));
        addParam(floatParam("efxmix", "EFX Mix", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(logParam("chorate", "Chorus Rate", 0.05f, 8.0f, 0.5f, "Hz", 2));
        addParam(floatParam("chodep", "Chorus Depth", 0.0f, 100.0f, 40.0f, "%", 0));
        addParam(floatParam("cholev", "Chorus Level", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(logParam("revtime", "Reverb Time", 0.2f, 12.0f, 2.2f, "s", 1));
        addParam(floatParam("revlev", "Reverb Level", 0.0f, 100.0f, 25.0f, "%", 0));
        addParam(floatParam("revdamp", "Reverb Damp", 0.0f, 100.0f, 40.0f, "%", 0));

        static const char* lfoShapes[] = {"Tri", "Sine", "Saw", "Square", "Random"};
        for (int t = 0; t < kTones; ++t)
        {
            const std::string n = std::to_string(t + 1);
            auto id = [&](const char* s) { return std::string("t") + n + s; };
            auto nm = [&](const char* s) { return std::string("T") + n + " " + s; };
            addParam(boolParam(id("on").c_str(), nm("On").c_str(), t == 0));
            addParam(choiceParam(id("wave").c_str(), nm("Wave").c_str(), names_, 2));
            addParam(floatParam(id("lev").c_str(), nm("Level").c_str(), 0.0f, 100.0f, 80.0f, "%", 0));
            addParam(floatParam(id("pan").c_str(), nm("Pan").c_str(), -100.0f, 100.0f, 0.0f, "", 0));
            addParam(intParam(id("crs").c_str(), nm("Coarse").c_str(), -48, 48, 0, "st"));
            addParam(floatParam(id("fine").c_str(), nm("Fine").c_str(), -50.0f, 50.0f, 0.0f, "ct", 0));
            addParam(intParam(id("klo").c_str(), nm("Key Lo").c_str(), 0, 127, 0));
            addParam(intParam(id("khi").c_str(), nm("Key Hi").c_str(), 0, 127, 127));
            addParam(floatParam(id("vel").c_str(), nm("Velocity").c_str(), 0.0f, 100.0f, 50.0f, "%", 0));
            addParam(choiceParam(id("ftype").c_str(), nm("Filter").c_str(), {"Off", "LPF", "BPF", "HPF", "Peak"}, 1));
            addParam(logParam(id("cut").c_str(), nm("Cutoff").c_str(), 20.0f, 20000.0f, 8000.0f, "Hz", 0));
            addParam(floatParam(id("res").c_str(), nm("Reso").c_str(), 0.0f, 100.0f, 10.0f, "%", 0));
            addParam(floatParam(id("kf").c_str(), nm("Key Follow").c_str(), -100.0f, 200.0f, 50.0f, "%", 0));
            addParam(floatParam(id("fenv").c_str(), nm("F Env").c_str(), -100.0f, 100.0f, 0.0f, "%", 0));
            addParam(logParam(id("fa").c_str(), nm("F Attack").c_str(), 0.5f, 10000.0f, 2.0f, "ms", 0));
            addParam(logParam(id("fd").c_str(), nm("F Decay").c_str(), 1.0f, 10000.0f, 600.0f, "ms", 0));
            addParam(floatParam(id("fs").c_str(), nm("F Sustain").c_str(), 0.0f, 100.0f, 50.0f, "%", 0));
            addParam(logParam(id("fr").c_str(), nm("F Release").c_str(), 1.0f, 10000.0f, 400.0f, "ms", 0));
            addParam(logParam(id("a").c_str(), nm("Attack").c_str(), 0.5f, 10000.0f, 2.0f, "ms", 0));
            addParam(logParam(id("d").c_str(), nm("Decay").c_str(), 1.0f, 10000.0f, 800.0f, "ms", 0));
            addParam(floatParam(id("s").c_str(), nm("Sustain").c_str(), 0.0f, 100.0f, 80.0f, "%", 0));
            addParam(logParam(id("r").c_str(), nm("Release").c_str(), 1.0f, 10000.0f, 400.0f, "ms", 0));
            addParam(logParam(id("lrate").c_str(), nm("LFO Rate").c_str(), 0.05f, 20.0f, 5.0f, "Hz", 2));
            addParam(choiceParam(id("lshape").c_str(), nm("LFO Shape").c_str(), {lfoShapes, lfoShapes + 5}, 0));
            addParam(floatParam(id("lfade").c_str(), nm("LFO Fade").c_str(), 0.0f, 5000.0f, 0.0f, "ms", 0, 0.3f));
            addParam(floatParam(id("lpit").c_str(), nm("LFO>Pitch").c_str(), 0.0f, 100.0f, 0.0f, "ct", 0));
            addParam(floatParam(id("lflt").c_str(), nm("LFO>Filter").c_str(), 0.0f, 100.0f, 0.0f, "%", 0));
            addParam(floatParam(id("lamp").c_str(), nm("LFO>Amp").c_str(), 0.0f, 100.0f, 0.0f, "%", 0));
            addParam(floatParam(id("lpan").c_str(), nm("LFO>Pan").c_str(), 0.0f, 100.0f, 0.0f, "%", 0));
            addParam(floatParam(id("penv").c_str(), nm("P Env").c_str(), -24.0f, 24.0f, 0.0f, "st", 1));
            addParam(logParam(id("ptime").c_str(), nm("P Env Time").c_str(), 1.0f, 4000.0f, 80.0f, "ms", 0));
        }
        if (numParams() != kNumParams) mpc::log("MeltRack: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);
        makePrograms();
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        for (auto& v : voices_)
            for (auto& t : v.t) { t.filter.setSampleRate(sampleRate); t.amp.setSampleRate(sampleRate); t.fenv.setSampleRate(sampleRate); }
        choL_.prepare((int)(0.04 * sampleRate) + 4);
        choR_.prepare((int)(0.04 * sampleRate) + 4);
        dlyL_.prepare((int)(1.1 * sampleRate) + 4);
        dlyR_.prepare((int)(1.1 * sampleRate) + 4);
        reverb_.prepare(sampleRate);
        volume_.setTime(15.0f, sampleRate);
        volume_.snap(dbToGain(get(kVolume)));
        controllers_.prepare(sampleRate / kCtl, 10.0f);
        reset();
    }

    void reset() override
    {
        for (auto& v : voices_) killVoice(v);
        choL_.reset(); choR_.reset(); dlyL_.reset(); dlyR_.reset(); reverb_.reset();
        choPh_ = efxPh_ = 0.0f;
        for (auto& a : phz_) a = 0.0f;
        phzFbL_ = phzFbR_ = enhL_ = enhR_ = 0.0f;
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
        effects(nf);

        for (int i = 0; i < nf; ++i)
        {
            const float g = volume_.process();
            L[i] = sanitize(softClip(bufL_[i] * g));
            if (R) R[i] = sanitize(softClip(bufR_[i] * g));
        }
        for (int i = nf; i < ctx.numFrames; ++i) { L[i] = 0.0f; if (R) R[i] = 0.0f; }
    }

private:
    static constexpr int kVoices = 16;
    static constexpr int kCtl = 16;
    static constexpr int kMaxBlock = 4096;

    static float softClip(float x)   // transparent below about -3 dBFS
    {
        const float a = std::fabs(x);
        if (a < 0.7f) return x;
        const float y = 0.7f + 0.3f * fastTanh((a - 0.7f) / 0.3f);
        return x < 0 ? -y : y;
    }

    struct ToneSettings
    {
        bool on = false;
        int src = 0, ftype = 1, lshape = 0;
        float level = 0.8f, pan = 0, coarse = 0, fine = 0, velSens = 0.5f;
        int keyLo = 0, keyHi = 127;
        float cutoff = 8000, reso = 0.1f, keyF = 0.5f, fenv = 0;
        float lrate = 5, lfade = 0, lpit = 0, lflt = 0, lamp = 0, lpan = 0;
        float penv = 0, ptime = 80;
        float env[8] = {};
    };

    struct Tone
    {
        bool on = false, done = true;
        float gain = 0.0f;          // level x velocity
        float ph = 0.0f, inc = 0.0f;
        int lv = 0;
        double spos = 0.0, sinc = 0.0;
        float lfoPh = 0.0f, lfoVal = 0.0f, randVal = 0.0f;
        float gl = 0, gr = 0, tgl = 0, tgr = 0;   // pan/tremolo gains (ramped)
        uint32_t rng = 1;
        float lastOsc = 0.0f;
        SVF filter;
        ADSR amp, fenv;
    };

    struct Voice
    {
        int note = -1;
        bool held = false, sustained = false;
        unsigned age = 0;
        float vel = 1.0f;
        float pitch = 60.0f, pitchTarget = 60.0f;   // glide (in notes)
        float time = 0.0f;                          // seconds since note on
        Tone t[kTones];
        bool active() const { for (const auto& x : t) if (x.on && !x.done) return true; return false; }
    };

    struct Settings
    {
        int mode = 0, efx = 0;
        bool ring12 = false, ring34 = false;
        float porta = 0, bend = 2, wheel = 0.4f;
        float efxAmt = 0.5f, efxRate = 0.4f, efxMix = 0.5f, choRate = 0.5f, choDepth = 0.4f, choLevel = 0.3f, revLevel = 0.25f;
        ToneSettings tone[kTones];
    } s_;

    void readSettings()
    {
        const int ns = (int)src_.size();
        s_.mode = getInt(kMode);
        s_.porta = get(kPorta);
        s_.bend = (float)getInt(kBend);
        s_.ring12 = getInt(kStruct12) == 1;
        s_.ring34 = getInt(kStruct34) == 1;
        s_.wheel = get(kWheel);
        s_.efx = getInt(kEfxType);
        s_.efxAmt = get(kEfxAmt) * 0.01f; s_.efxRate = get(kEfxRate) * 0.01f; s_.efxMix = get(kEfxMix) * 0.01f;
        s_.choRate = get(kChoRate); s_.choDepth = get(kChoDepth) * 0.01f; s_.choLevel = get(kChoLevel) * 0.01f;
        s_.revLevel = get(kRevLevel) * 0.01f;
        reverb_.set(get(kRevTime), get(kRevDamp) * 0.01f);
        volume_.setTarget(dbToGain(get(kVolume)));
        for (int t = 0; t < kTones; ++t)
        {
            ToneSettings& x = s_.tone[t];
            x.on = getBool(tp(t, tOn));
            x.src = mpc::clamp(getInt(tp(t, tWave)), 0, ns - 1);
            x.level = get(tp(t, tLevel)) * 0.01f;
            x.pan = get(tp(t, tPan)) * 0.01f;
            x.coarse = (float)getInt(tp(t, tCoarse));
            x.fine = get(tp(t, tFine)) * 0.01f;
            x.keyLo = getInt(tp(t, tKeyLo)); x.keyHi = getInt(tp(t, tKeyHi));
            x.velSens = get(tp(t, tVelSens)) * 0.01f;
            x.ftype = getInt(tp(t, tFType));
            x.cutoff = get(tp(t, tCutoff));
            x.reso = get(tp(t, tReso)) * 0.01f;
            x.keyF = get(tp(t, tKeyF)) * 0.01f;
            x.fenv = get(tp(t, tFEnv)) * 0.01f;
            for (int k = 0; k < 8; ++k) x.env[k] = get(tp(t, tFA + k));
            x.lrate = get(tp(t, tLfoRate));
            x.lshape = getInt(tp(t, tLfoShape));
            x.lfade = get(tp(t, tLfoFade)) * 0.001f;
            x.lpit = get(tp(t, tLfoPitch)) * 0.01f;
            x.lflt = get(tp(t, tLfoFilter)) * 0.01f;
            x.lamp = get(tp(t, tLfoAmp)) * 0.01f;
            x.lpan = get(tp(t, tLfoPan)) * 0.01f;
            x.penv = get(tp(t, tPEnv));
            x.ptime = get(tp(t, tPTime)) * 0.001f;
        }
        // envelope times can change while notes sound
        for (auto& v : voices_)
            for (int t = 0; t < kTones; ++t)
                if (v.t[t].on && !v.t[t].done)
                {
                    const float* e = s_.tone[t].env;
                    v.t[t].fenv.set(e[0], e[1], e[2] * 0.01f, e[3]);
                    v.t[t].amp.set(e[4], e[5], e[6] * 0.01f, e[7]);
                }
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
            if (!sustain_) for (auto& v : voices_) if (v.sustained) { v.sustained = false; release(v); }
        }
        else controllers_.handle(ev);
    }

    void noteOn(int note, int velocity)
    {
        const float from = lastNote_ < 0 ? (float)note : lastNote_;
        lastNote_ = (float)note;
        if (s_.mode == 1)
        {
            // mono: remember held keys (last-note priority); legato keeps the envelopes running
            if (held_ < 16) heldKeys_[held_++] = note;
            Voice& v = voices_[0];
            if (v.active() && (v.held || v.sustained))
            {
                v.note = note; v.held = true; v.sustained = false;
                v.pitchTarget = (float)note;
                if (s_.porta <= 0.0f) v.pitch = (float)note;
                return;
            }
            startVoice(v, note, velocity, s_.porta > 0.0f ? v.active() ? v.pitch : from : (float)note);
            return;
        }
        startVoice(allocate(), note, velocity, s_.porta > 0.0f ? from : (float)note);
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
            if (held_ > 0)   // fall back to the previous key, legato
            {
                v.note = heldKeys_[held_ - 1];
                v.pitchTarget = (float)v.note;
                if (s_.porta <= 0.0f) v.pitch = v.pitchTarget;
                lastNote_ = v.pitchTarget;
                return;
            }
        }
        for (auto& v : voices_)
            if (v.held && v.note == note)
            {
                v.held = false;
                if (sustain_) v.sustained = true; else release(v);
            }
    }

    void startVoice(Voice& v, int note, int velocity, float fromPitch)
    {
        v.note = note; v.held = true; v.sustained = false; v.age = ++age_;
        v.vel = (float)velocity / 127.0f;
        v.pitch = fromPitch; v.pitchTarget = (float)note;
        v.time = 0.0f;
        for (int t = 0; t < kTones; ++t)
        {
            const ToneSettings& x = s_.tone[t];
            Tone& tn = v.t[t];
            const bool was = tn.on && !tn.done;
            tn.on = x.on && note >= x.keyLo && note <= x.keyHi;
            if (!tn.on) { tn.done = true; continue; }
            tn.done = false;
            tn.gain = x.level * lerp(1.0f, v.vel * v.vel, x.velSens);
            tn.ph = 0.0f; tn.spos = 0.0;
            tn.lfoPh = 0.0f; tn.rng = 0x9E3779B9u * (uint32_t)(age_ * 4 + t + 1);
            tn.randVal = 0.0f;
            if (!was) { tn.filter.reset(); tn.amp.reset(); tn.fenv.reset(); tn.gl = tn.gr = 0.0f; }
            tn.fenv.set(x.env[0], x.env[1], x.env[2] * 0.01f, x.env[3]);
            tn.amp.set(x.env[4], x.env[5], x.env[6] * 0.01f, x.env[7]);
            tn.amp.noteOn(); tn.fenv.noteOn();
        }
    }

    void release(Voice& v)
    {
        v.held = v.sustained = false;
        for (auto& t : v.t) { t.amp.noteOff(); t.fenv.noteOff(); }
    }

    void killVoice(Voice& v)
    {
        v.note = -1; v.held = v.sustained = false;
        for (auto& t : v.t) { t.on = false; t.done = true; t.amp.reset(); t.fenv.reset(); t.filter.reset(); }
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

    // ---- sound -------------------------------------------------------------
    float lfoShape(Tone& tn, int shape, float inc)
    {
        float p = tn.lfoPh + inc;
        const bool wrapped = p >= 1.0f;
        if (wrapped) p -= std::floor(p);
        tn.lfoPh = p;
        switch (shape)
        {
            case 1: return std::sin(2.0f * kPiF * p);
            case 2: return 1.0f - 2.0f * p;
            case 3: return p < 0.5f ? 1.0f : -1.0f;
            case 4:
                if (wrapped || tn.randVal == 0.0f) { tn.rng ^= tn.rng << 13; tn.rng ^= tn.rng >> 17; tn.rng ^= tn.rng << 5; tn.randVal = (float)(tn.rng >> 8) / 8388608.0f - 1.0f + 1.0e-6f; }
                return tn.randVal;
            default: return p < 0.25f ? 4.0f * p : p < 0.75f ? 2.0f - 4.0f * p : 4.0f * p - 4.0f;
        }
    }

    void render(int from, int to)
    {
        for (int c = from; c < to; c += kCtl)
        {
            const int n = std::min(kCtl, to - c);
            renderChunk(c, n);
        }
    }

    void renderChunk(int off, int n)
    {
        controllers_.tick();
        const float dtc = (float)kCtl / (float)sr_;
        const float bendSemis = controllers_.pitchBend() * s_.bend;
        const float wheelCents = controllers_.modWheel() * s_.wheel;
        // glide coefficient per control step
        const float glide = s_.porta > 0.0f ? std::exp(-dtc / (s_.porta * 0.001f * 0.4f)) : 0.0f;
        const float invN = 1.0f / (float)n;

        for (auto& v : voices_)
        {
            if (!v.active()) continue;
            v.pitch = v.pitchTarget + (v.pitch - v.pitchTarget) * glide;
            const float baseNote = v.pitch + bendSemis;
            float osc[kTones][kCtl];

            // 1) oscillators
            for (int t = 0; t < kTones; ++t)
            {
                Tone& tn = v.t[t];
                if (!tn.on || tn.done) continue;
                const ToneSettings& x = s_.tone[t];
                const Source& sc = src_[(size_t)x.src];
                const float fade = x.lfade > 0.0f ? std::min(1.0f, v.time / x.lfade) : 1.0f;
                tn.lfoVal = lfoShape(tn, x.lshape, x.lrate * dtc) * fade;
                const float penv = x.penv != 0.0f ? x.penv * std::exp(-v.time / std::max(0.001f, x.ptime)) : 0.0f;
                const float semis = baseNote + x.coarse + x.fine + penv + tn.lfoVal * x.lpit + std::sin(2.0f * kPiF * vibPh_) * wheelCents * 0.01f;
                const float hzNorm = 440.0f * std::pow(2.0f, (semis - 69.0f) / 12.0f) / (float)sr_;
                float* o = osc[t];
                if (sc.kind == Source::Table)
                {
                    tn.inc = std::min(0.49f, hzNorm);
                    tn.lv = wr::levelFor(tn.inc);
                    const float* tb = sc.wave.level(tn.lv);
                    float ph = tn.ph;
                    for (int i = 0; i < n; ++i)
                    {
                        const float p = ph * (float)wr::kTable;
                        const int k = (int)p;
                        const float f = p - (float)k;
                        o[i] = tb[k] + f * (tb[k + 1] - tb[k]);
                        ph += tn.inc; if (ph >= 1.0f) ph -= 1.0f;
                    }
                    tn.ph = ph;
                }
                else if (sc.kind == Source::Noise)
                {
                    uint32_t r = tn.rng;
                    for (int i = 0; i < n; ++i) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; o[i] = ((float)(r >> 8) / 8388608.0f - 1.0f) * 0.6f; }
                    tn.rng = r;
                }
                else
                {
                    const double inc = std::pow(2.0, (double)(semis - (float)sc.root) / 12.0) * sc.rate / sr_;
                    const double end = (double)sc.data.size() - 4.0;
                    const double loopLen = sc.loopEnd - sc.loopStart;
                    double p = tn.spos;
                    const float* d = sc.data.data();
                    for (int i = 0; i < n; ++i)
                    {
                        if (sc.looped) { while (p >= sc.loopEnd) p -= loopLen; }
                        else if (p >= end) { o[i] = 0.0f; if (i == n - 1) tn.done = true; continue; }
                        const size_t k = (size_t)p;
                        const float f = (float)(p - (double)k);
                        const float xm1 = k > 0 ? d[k - 1] : d[k], x0 = d[k], x1 = d[k + 1], x2 = d[k + 2];
                        const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
                        o[i] = ((c3 * f + c2) * f + c1) * f + x0;
                        p += inc;
                    }
                    tn.spos = p;
                }
            }
            // 2) structure: ring modulation of pairs (the second tone of the pair hears osc1 x osc2)
            for (int pr = 0; pr < 2; ++pr)
            {
                if (!(pr == 0 ? s_.ring12 : s_.ring34)) continue;
                Tone& a = v.t[pr * 2];
                Tone& b = v.t[pr * 2 + 1];
                if (!a.on || a.done || !b.on || b.done) continue;
                for (int i = 0; i < n; ++i) osc[pr * 2 + 1][i] *= osc[pr * 2][i] * 2.0f;
            }
            // 3) filter + amp + pan
            for (int t = 0; t < kTones; ++t)
            {
                Tone& tn = v.t[t];
                if (!tn.on || tn.done) continue;
                const ToneSettings& x = s_.tone[t];
                const float oct = x.fenv * tn.fenv.level() * 8.0f + x.keyF * ((float)v.pitch - 60.0f) / 12.0f + tn.lfoVal * x.lflt * 3.0f;
                const bool filt = x.ftype > 0;
                if (filt) tn.filter.set(mpc::clamp(x.cutoff * std::pow(2.0f, mpc::clamp(oct, -10.0f, 10.0f)), 20.0f, 20000.0f), x.reso);
                const int mode = x.ftype == 1 ? 0 : x.ftype == 2 ? 1 : x.ftype == 3 ? 2 : 4;
                const float pan = mpc::clamp(x.pan + tn.lfoVal * x.lpan, -1.0f, 1.0f);
                const float trem = 1.0f - x.lamp * 0.5f * (1.0f - tn.lfoVal);
                const float ang = (pan + 1.0f) * 0.25f * kPiF;
                tn.tgl = std::cos(ang) * trem * tn.gain;
                tn.tgr = std::sin(ang) * trem * tn.gain;
                const float dl = (tn.tgl - tn.gl) * invN, dr = (tn.tgr - tn.gr) * invN;
                const float* o = osc[t];
                float* bl = bufL_ + off;
                float* br = bufR_ + off;
                for (int i = 0; i < n; ++i)
                {
                    tn.fenv.process();
                    float y = filt ? tn.filter.process(o[i], mode) : o[i];
                    y *= tn.amp.process() * 0.3f;
                    tn.gl += dl; tn.gr += dr;
                    bl[i] += y * tn.gl;
                    br[i] += y * tn.gr;
                }
                const bool silentSustain = tn.amp.stage() == ADSR::Stage::Sustain && tn.amp.level() < 1.0e-4f;
                if (!tn.amp.isActive() || silentSustain) { tn.done = true; tn.amp.reset(); tn.filter.reset(); }
            }
            v.time += dtc;
            if (!v.active()) { v.held = v.sustained = false; }
        }
        vibPh_ += 5.5f * dtc;   // mod wheel vibrato at 5.5 Hz
        if (vibPh_ >= 1.0f) vibPh_ -= 1.0f;
    }

    void effects(int n)
    {
        // EFX
        const int type = s_.efx;
        const float mix = s_.efxMix;
        if (type == 1)   // overdrive
        {
            const float drive = 1.0f + s_.efxAmt * 19.0f, norm = 1.0f / fastTanh(drive);
            const float toneA = 0.15f + 0.8f * s_.efxRate;   // brightness: one-pole smoothing
            for (int i = 0; i < n; ++i)
            {
                const float l = fastTanh(bufL_[i] * drive) * norm * 0.5f, r = fastTanh(bufR_[i] * drive) * norm * 0.5f;
                odL_ += toneA * (l - odL_); odR_ += toneA * (r - odR_);
                bufL_[i] += mix * (odL_ - bufL_[i]); bufR_[i] += mix * (odR_ - bufR_[i]);
            }
        }
        else if (type == 2)   // phaser: 4 all-pass stages per side, swept by an LFO
        {
            const float rate = 0.05f + s_.efxRate * s_.efxRate * 5.0f;
            const float fb = s_.efxAmt * 0.7f;
            for (int i = 0; i < n; ++i)
            {
                efxPh_ += rate / (float)sr_; if (efxPh_ >= 1.0f) efxPh_ -= 1.0f;
                for (int side = 0; side < 2; ++side)
                {
                    const float m = 0.5f + 0.5f * std::sin(2.0f * kPiF * (efxPh_ + side * 0.25f));
                    const float hz = 200.0f * std::pow(16.0f, m);
                    const float tt = std::tan(kPiF * std::min(hz, 0.45f * (float)sr_) / (float)sr_);
                    const float a = (tt - 1.0f) / (tt + 1.0f);
                    float& fbS = side ? phzFbR_ : phzFbL_;
                    float x = (side ? bufR_[i] : bufL_[i]) + fbS * fb;
                    for (int st = 0; st < 4; ++st)
                    {
                        float& z = phz_[side * 4 + st];
                        const float y = a * x + z;
                        z = x - a * y;
                        x = y;
                    }
                    fbS = x;
                    float& out = side ? bufR_[i] : bufL_[i];
                    out = (1.0f - 0.5f * mix) * out + 0.5f * mix * x;
                }
            }
        }
        else if (type == 3)   // stereo delay (ping-pong)
        {
            const float ms = 40.0f + s_.efxRate * 960.0f;
            const float d = ms * 0.001f * (float)sr_;
            const float fb = s_.efxAmt * 0.85f;
            for (int i = 0; i < n; ++i)
            {
                const float tl = dlyL_.readLinear(d), tr = dlyR_.readLinear(d);
                dlyL_.write(bufL_[i] * 0.5f + bufR_[i] * 0.5f + tr * fb);
                dlyR_.write(tl * fb);
                bufL_[i] += tl * mix; bufR_[i] += tr * mix;
            }
        }
        else if (type == 4)   // enhancer: add saturated highs
        {
            const float a = std::exp(-2.0f * kPiF * (1500.0f + 4000.0f * (1.0f - s_.efxRate)) / (float)sr_);
            const float amt = s_.efxAmt * 2.0f * mix;
            for (int i = 0; i < n; ++i)
            {
                enhL_ = a * enhL_ + (1.0f - a) * bufL_[i];
                enhR_ = a * enhR_ + (1.0f - a) * bufR_[i];
                bufL_[i] += fastTanh((bufL_[i] - enhL_) * 4.0f) * 0.25f * amt;
                bufR_[i] += fastTanh((bufR_[i] - enhR_) * 4.0f) * 0.25f * amt;
            }
        }
        // chorus + reverb
        const float choInc = s_.choRate / (float)sr_;
        const float base = 0.010f * (float)sr_, depth = 0.006f * (float)sr_ * s_.choDepth;
        const float cl = s_.choLevel, rl = s_.revLevel;
        for (int i = 0; i < n; ++i)
        {
            float l = bufL_[i], r = bufR_[i];
            choL_.write(l); choR_.write(r);
            if (cl > 0.0f)
            {
                choPh_ += choInc; if (choPh_ >= 1.0f) choPh_ -= 1.0f;
                const float sl = std::sin(2.0f * kPiF * choPh_), sr = std::cos(2.0f * kPiF * choPh_);
                const float wl = choL_.readLinear(base + depth * sl), wr2 = choR_.readLinear(base + depth * sr);
                l = l * (1.0f - 0.4f * cl) + wl * cl;
                r = r * (1.0f - 0.4f * cl) + wr2 * cl;
            }
            if (rl > 0.0f)
            {
                float rvL, rvR;
                reverb_.process(l, r, rvL, rvR);
                l += rvL * rl; r += rvR * rl;
            }
            else { float a, b; reverb_.process(0.0f, 0.0f, a, b); }
            bufL_[i] = l; bufR_[i] = r;
        }
    }

    // ---- presets -----------------------------------------------------------
    void makePrograms()
    {
        auto idx = [&](const char* n) { for (size_t k = 0; k < names_.size(); ++k) if (names_[k] == n) return (float)k; return 0.0f; };
        struct T { int tone; std::initializer_list<std::pair<int, float>> v; };
        auto program = [&](const char* name, std::initializer_list<std::pair<int, float>> common, std::initializer_list<T> tones)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (int t = 0; t < kTones; ++t) v[(size_t)tp(t, tOn)] = 0.0f;
            for (const auto& p : common) v[(size_t)p.first] = p.second;
            for (const auto& t : tones)
            {
                v[(size_t)tp(t.tone, tOn)] = 1.0f;
                for (const auto& p : t.v) v[(size_t)tp(t.tone, p.first)] = p.second;
            }
            addProgram(name, std::move(v));
        };
        const float W_SAW = idx("Saw"), W_BSAW = idx("Bright Saw"), W_SIN = idx("Sine"), W_TRI = idx("Triangle"), W_SQ = idx("Square"),
                    W_P25 = idx("Pulse 25"), W_BELL = idx("Bell Tone"), W_GLASS = idx("Glass"), W_CHOIR = idx("Choir"), W_SORG = idx("Soft Organ"),
                    W_ORG = idx("Organ"), W_FMB = idx("FM Bright"), W_FMM = idx("FM Metal"), W_HOL = idx("Hollow"), W_PLK = idx("Pluck"),
                    W_NOISE = idx("Noise"), W_AH = idx("Vox Ah"), W_OO = idx("Vox Oo"), W_ODD = idx("Odd Saw");

        program("Rack Strings", {{kChoLevel, 45}, {kRevLevel, 30}, {kRevTime, 2.8f}},
            {{0, {{tWave, W_SAW}, {tFine, -7}, {tPan, -40}, {tCutoff, 2600}, {tReso, 5}, {tA, 350}, {tD, 1000}, {tS, 90}, {tR, 700}, {tLfoRate, 5.2f}, {tLfoFade, 600}, {tLfoPitch, 6}}},
             {1, {{tWave, W_SAW}, {tFine, 8}, {tPan, 40}, {tCutoff, 2400}, {tReso, 5}, {tA, 420}, {tD, 1000}, {tS, 90}, {tR, 750}, {tLfoRate, 4.7f}, {tLfoFade, 700}, {tLfoPitch, 6}}},
             {2, {{tWave, W_ODD}, {tCoarse, 12}, {tLevel, 35}, {tCutoff, 3500}, {tA, 600}, {tS, 80}, {tR, 800}}}});
        program("Glass Heaven", {{kChoLevel, 50}, {kRevLevel, 45}, {kRevTime, 5.0f}},
            {{0, {{tWave, W_BELL}, {tLevel, 70}, {tCoarse, 12}, {tFType, 0}, {tA, 1}, {tD, 2800}, {tS, 0}, {tR, 2500}, {tVelSens, 70}}},
             {1, {{tWave, W_GLASS}, {tLevel, 45}, {tCoarse, 24}, {tFine, 4}, {tFType, 0}, {tA, 1}, {tD, 1500}, {tS, 0}, {tR, 1500}, {tPan, 50}}},
             {2, {{tWave, W_CHOIR}, {tLevel, 75}, {tCutoff, 3000}, {tA, 900}, {tD, 2000}, {tS, 90}, {tR, 2500}, {tLfoRate, 0.3f}, {tLfoPan, 60}}},
             {3, {{tWave, W_SORG}, {tLevel, 30}, {tFine, -6}, {tA, 1200}, {tS, 100}, {tR, 2500}, {tPan, -50}}}});
        program("Tine Keys", {{kEfxType, 2}, {kEfxRate, 25}, {kEfxAmt, 30}, {kEfxMix, 45}, {kChoLevel, 20}, {kRevLevel, 20}},
            {{0, {{tWave, W_SIN}, {tFType, 0}, {tA, 1}, {tD, 3000}, {tS, 15}, {tR, 350}, {tVelSens, 75}}},
             {1, {{tWave, W_FMB}, {tLevel, 40}, {tCoarse, 12}, {tFType, 1}, {tCutoff, 5000}, {tA, 1}, {tD, 400}, {tS, 0}, {tR, 200}, {tVelSens, 100}}},
             {2, {{tWave, W_FMM}, {tLevel, 18}, {tCoarse, 24}, {tFType, 0}, {tA, 0.5f}, {tD, 90}, {tS, 0}, {tR, 60}, {tVelSens, 100}}}});
        program("Funk Whistle", {{kMode, 1}, {kPorta, 140}, {kEfxType, 3}, {kEfxRate, 35}, {kEfxAmt, 30}, {kEfxMix, 25}, {kChoLevel, 0}, {kRevLevel, 20}},
            {{0, {{tWave, W_SIN}, {tFType, 0}, {tA, 15}, {tS, 100}, {tR, 180}, {tVelSens, 20}, {tLfoRate, 5.6f}, {tLfoShape, 1}, {tLfoFade, 450}, {tLfoPitch, 22}}},
             {1, {{tWave, W_TRI}, {tLevel, 18}, {tCoarse, 12}, {tFType, 0}, {tA, 20}, {tS, 100}, {tR, 180}, {tLfoRate, 5.6f}, {tLfoShape, 1}, {tLfoFade, 450}, {tLfoPitch, 22}}}});
        program("Sub Rumble", {{kMode, 1}, {kPorta, 30}, {kChoLevel, 0}, {kRevLevel, 0}},
            {{0, {{tWave, W_SIN}, {tCoarse, -12}, {tLevel, 100}, {tFType, 0}, {tA, 2}, {tD, 1500}, {tS, 85}, {tR, 120}, {tVelSens, 30}}},
             {1, {{tWave, W_TRI}, {tLevel, 35}, {tCutoff, 900}, {tFEnv, 25}, {tFD, 200}, {tFS, 0}, {tA, 2}, {tD, 300}, {tS, 40}, {tR, 100}}}});
        program("Fretless Glide", {{kMode, 1}, {kPorta, 70}, {kChoLevel, 15}, {kRevLevel, 8}},
            {{0, {{tWave, W_HOL}, {tCoarse, -12}, {tCutoff, 700}, {tReso, 20}, {tFEnv, 35}, {tFA, 30}, {tFD, 600}, {tFS, 20}, {tA, 12}, {tD, 2000}, {tS, 60}, {tR, 150}, {tLfoRate, 4.5f}, {tLfoFade, 700}, {tLfoPitch, 12}}},
             {1, {{tWave, W_SIN}, {tCoarse, -12}, {tLevel, 60}, {tFType, 0}, {tA, 12}, {tS, 90}, {tR, 150}}}});
        program("Pizz Section", {{kChoLevel, 30}, {kRevLevel, 30}, {kRevTime, 1.8f}},
            {{0, {{tWave, W_SAW}, {tFine, -5}, {tPan, -30}, {tCutoff, 900}, {tFEnv, 55}, {tFA, 1}, {tFD, 140}, {tFS, 0}, {tA, 1}, {tD, 280}, {tS, 0}, {tR, 220}, {tVelSens, 70}}},
             {1, {{tWave, W_PLK}, {tFine, 6}, {tPan, 30}, {tCutoff, 1500}, {tFEnv, 40}, {tFD, 120}, {tFS, 0}, {tA, 1}, {tD, 250}, {tS, 0}, {tR, 200}}},
             {2, {{tWave, W_NOISE}, {tLevel, 25}, {tFType, 3}, {tCutoff, 3000}, {tA, 0.5f}, {tD, 25}, {tS, 0}, {tR, 20}}}});
        program("Rack Brass", {{kChoLevel, 25}, {kRevLevel, 20}},
            {{0, {{tWave, W_SAW}, {tCutoff, 700}, {tReso, 10}, {tFEnv, 45}, {tFA, 70}, {tFD, 500}, {tFS, 55}, {tFR, 250}, {tA, 25}, {tD, 600}, {tS, 85}, {tR, 250}, {tPEnv, -1.0f}, {tPTime, 70}, {tVelSens, 70}}},
             {1, {{tWave, W_SAW}, {tFine, 9}, {tPan, 25}, {tCutoff, 800}, {tFEnv, 45}, {tFA, 90}, {tFD, 500}, {tFS, 55}, {tA, 30}, {tS, 85}, {tR, 250}, {tPEnv, -1.0f}, {tPTime, 90}}},
             {2, {{tWave, W_BSAW}, {tCoarse, -12}, {tLevel, 40}, {tPan, -25}, {tCutoff, 500}, {tFEnv, 35}, {tFA, 80}, {tFD, 500}, {tFS, 40}, {tA, 30}, {tS, 85}, {tR, 250}}}});
        program("Big Stab", {{kChoLevel, 20}, {kRevLevel, 35}, {kRevTime, 2.5f}},
            {{0, {{tWave, W_BSAW}, {tCutoff, 4000}, {tA, 1}, {tD, 450}, {tS, 0}, {tR, 350}, {tVelSens, 40}}},
             {1, {{tWave, W_SQ}, {tCoarse, 12}, {tLevel, 60}, {tPan, -35}, {tCutoff, 5000}, {tA, 1}, {tD, 380}, {tS, 0}, {tR, 300}}},
             {2, {{tWave, W_SAW}, {tCoarse, 19}, {tLevel, 45}, {tPan, 35}, {tCutoff, 6000}, {tA, 1}, {tD, 320}, {tS, 0}, {tR, 260}}},
             {3, {{tWave, W_NOISE}, {tLevel, 40}, {tFType, 2}, {tCutoff, 2500}, {tReso, 20}, {tA, 0.5f}, {tD, 140}, {tS, 0}, {tR, 120}}}});
        program("Choir Air", {{kChoLevel, 45}, {kRevLevel, 45}, {kRevTime, 4.0f}},
            {{0, {{tWave, W_AH}, {tPan, -35}, {tCutoff, 3500}, {tA, 500}, {tD, 1500}, {tS, 90}, {tR, 1500}, {tLfoRate, 4.8f}, {tLfoFade, 800}, {tLfoPitch, 8}}},
             {1, {{tWave, W_OO}, {tFine, 7}, {tPan, 35}, {tCutoff, 3000}, {tA, 650}, {tS, 90}, {tR, 1600}, {tLfoRate, 5.3f}, {tLfoFade, 900}, {tLfoPitch, 8}}},
             {2, {{tWave, W_NOISE}, {tLevel, 12}, {tFType, 2}, {tCutoff, 2200}, {tReso, 30}, {tKeyF, 100}, {tA, 400}, {tS, 70}, {tR, 1200}}}});
        program("Square Lead", {{kMode, 1}, {kPorta, 45}, {kEfxType, 3}, {kEfxRate, 40}, {kEfxAmt, 35}, {kEfxMix, 30}, {kChoLevel, 15}, {kRevLevel, 15}},
            {{0, {{tWave, W_SQ}, {tCutoff, 1800}, {tReso, 25}, {tFEnv, 35}, {tFD, 400}, {tFS, 40}, {tA, 3}, {tS, 95}, {tR, 150}, {tLfoRate, 5.5f}, {tLfoFade, 500}, {tLfoPitch, 10}}},
             {1, {{tWave, W_P25}, {tFine, 10}, {tLevel, 55}, {tCutoff, 2200}, {tFEnv, 30}, {tFD, 400}, {tFS, 40}, {tA, 3}, {tS, 95}, {tR, 150}}}});
        program("Organ Drive", {{kEfxType, 1}, {kEfxAmt, 35}, {kEfxRate, 60}, {kEfxMix, 70}, {kChoRate, 6.0f}, {kChoDepth, 25}, {kChoLevel, 45}, {kRevLevel, 15}},
            {{0, {{tWave, W_ORG}, {tFType, 0}, {tA, 2}, {tS, 100}, {tR, 40}, {tVelSens, 0}}},
             {1, {{tWave, W_SORG}, {tCoarse, 12}, {tLevel, 45}, {tFType, 0}, {tA, 2}, {tS, 100}, {tR, 40}, {tVelSens, 0}}},
             {2, {{tWave, W_SIN}, {tCoarse, 19}, {tLevel, 18}, {tFType, 0}, {tA, 1}, {tD, 120}, {tS, 0}, {tR, 40}, {tVelSens, 0}}}});
        program("Analog Sweep", {{kChoLevel, 40}, {kRevLevel, 35}, {kRevTime, 3.5f}},
            {{0, {{tWave, W_SAW}, {tFine, -9}, {tPan, -45}, {tCutoff, 600}, {tReso, 45}, {tLfoRate, 0.12f}, {tLfoFilter, 45}, {tA, 700}, {tS, 100}, {tR, 1800}}},
             {1, {{tWave, W_SAW}, {tFine, 9}, {tPan, 45}, {tCutoff, 650}, {tReso, 45}, {tLfoRate, 0.15f}, {tLfoFilter, 45}, {tA, 800}, {tS, 100}, {tR, 1800}}}});
        program("Ring Bell", {{kStruct12, 1}, {kChoLevel, 30}, {kRevLevel, 40}, {kRevTime, 4.0f}},
            {{0, {{tWave, W_SIN}, {tLevel, 25}, {tFType, 0}, {tA, 1}, {tD, 2500}, {tS, 0}, {tR, 2000}}},
             {1, {{tWave, W_SIN}, {tCoarse, 7}, {tFine, 31}, {tLevel, 85}, {tFType, 0}, {tA, 1}, {tD, 1800}, {tS, 0}, {tR, 1800}, {tVelSens, 70}}},
             {2, {{tWave, W_GLASS}, {tCoarse, 12}, {tLevel, 30}, {tFType, 0}, {tA, 1}, {tD, 3000}, {tS, 0}, {tR, 2000}}}});
        program("Bass + Keys Split", {{kChoLevel, 25}, {kRevLevel, 15}},
            {{0, {{tWave, W_SIN}, {tCoarse, -12}, {tKeyHi, 52}, {tLevel, 100}, {tFType, 0}, {tA, 2}, {tS, 85}, {tR, 120}}},
             {1, {{tWave, W_TRI}, {tKeyHi, 52}, {tLevel, 30}, {tCutoff, 800}, {tA, 2}, {tD, 300}, {tS, 40}, {tR, 100}}},
             {2, {{tWave, W_SIN}, {tKeyLo, 53}, {tFType, 0}, {tA, 1}, {tD, 3000}, {tS, 15}, {tR, 350}, {tVelSens, 75}}},
             {3, {{tWave, W_FMB}, {tKeyLo, 53}, {tLevel, 35}, {tCoarse, 12}, {tCutoff, 5000}, {tA, 1}, {tD, 400}, {tS, 0}, {tR, 200}, {tVelSens, 100}}}});
        program("Init Tone", {}, {{0, {}}});
    }

    const std::vector<Source>& src_;
    std::vector<std::string> names_;
    double sr_ = 44100.0;
    Voice voices_[kVoices];
    unsigned age_ = 0;
    bool sustain_ = false;
    int heldKeys_[16] = {}, held_ = 0;
    float lastNote_ = -1.0f, vibPh_ = 0.0f;
    float bufL_[kMaxBlock], bufR_[kMaxBlock];
    DelayLine choL_, choR_, dlyL_, dlyR_;
    Reverb reverb_;
    float choPh_ = 0, efxPh_ = 0, phz_[8] = {}, phzFbL_ = 0, phzFbR_ = 0, enhL_ = 0, enhR_ = 0, odL_ = 0, odR_ = 0;
    Smoother volume_;
    MidiControllers controllers_;
};

MPC_PLUGIN_ENTRY(MeltRack)
