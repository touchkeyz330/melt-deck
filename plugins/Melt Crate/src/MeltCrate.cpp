// MeltCrate.cpp - an early-90s keyboard-sampler style instrument for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// An original design in the spirit of the classic 16-bit sampling keyboards of the early 90s:
// play your own WAVs (one-shots, loops, or whole multisampled instruments) with loop modes,
// chopping across keys, velocity-to-start, a dual filter, three envelopes, an LFO, a
// CHARACTER stage (Clean, Vintage, Crunch, Dusty: interpolation, engine rate, bits, smoothing)
// and chorus / delay / reverb. Not a circuit-accurate copy of any product.
//
// Sounds: Content/Samples/
//   - every WAV file is a sound (root note and loop from its sampler 'smpl' chunk, else C3 = 60
//     and no loop),
//   - every sub-folder of WAVs is a multisampled sound: each file's root comes from a note name
//     or number in its file name ("Keys C3.wav", "Bass_F#1.wav", "pad 48.wav") or its smpl chunk;
//     key ranges split halfway between roots.
// Only the selected sound is in memory; picking another loads it on a background thread.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/svf.h"
#include "dsp/adsr.h"
#include "dsp/biquad.h"
#include "dsp/smoother.h"
#include "dsp/delay_line.h"
#include "dsp/midi_cc.h"
#include "dsp/wav.h"
#include "reverb.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <sys/stat.h>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <dirent.h>
#endif

using namespace mpc::dsp;
static constexpr float kPiF = 3.14159265358979f;

// ---------------------------------------------------------------------------
// Sound list and loading
// ---------------------------------------------------------------------------
struct Entry { std::string name, path; bool folder = false; };

static std::vector<std::string> listDir(const std::string& dir)
{
    std::vector<std::string> names;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "/*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do { names.push_back(fd.cFileName); } while (FindNextFileA(h, &fd)); FindClose(h); }
#else
    if (DIR* d = opendir(dir.c_str())) { while (dirent* e = readdir(d)) names.push_back(e->d_name); closedir(d); }
#endif
    names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& n) { return n.empty() || n[0] == '.'; }), names.end());
    std::sort(names.begin(), names.end());
    return names;
}
static bool isDir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && (st.st_mode & S_IFDIR); }
static bool isWav(const std::string& n)
{
    if (n.size() < 5) return false;
    std::string e = n.substr(n.size() - 4);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e == ".wav";
}

static std::vector<Entry> scanSounds()
{
    std::vector<Entry> out;
    const std::string dir = mpc::contentPath("Samples");
    for (const auto& n : listDir(dir))
    {
        const std::string p = dir + "/" + n;
        if (isDir(p))
        {
            bool any = false;
            for (const auto& f : listDir(p)) if (isWav(f)) { any = true; break; }
            if (any) out.push_back({n.size() > 16 ? n.substr(0, 16) : n, p, true});
        }
        else if (isWav(n))
        {
            std::string b = n.substr(0, n.size() - 4);
            out.push_back({b.size() > 16 ? b.substr(0, 16) : b, p, false});
        }
        if (out.size() >= 512) break;
    }
    return out;
}

static void readSamplerChunk(const std::vector<uint8_t>& d, int& root, bool& looped, double& ls, double& le)
{
    auto u32 = [&](size_t p) { return (uint32_t)d[p] | ((uint32_t)d[p + 1] << 8) | ((uint32_t)d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24); };
    if (d.size() < 12) return;
    for (size_t p = 12; p + 8 <= d.size();)
    {
        const uint32_t len = u32(p + 4);
        const size_t body = p + 8;
        if (len > d.size() || body + len > d.size()) break;
        if (std::memcmp(d.data() + p, "smpl", 4) == 0 && len >= 36)
        {
            const uint32_t unity = u32(body + 12);
            if (unity <= 127) root = (int)unity;
            if (u32(body + 28) >= 1 && len >= 60)
            {
                const uint32_t s = u32(body + 44), e = u32(body + 48);
                if (e > s + 8) { looped = true; ls = s; le = (double)e + 1.0; }
            }
            return;
        }
        p = body + len + (len & 1u);
    }
}

// Note from a file name: a token like C3, F#1, Bb-1 (C3 = 60) or a number 0-127.
static int noteFromName(const std::string& file)
{
    std::string n = file.substr(0, file.size() - 4);
    for (char& c : n) if (c == '_' || c == '-' || c == '.' || c == '(' || c == ')') c = ' ';
    // keep '-' for negative octaves: re-scan the original for "x-1"
    std::vector<std::string> toks;
    std::string cur;
    for (char c : n) { if (c == ' ') { if (!cur.empty()) toks.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) toks.push_back(cur);
    int found = -1;
    for (const auto& t : toks)
    {
        static const int pcs[7] = {9, 11, 0, 2, 4, 5, 7};   // A B C D E F G
        const char c0 = (char)std::toupper((unsigned char)t[0]);
        if (c0 >= 'A' && c0 <= 'G' && t.size() >= 2 && t.size() <= 3)
        {
            int pc = pcs[c0 - 'A'];
            size_t k = 1;
            if (t[k] == '#') { ++pc; ++k; } else if (t[k] == 'b' && t.size() == 3) { --pc; ++k; }
            if (k < t.size() && std::isdigit((unsigned char)t[k]) && k + 1 == t.size())
            {
                const int midi = (t[k] - '0' + 2) * 12 + pc;
                if (midi >= 0 && midi <= 127) found = midi;
            }
        }
        else if (std::all_of(t.begin(), t.end(), [](char c) { return std::isdigit((unsigned char)c); }) && t.size() <= 3)
        {
            const int v = std::atoi(t.c_str());
            if (v >= 0 && v <= 127 && found < 0) found = v;
        }
    }
    // octave -1 / -2 written with a dash, e.g. "C-1"
    for (size_t i = 0; i + 2 < file.size(); ++i)
        if (file[i + 1] == '-' && std::isdigit((unsigned char)file[i + 2]) && std::strchr("ABCDEFGabcdefg", file[i]))
        {
            static const int pcs[7] = {9, 11, 0, 2, 4, 5, 7};
            const int midi = (2 - (file[i + 2] - '0')) * 12 + pcs[std::toupper((unsigned char)file[i]) - 'A'];
            if (midi >= 0 && midi <= 127 && (i + 3 == file.size() - 4 || !std::isalnum((unsigned char)file[i + 3]))) found = midi;
        }
    return found;
}

struct Zone
{
    std::vector<float> l, r;     // r empty for mono; 4 guard samples at the end
    size_t frames = 0;
    double rate = 44100.0;
    int root = 60, lo = 0, hi = 127;
    bool looped = false;
    double ls = 0, le = 0;
};
struct Sound { std::vector<Zone> zones; };

static bool loadZone(const std::string& path, Zone& z, int fallbackRoot, size_t& budget)
{
    std::vector<uint8_t> bytes;
    if (!mpc::readFile(path, bytes)) return false;
    dsp::WavData w;
    if (!dsp::parseWav(bytes, w) || w.numFrames() < 8) return false;
    const size_t n = w.numFrames();
    if (n > budget) { mpc::log("MeltCrate: %s is too long, skipped", path.c_str()); return false; }
    budget -= n;
    z.frames = n;
    z.rate = w.sampleRate > 1000.0 ? w.sampleRate : 44100.0;
    z.root = fallbackRoot;
    int root = -1;
    readSamplerChunk(bytes, root, z.looped, z.ls, z.le);
    if (fallbackRoot < 0) z.root = root >= 0 ? root : 60;
    else z.root = fallbackRoot;
    if (z.looped) { z.le = std::min(z.le, (double)n); if (z.le - z.ls < 8) z.looped = false; }
    z.l.assign(w.channels[0].begin(), w.channels[0].end());
    if (w.numChannels() >= 2) z.r.assign(w.channels[1].begin(), w.channels[1].end());
    for (int g = 0; g < 4; ++g) { z.l.push_back(0.0f); if (!z.r.empty()) z.r.push_back(0.0f); }
    return true;
}

static Sound* loadSound(const Entry& e)
{
    auto* s = new Sound;
    size_t budget = 16u * 1024u * 1024u;   // frames per sound (~6 min stereo at 44.1 kHz)
    if (!e.folder)
    {
        Zone z;
        if (loadZone(e.path, z, -1, budget)) s->zones.push_back(std::move(z));
    }
    else
    {
        for (const auto& f : listDir(e.path))
        {
            if (!isWav(f) || s->zones.size() >= 128) continue;
            Zone z;
            const int nn = noteFromName(f);
            if (!loadZone(e.path + "/" + f, z, nn, budget)) { mpc::log("MeltCrate: could not read %s", f.c_str()); continue; }
            s->zones.push_back(std::move(z));
        }
        std::sort(s->zones.begin(), s->zones.end(), [](const Zone& a, const Zone& b) { return a.root < b.root; });
        for (size_t i = 0; i < s->zones.size(); ++i)
        {
            s->zones[i].lo = i == 0 ? 0 : (s->zones[i - 1].root + s->zones[i].root) / 2 + 1;
            s->zones[i].hi = i + 1 == s->zones.size() ? 127 : (s->zones[i].root + s->zones[i + 1].root) / 2;
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
class MeltCrate : public mpc::Plugin
{
public:
    enum Params
    {
        kLoopMode = 0, kTrigger, kSlices, kSliceKey, kStart, kVelStart, kTranspose, kFine, kKeyTrack,
        kVoiceMode, kGlide, kBend, kVolume, kPan,
        kFMode, kCutoff, kReso, kFEnv, kKeyF, kVelF, kFA, kFD, kFS, kFR,
        kA, kD, kS, kR, kVelAmp, kPEnv, kPDecay, kLRate, kLWave, kLDelay, kLPitch, kLFilter, kLAmp,
        kChar, kBits, kDrive, kChoLevel, kChoRate, kDlyTime, kDlyFb, kDlyMix, kRevTime, kRevLevel,
        kSound,   // last on purpose: presets hold every parameter except this one, so they keep your sound
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt Crate";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'L', 'Q', 'x');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    MeltCrate() : mpc::Plugin(makeInfo()), entries_(scanSounds())
    {
        using namespace mpc;
        std::vector<std::string> names;
        for (const auto& e : entries_) names.push_back(e.name);
        if (names.empty()) names.push_back("(no samples)");
        addParam(choiceParam("loop", "Loop Mode", {"One Shot", "Loop", "Ping-Pong", "Reverse", "Loop+Rel"}, 0));
        addParam(choiceParam("trig", "Trigger", {"Gate", "One Shot"}, 0));
        addParam(choiceParam("slices", "Slices", {"Off", "2", "4", "8", "16", "32", "64"}, 0));
        addParam(intParam("slkey", "Slice Key", 0, 127, 36));
        addParam(floatParam("start", "Start", 0.0f, 95.0f, 0.0f, "%", 1));
        addParam(floatParam("velstart", "Vel>Start", 0.0f, 50.0f, 0.0f, "%", 1));
        addParam(intParam("trans", "Transpose", -24, 24, 0, "st"));
        addParam(floatParam("fine", "Fine", -100.0f, 100.0f, 0.0f, "ct", 0));
        addParam(boolParam("keytrk", "Key Track", true));
        addParam(choiceParam("vmode", "Voice Mode", {"Poly", "Mono"}, 0));
        addParam(floatParam("glide", "Glide", 0.0f, 2000.0f, 0.0f, "ms", 0, 0.3f));
        addParam(intParam("bend", "Bend Range", 0, 24, 2, "st"));
        addParam(floatParam("vol", "Volume", -40.0f, 6.0f, -3.0f, "dB", 1));
        addParam(floatParam("pan", "Pan", -100.0f, 100.0f, 0.0f, "", 0));
        addParam(choiceParam("fmode", "Filter", {"Off", "LP 24", "LP 12", "Band", "HP 12", "HP 24"}, 0));
        addParam(logParam("cut", "Cutoff", 20.0f, 20000.0f, 8000.0f, "Hz", 0));
        addParam(floatParam("res", "Reso", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("fenv", "Filt Env", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("keyf", "Key Follow", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam("velf", "Vel>Filter", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam("fa", "F Attack", 0.5f, 10000.0f, 1.0f, "ms", 0));
        addParam(logParam("fd", "F Decay", 1.0f, 10000.0f, 500.0f, "ms", 0));
        addParam(floatParam("fs", "F Sustain", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam("fr", "F Release", 1.0f, 10000.0f, 300.0f, "ms", 0));
        addParam(logParam("a", "Attack", 0.5f, 10000.0f, 0.5f, "ms", 1));
        addParam(logParam("d", "Decay", 1.0f, 20000.0f, 1000.0f, "ms", 0));
        addParam(floatParam("s", "Sustain", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(logParam("r", "Release", 1.0f, 10000.0f, 150.0f, "ms", 0));
        addParam(floatParam("velamp", "Vel>Amp", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam("penv", "Pitch Env", -24.0f, 24.0f, 0.0f, "st", 1));
        addParam(logParam("pdec", "P Env Decay", 1.0f, 4000.0f, 100.0f, "ms", 0));
        addParam(logParam("lrate", "LFO Rate", 0.05f, 20.0f, 5.0f, "Hz", 2));
        addParam(choiceParam("lwave", "LFO Wave", {"Tri", "Sine", "Saw", "Square", "Random"}, 0));
        addParam(floatParam("ldelay", "LFO Delay", 0.0f, 5000.0f, 0.0f, "ms", 0, 0.3f));
        addParam(floatParam("lpit", "LFO>Pitch", 0.0f, 100.0f, 0.0f, "ct", 0));
        addParam(floatParam("lflt", "LFO>Filter", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("lamp", "LFO>Amp", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("char", "Character", {"Clean", "Vintage", "Crunch", "Dusty"}, 1));
        addParam(intParam("bits", "Bits", 6, 16, 16));
        addParam(floatParam("drive", "Drive", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("cholev", "Chorus", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam("chorate", "Chorus Rate", 0.05f, 8.0f, 0.6f, "Hz", 2));
        addParam(floatParam("dtime", "Delay Time", 20.0f, 1000.0f, 375.0f, "ms", 0));
        addParam(floatParam("dfb", "Delay Fdbk", 0.0f, 90.0f, 30.0f, "%", 0));
        addParam(floatParam("dmix", "Delay Mix", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam("revtime", "Reverb Time", 0.2f, 12.0f, 1.8f, "s", 1));
        addParam(floatParam("revlev", "Reverb", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("sound", "Sound", names, 0));
        if (numParams() != kNumParams) mpc::log("MeltCrate: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);
        makePrograms();
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~MeltCrate() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
        delete cur_;
        delete pending_.exchange(nullptr);
        delete retired_.exchange(nullptr);
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        for (auto& v : voices_) { v.amp.setSampleRate(sampleRate); v.fenv.setSampleRate(sampleRate); for (auto& f : v.f) f.setSampleRate(sampleRate); }
        choL_.prepare((int)(0.04 * sampleRate) + 4);
        dlyL_.prepare((int)(1.05 * sampleRate) + 4);
        dlyR_.prepare((int)(1.05 * sampleRate) + 4);
        reverb_.prepare(sampleRate);
        volume_.setTime(15.0f, sampleRate);
        controllers_.prepare(sampleRate / kCtl, 10.0f);
        reset();
        requestSound(getInt(kSound));
        // first load: give the worker a moment so a sound is ready before the first notes
        for (int i = 0; i < 200 && !pending_.load() && !cur_ && !entries_.empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    void reset() override
    {
        for (auto& v : voices_) v.stop();
        choL_.reset(); dlyL_.reset(); dlyR_.reset(); reverb_.reset(); smooth_.reset();
        choPh_ = lfoPh_ = 0.0f; rnd_ = 0.0f; holdPh_ = 0.0f; holdL_ = holdR_ = 0.0f;
        sustain_ = false; age_ = 0; held_ = 0; lastNote_ = -1.0f;
        volume_.snap(dbToGain(get(kVolume)));
        controllers_.reset();
    }

    void process(mpc::ProcessContext& ctx) override
    {
        // swap in a newly loaded sound
        if (Sound* p = pending_.load())
        {
            if (retired_.load() == nullptr)
            {
                pending_.store(nullptr);
                for (auto& v : voices_) v.stop();
                retired_.store(cur_);
                cur_ = p;
            }
        }
        if (getInt(kSound) != requested_) requestSound(getInt(kSound));
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
        output(L, R, nf);
        for (int i = nf; i < ctx.numFrames; ++i) { L[i] = 0.0f; if (R) R[i] = 0.0f; }
    }

private:
    static constexpr int kVoices = 16;
    static constexpr int kCtl = 16;
    static constexpr int kMaxBlock = 4096;

    struct Voice
    {
        const Zone* z = nullptr;
        int note = -1;
        bool held = false, sustained = false, slice = false, done = true;
        unsigned age = 0;
        float vel = 1.0f, pitch = 60.0f, target = 60.0f, time = 0.0f, gain = 1.0f;
        double pos = 0, start = 0, end = 0, ls = 0, le = 0;
        int dir = 1, mode = 0;
        ADSR amp, fenv;
        SVF f[4];   // L stage 1, L stage 2, R stage 1, R stage 2
        bool active() const { return z != nullptr && !done; }
        void stop() { z = nullptr; done = true; amp.reset(); fenv.reset(); for (auto& x : f) x.reset(); note = -1; held = sustained = false; }
    };

    struct Settings
    {
        int loop = 0, trig = 0, slices = 0, sliceKey = 36, vmode = 0, fmode = 0, lwave = 0, chr = 1, bits = 16;
        bool keyTrack = true;
        float start = 0, velStart = 0, transpose = 0, glide = 0, bend = 2, pan = 0;
        float cutoff = 8000, reso = 0, fenv = 0, keyF = 0.5f, velF = 0, velAmp = 0.5f, penv = 0, pdec = 0.1f;
        float lrate = 5, ldelay = 0, lpit = 0, lflt = 0, lamp = 0, drive = 0;
        float choLevel = 0, choRate = 0.6f, dTime = 375, dFb = 0.3f, dMix = 0, revLevel = 0;
        float env[8] = {};
    } s_;

    // ---- loading -----------------------------------------------------------
    void requestSound(int idx)
    {
        requested_ = idx;
        if (idx >= 0 && idx < (int)entries_.size()) request_.store(idx);
    }

    void workerLoop()
    {
        while (!quit_.load())
        {
            delete retired_.exchange(nullptr);
            const int want = request_.exchange(-1);
            if (want >= 0)
            {
                Sound* s = loadSound(entries_[(size_t)want]);
                if (request_.load() >= 0) { delete s; continue; }   // a newer request arrived: skip this one
                delete pending_.exchange(s);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // ---- settings ----------------------------------------------------------
    void readSettings()
    {
        static const int sliceCounts[7] = {0, 2, 4, 8, 16, 32, 64};
        s_.loop = getInt(kLoopMode); s_.trig = getInt(kTrigger);
        s_.slices = sliceCounts[mpc::clamp(getInt(kSlices), 0, 6)];
        s_.sliceKey = getInt(kSliceKey);
        s_.start = get(kStart) * 0.01f; s_.velStart = get(kVelStart) * 0.01f;
        s_.transpose = (float)getInt(kTranspose) + get(kFine) * 0.01f;
        s_.keyTrack = getBool(kKeyTrack);
        s_.vmode = getInt(kVoiceMode); s_.glide = get(kGlide); s_.bend = (float)getInt(kBend);
        s_.pan = get(kPan) * 0.01f;
        s_.fmode = getInt(kFMode); s_.cutoff = get(kCutoff); s_.reso = get(kReso) * 0.01f;
        s_.fenv = get(kFEnv) * 0.01f; s_.keyF = get(kKeyF) * 0.01f; s_.velF = get(kVelF) * 0.01f;
        for (int k = 0; k < 8; ++k) s_.env[k] = get(kFA + k);
        s_.velAmp = get(kVelAmp) * 0.01f;
        s_.penv = get(kPEnv); s_.pdec = get(kPDecay) * 0.001f;
        s_.lrate = get(kLRate); s_.lwave = getInt(kLWave); s_.ldelay = get(kLDelay) * 0.001f;
        s_.lpit = get(kLPitch) * 0.01f; s_.lflt = get(kLFilter) * 0.01f; s_.lamp = get(kLAmp) * 0.01f;
        s_.chr = getInt(kChar); s_.bits = getInt(kBits); s_.drive = get(kDrive) * 0.01f;
        s_.choLevel = get(kChoLevel) * 0.01f; s_.choRate = get(kChoRate);
        s_.dTime = get(kDlyTime); s_.dFb = get(kDlyFb) * 0.01f; s_.dMix = get(kDlyMix) * 0.01f;
        s_.revLevel = get(kRevLevel) * 0.01f;
        reverb_.set(get(kRevTime), 0.4f);
        volume_.setTarget(dbToGain(get(kVolume)));
        for (auto& v : voices_)
            if (v.active()) { v.fenv.set(s_.env[0], s_.env[1], s_.env[2] * 0.01f, s_.env[3]); v.amp.set(s_.env[4], s_.env[5], s_.env[6] * 0.01f, s_.env[7]); }
        // character: smoothing filter and engine hold rate
        static const float smoothHz[4] = {0.0f, 15000.0f, 11000.0f, 7500.0f};
        static const float holdHz[4] = {0.0f, 32000.0f, 26000.0f, 20000.0f};
        if (s_.chr != lastChr_)
        {
            lastChr_ = s_.chr;
            if (s_.chr > 0) smooth_.setCoeffs(BiquadCoeffs::lowpass(sr_, std::min(smoothHz[s_.chr], 0.45f * (float)sr_), 0.7071f));
            smooth_.reset();
        }
        holdInc_ = s_.chr > 0 && holdHz[s_.chr] < sr_ ? holdHz[s_.chr] / (float)sr_ : 1.0f;
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
        if (!cur_ || cur_->zones.empty()) return;
        const float from = lastNote_ < 0 ? (float)note : lastNote_;
        if (s_.vmode == 1)
        {
            if (held_ < 16) heldKeys_[held_++] = note;
            Voice& v = voices_[0];
            if (s_.glide > 0.0f && v.active() && v.held && !v.slice && s_.slices == 0)
            {   // legato glide: keep playing, move pitch
                v.note = note; v.target = (float)note; lastNote_ = (float)note;
                return;
            }
            if (start(v, note, velocity)) { v.pitch = s_.glide > 0.0f ? from : (float)note; lastNote_ = (float)note; }
            return;
        }
        // retrigger the same slice/one-shot: cut the old one (like a sampler's pad)
        Voice* use = nullptr;
        if (s_.slices > 0 || s_.trig == 1) for (auto& v : voices_) if (v.active() && v.note == note) { use = &v; break; }
        Voice& v = use ? *use : allocate();
        if (start(v, note, velocity)) { v.pitch = s_.glide > 0.0f ? from : (float)note; lastNote_ = (float)note; }
    }

    void noteOff(int note)
    {
        if (s_.vmode == 1)
        {
            int w = 0;
            for (int i = 0; i < held_; ++i) if (heldKeys_[i] != note) heldKeys_[w++] = heldKeys_[i];
            held_ = w;
            Voice& v = voices_[0];
            if (v.note == note && held_ > 0 && s_.glide > 0.0f && v.active() && !v.slice)
            {
                v.note = heldKeys_[held_ - 1]; v.target = (float)v.note; lastNote_ = v.target;
                return;
            }
        }
        if (s_.trig == 1) return;   // one-shot: ignore note-off
        for (auto& v : voices_)
            if (v.held && v.note == note) { v.held = false; if (sustain_) v.sustained = true; else release(v); }
    }

    bool start(Voice& v, int note, int velocity)
    {
        const Zone* z = nullptr;
        double rs = 0, re = 0;
        bool slice = false;
        if (s_.slices > 0)
        {
            const int k = note - s_.sliceKey;
            if (k < 0 || k >= s_.slices) return false;
            z = &cur_->zones[0];
            rs = (double)z->frames * k / s_.slices;
            re = (double)z->frames * (k + 1) / s_.slices;
            slice = true;
        }
        else
        {
            for (const auto& c : cur_->zones) if (note >= c.lo && note <= c.hi) { z = &c; break; }
            if (!z) return false;
            rs = 0; re = (double)z->frames;
        }
        const bool wasActive = v.active();
        v.z = z; v.note = note; v.held = true; v.sustained = false; v.slice = slice; v.done = false;
        v.age = ++age_;
        v.vel = (float)velocity / 127.0f;
        v.target = (float)note; v.time = 0.0f;
        v.gain = lerp(1.0f, v.vel * v.vel, s_.velAmp);
        v.mode = s_.loop;
        v.start = rs; v.end = re;
        if (!slice && z->looped) { v.ls = std::max(rs, z->ls); v.le = std::min(re, z->le); }
        else { v.ls = rs; v.le = re; }
        if (v.le - v.ls < 8) { v.ls = rs; v.le = re; }
        const double offs = std::min(0.95, (double)(s_.start + s_.velStart * (1.0f - v.vel))) * (re - rs);
        if (v.mode == 3) { v.pos = re - 1.0 - offs; v.dir = -1; }
        else { v.pos = rs + offs; v.dir = 1; }
        if (!wasActive) { v.amp.reset(); v.fenv.reset(); for (auto& f : v.f) f.reset(); }
        v.fenv.set(s_.env[0], s_.env[1], s_.env[2] * 0.01f, s_.env[3]);
        v.amp.set(std::max(s_.env[4], wasActive ? 1.5f : 0.5f), s_.env[5], s_.env[6] * 0.01f, s_.env[7]);
        v.amp.noteOn(); v.fenv.noteOn();
        return true;
    }

    void release(Voice& v)
    {
        v.held = v.sustained = false;
        v.amp.noteOff(); v.fenv.noteOff();
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

    // Read one frame at pos with the Character's interpolation.
    inline void readFrame(const Zone& z, double pos, float& l, float& r) const
    {
        const long n = (long)z.frames;
        long k = (long)std::floor(pos);
        const float f = (float)(pos - (double)k);
        auto at = [&](const std::vector<float>& d, long i) { return d[(size_t)(i < 0 ? 0 : (i >= n ? n - 1 : i))]; };
        const bool stereo = !z.r.empty();
        if (s_.chr >= 2)   // drop-sample: no interpolation
        {
            l = at(z.l, k); r = stereo ? at(z.r, k) : l;
        }
        else if (s_.chr == 1)   // linear
        {
            l = at(z.l, k) + f * (at(z.l, k + 1) - at(z.l, k));
            r = stereo ? at(z.r, k) + f * (at(z.r, k + 1) - at(z.r, k)) : l;
        }
        else   // cubic Hermite
        {
            auto herm = [&](const std::vector<float>& d) {
                const float xm1 = at(d, k - 1), x0 = at(d, k), x1 = at(d, k + 1), x2 = at(d, k + 2);
                const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
                return ((c3 * f + c2) * f + c1) * f + x0; };
            l = herm(z.l); r = stereo ? herm(z.r) : l;
        }
    }

    void renderChunk(int off, int n)
    {
        controllers_.tick();
        const float dtc = (float)kCtl / (float)sr_;
        const float bendSemis = controllers_.pitchBend() * s_.bend;
        const float lfoRaw = lfo(s_.lrate * dtc);
        const float glide = s_.glide > 0.0f ? std::exp(-dtc / (s_.glide * 0.001f * 0.4f)) : 0.0f;
        const float panL = std::cos((s_.pan + 1.0f) * 0.25f * kPiF) * 1.4142f, panR = std::sin((s_.pan + 1.0f) * 0.25f * kPiF) * 1.4142f;
        const int fm = s_.fmode;
        for (auto& v : voices_)
        {
            if (!v.active()) continue;
            const Zone& z = *v.z;
            v.pitch = v.target + (v.pitch - v.target) * glide;
            const float fade = s_.ldelay > 0.0f ? mpc::clamp((v.time - s_.ldelay * 0.5f) / (s_.ldelay * 0.5f), 0.0f, 1.0f) : 1.0f;
            const float lf = lfoRaw * fade;
            const float penv = s_.penv != 0.0f ? s_.penv * std::exp(-v.time / std::max(0.001f, s_.pdec)) : 0.0f;
            const float keySemis = (s_.keyTrack && !v.slice) ? v.pitch - (float)z.root : 0.0f;
            const float semis = keySemis + s_.transpose + bendSemis + penv + lf * s_.lpit;
            const double inc = std::pow(2.0, (double)semis / 12.0) * z.rate / sr_;
            if (fm > 0)
            {
                const float oct = s_.fenv * v.fenv.level() * 8.0f + s_.keyF * (v.pitch - 60.0f) / 12.0f + s_.velF * (v.vel - 1.0f) * 4.0f + lf * s_.lflt * 3.0f;
                const float hz = mpc::clamp(s_.cutoff * std::pow(2.0f, mpc::clamp(oct, -10.0f, 10.0f)), 20.0f, 20000.0f);
                v.f[0].set(hz, fm == 1 || fm == 5 ? 0.0f : s_.reso); v.f[1].set(hz, s_.reso);
                v.f[2].set(hz, fm == 1 || fm == 5 ? 0.0f : s_.reso); v.f[3].set(hz, s_.reso);
            }
            const float trem = 1.0f - s_.lamp * 0.5f * (1.0f - lf);
            const float g = v.gain * trem * 0.5f;
            float* bl = bufL_ + off;
            float* br = bufR_ + off;
            for (int i = 0; i < n; ++i)
            {
                // advance position with the loop mode
                if (v.dir > 0)
                {
                    const bool loopNow = (v.mode == 1 || v.mode == 2 || (v.mode == 4 && (v.held || v.sustained)));
                    if (loopNow && v.pos >= v.le)
                    {
                        if (v.mode == 2) { v.pos = v.le - (v.pos - v.le) - 1.0; v.dir = -1; }
                        else v.pos -= (v.le - v.ls);
                        if (v.pos < v.ls) v.pos = v.ls;
                    }
                    else if (!loopNow && v.pos >= v.end - 1.0) { v.done = true; break; }
                }
                else
                {
                    if (v.mode == 2 && v.pos < v.ls) { v.pos = v.ls + (v.ls - v.pos); v.dir = 1; }
                    else if (v.pos < v.start) { v.done = true; break; }
                }
                float l, r;
                readFrame(z, v.pos, l, r);
                // short fade at the region's end so slices and one-shots never click
                const double remain = v.dir > 0 ? v.end - v.pos : v.pos - v.start;
                const bool looping = v.mode == 1 || v.mode == 2 || (v.mode == 4 && (v.held || v.sustained));
                if (!looping && remain < 64.0 * inc + 1.0) { const float k = (float)(remain / (64.0 * inc + 1.0)); l *= k; r *= k; }
                v.pos += inc * v.dir;
                v.fenv.process();
                if (fm > 0)
                {
                    const int m = fm <= 2 ? 0 : fm == 3 ? 1 : 2;
                    l = v.f[0].process(l, m); r = v.f[2].process(r, m);
                    if (fm == 1 || fm == 5) { l = v.f[1].process(l, m); r = v.f[3].process(r, m); }
                }
                const float a = v.amp.process() * g;
                bl[i] += l * a * panL;
                br[i] += r * a * panR;
            }
            if (!v.amp.isActive() || (v.amp.stage() == ADSR::Stage::Sustain && v.amp.level() < 1.0e-4f)) v.done = true;
            if (v.done) v.stop();
            else v.time += dtc;
        }
    }

    void output(float* L, float* R, int n)
    {
        const float drive = 1.0f + s_.drive * 8.0f, dnorm = 1.0f / fastTanh(drive);
        const bool crush = s_.bits < 16 || s_.chr >= 2;
        const float q = (float)(1 << ((s_.chr >= 2 && s_.bits > 12 ? 12 : s_.bits) - 1));
        const float choInc = s_.choRate / (float)sr_;
        const float base = 0.010f * (float)sr_, depth = 0.005f * (float)sr_;
        const float dlen = s_.dTime * 0.001f * (float)sr_;
        for (int i = 0; i < n; ++i)
        {
            float l = bufL_[i], r = bufR_[i];
            // CHARACTER: engine-rate hold, bit depth, smoothing filter, drive
            if (holdInc_ < 1.0f)
            {
                holdPh_ += holdInc_;
                if (holdPh_ >= 1.0f) { holdPh_ -= 1.0f; holdL_ = l; holdR_ = r; }
                l = holdL_; r = holdR_;
            }
            if (crush) { l = std::round(mpc::clamp(l, -1.0f, 1.0f) * q) / q; r = std::round(mpc::clamp(r, -1.0f, 1.0f) * q) / q; }
            if (s_.chr > 0) { smooth_.process(l, r); if (std::fabs(l) < 1.0e-20f) l = 0.0f; if (std::fabs(r) < 1.0e-20f) r = 0.0f; }
            if (s_.drive > 0.0f) { l = fastTanh(l * drive) * dnorm; r = fastTanh(r * drive) * dnorm; }
            // chorus
            if (s_.choLevel > 0.0f)
            {
                choL_.write((l + r) * 0.5f);
                choPh_ += choInc; if (choPh_ >= 1.0f) choPh_ -= 1.0f;
                const float wl = choL_.readLinear(base + depth * std::sin(2.0f * kPiF * choPh_));
                const float wr = choL_.readLinear(base + depth * std::cos(2.0f * kPiF * choPh_));
                l = l * (1.0f - 0.4f * s_.choLevel) + wl * s_.choLevel;
                r = r * (1.0f - 0.4f * s_.choLevel) + wr * s_.choLevel;
            }
            else choL_.write((l + r) * 0.5f);
            // ping-pong delay
            const float tl = dlyL_.readLinear(dlen), tr = dlyR_.readLinear(dlen);
            dlyL_.write((s_.dMix > 0.0f ? (l + r) * 0.5f : 0.0f) + tr * s_.dFb);
            dlyR_.write(tl * s_.dFb);
            l += tl * s_.dMix; r += tr * s_.dMix;
            // reverb
            float rvL, rvR;
            reverb_.process(s_.revLevel > 0.0f ? l : 0.0f, s_.revLevel > 0.0f ? r : 0.0f, rvL, rvR);
            l += rvL * s_.revLevel; r += rvR * s_.revLevel;
            const float gv = volume_.process();
            l = softClip(l * gv); r = softClip(r * gv);
            if (std::fabs(l) < 1.0e-20f) l = 0.0f;   // tiny filter/reverb tails
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

    // ---- set-ups (the sound is whatever is selected; these set the engine) -------------
    void makePrograms()
    {
        auto program = [&](const char* name, std::initializer_list<std::pair<int, float>> o)
        {
            std::vector<float> v((size_t)kSound);   // everything except Sound
            for (int i = 0; i < kSound; ++i) v[(size_t)i] = param(i).def;
            for (const auto& p : o) v[(size_t)p.first] = p.second;
            addProgram(name, std::move(v));
        };
        program("Keys Vintage", {{kChar, 1}});
        program("Chop 16 Pads", {{kSlices, 4}, {kTrigger, 1}, {kChar, 2}, {kSliceKey, 36}});
        program("Chop 8 Gate", {{kSlices, 3}, {kTrigger, 0}, {kChar, 1}, {kR, 30}});
        program("Loop Crunch", {{kLoopMode, 1}, {kChar, 2}, {kKeyTrack, 1}});
        program("Dusty Filtered", {{kChar, 3}, {kFMode, 1}, {kCutoff, 2500}, {kDrive, 25}});
        program("Pitched Down Dirt", {{kChar, 2}, {kTranspose, -5}, {kBits, 12}, {kFMode, 2}, {kCutoff, 6000}});
        program("Reverse Swell", {{kLoopMode, 3}, {kA, 300}, {kRevLevel, 35}, {kChar, 1}});
        program("Pad Loop Wide", {{kLoopMode, 2}, {kA, 400}, {kR, 1200}, {kChoLevel, 45}, {kRevLevel, 30}, {kChar, 1}});
        program("Mono Bass Glide", {{kVoiceMode, 1}, {kGlide, 80}, {kFMode, 1}, {kCutoff, 900}, {kFEnv, 40}, {kFD, 250}, {kChar, 2}});
        program("Clean Hi-Fi", {{kChar, 0}});
    }

    std::vector<Entry> entries_;
    std::thread worker_;
    std::atomic<bool> quit_{false};
    std::atomic<int> request_{-1};
    std::atomic<Sound*> pending_{nullptr}, retired_{nullptr};
    Sound* cur_ = nullptr;
    int requested_ = -1;

    double sr_ = 44100.0;
    Voice voices_[kVoices];
    unsigned age_ = 0;
    bool sustain_ = false;
    int heldKeys_[16] = {}, held_ = 0;
    float lastNote_ = -1.0f, lfoPh_ = 0.0f, rnd_ = 0.0f;
    uint32_t rng_ = 987654321u;
    int lastChr_ = -1;
    float holdInc_ = 1.0f, holdPh_ = 0.0f, holdL_ = 0.0f, holdR_ = 0.0f;
    StereoBiquad smooth_;
    float bufL_[kMaxBlock], bufR_[kMaxBlock];
    DelayLine choL_, dlyL_, dlyR_;
    Reverb reverb_;
    float choPh_ = 0.0f;
    Smoother volume_;
    MidiControllers controllers_;
};

MPC_PLUGIN_ENTRY(MeltCrate)
