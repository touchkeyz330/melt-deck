// FontBox.cpp - a SoundFont (.sf2) player for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// Drop .sf2 files into Content/SoundFonts/ (in the project before building, or straight into
// the plugin folder on the MPC drive). Pick one with File and a preset with Preset; the preset
// list is read when the plugin loads, the audio when you choose a file.
//
// Per voice: SF2 zone (cubic interpolation, loop modes, root key/tuning/scale tuning)
//   -> low-pass filter (zone cutoff/Q + Brightness/Resonance) -> DAHDSR volume envelope
//   -> velocity curve and zone attenuation -> equal-power pan -> stereo mix
//   -> Bits (bit depth) -> Volume.
// Layers (several zones on one key/velocity) each get a voice. 32 voices.
//
// Memory: only the selected file's samples are in RAM (16-bit). Changing File loads in the
// background and swaps in when ready; notes playing at that moment are stopped. Files over
// kMaxFontBytes are refused (Font Box then plays a simple built-in tone).

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/svf.h"
#include "dsp/smoother.h"
#include "dsp/midi_cc.h"
#include "sf2.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <dirent.h>
#endif

using namespace mpc::dsp;

static constexpr uint64_t kMaxFontBytes = 512ull * 1024 * 1024;

// ---------------------------------------------------------------------------
// Files and presets (metadata read in the constructor)
// ---------------------------------------------------------------------------
struct FontFile
{
    std::string name, path;
    sf2::Hydra hydra;
    std::vector<sf2::PresetInfo> presets;
};

static std::vector<std::string> listSf2(const std::string& dir)
{
    std::vector<std::string> out;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "/*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do { out.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    if (DIR* d = opendir(dir.c_str()))
    {
        while (dirent* e = readdir(d)) out.push_back(e->d_name);
        closedir(d);
    }
#endif
    std::vector<std::string> keep;
    for (const auto& n : out)
        if (n.size() > 4 && n[0] != '.')
        {
            std::string ext = n.substr(n.size() - 4);
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            if (ext == ".sf2") keep.push_back(n);
        }
    std::sort(keep.begin(), keep.end());
    return keep;
}

static std::vector<FontFile> scanFonts()
{
    std::vector<FontFile> fonts;
    const std::string dir = mpc::contentPath("SoundFonts");
    for (const auto& n : listSf2(dir))
    {
        FontFile f;
        f.name = n.substr(0, n.size() - 4);
        f.path = dir + "/" + n;
        if (!sf2::readHydra(f.path, f.hydra)) { mpc::log("FontBox: not a usable SoundFont: %s", n.c_str()); continue; }
        f.presets = sf2::listPresets(f.hydra);
        if (f.presets.empty()) continue;
        if (f.hydra.smplBytes > kMaxFontBytes) f.name += " (too big)";
        fonts.push_back(std::move(f));
    }
    return fonts;
}

// ---------------------------------------------------------------------------
// Loaded data: one file's samples (shared) and one preset's zones
// ---------------------------------------------------------------------------
struct FontData { int file = -1; std::vector<int16_t> pcm; size_t points = 0; };

struct Program
{
    int key = -1;
    std::shared_ptr<const FontData> font;   // touched only on non-audio threads
    const int16_t* pcm = nullptr;           // what the audio thread reads
    size_t points = 0;
    std::vector<sf2::Zone> zones;
};

static Program* makeFallback(int key)
{
    // A looped one-cycle saw at 440 Hz, so Font Box still plays with no SoundFonts.
    auto fd = std::make_shared<FontData>();
    for (int i = 0; i < 100; ++i) fd->pcm.push_back((int16_t)(20000.0f * (2.0f * (float)i / 100.0f - 1.0f)));
    fd->points = fd->pcm.size();
    fd->pcm.resize(fd->points + 4, 0);
    Program* p = new Program();
    p->key = key; p->font = fd; p->pcm = fd->pcm.data(); p->points = fd->points;
    sf2::Zone z;
    z.start = 0; z.end = 100; z.loopStart = 0; z.loopEnd = 100; z.loopMode = 1; z.rate = 44000.0; z.rootKey = 69;
    z.release = 0.2f;
    p->zones.push_back(z);
    return p;
}

// ---------------------------------------------------------------------------
// The plugin
// ---------------------------------------------------------------------------
class FontBox : public mpc::Plugin
{
public:
    enum Params { kVolume, kFile, kPreset, kTranspose, kFine, kVelSens, kBendRange, kBright, kReso, kAttack, kRelease, kBits, kNumParams };
    static constexpr int kMaxPresets = 512;

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Font Box";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'V', 'h', 'd');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    FontBox() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        fonts_ = scanFonts();
        std::vector<std::string> names;
        for (const auto& f : fonts_) names.push_back(f.name);
        if (names.empty()) names.push_back("No SoundFonts");
        if (names.size() == 1) names.push_back("-");

        addParam(floatParam("vol",     "Volume", -40.0f, 6.0f, -6.0f, "dB", 1));
        addParam(choiceParam("file",   "File", names, 0));
        addParam(intParam  ("preset",  "Preset", 1, kMaxPresets, 1));
        addParam(intParam  ("transp",  "Transpose", -24, 24, 0, "st"));
        addParam(floatParam("fine",    "Fine", -50.0f, 50.0f, 0.0f, "ct", 0));
        addParam(floatParam("velsens", "Velocity", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(intParam  ("bend",    "Bend Range", 0, 24, 2, "st"));
        addParam(floatParam("bright",  "Brightness", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("reso",    "Resonance", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("atkscl",  "Attack", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("relscl",  "Release", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(intParam  ("bits",    "Bits", 4, 16, 16));
        if (numParams() != kNumParams) mpc::log("FontBox: parameter count mismatch");

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> o)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& p : o) v[(size_t)p.first] = p.second;
            addProgram(name, std::move(v));
        };
        program("Init", {});
        program("Darker", {{kBright, -40}, {kReso, 10}});
        program("Long Tails", {{kRelease, 60}});
        program("Snappy", {{kAttack, -60}, {kRelease, -50}});
        program("12-Bit", {{kBits, 12}, {kBright, -15}});

        worker_ = std::thread([this] { workerLoop(); });
    }

    ~FontBox() override
    {
        running_.store(false);
        if (worker_.joinable()) worker_.join();
        delete active_;
        delete pending_.exchange(nullptr);
        delete retired_.exchange(nullptr);
    }

    bool formatValue(int index, float plainValue, std::string& out) override
    {
        if (index != kPreset || fonts_.empty()) return false;
        const FontFile& f = fonts_[(size_t)mpc::clamp(getInt(kFile), 0, (int)fonts_.size() - 1)];
        const sf2::PresetInfo& p = f.presets[(size_t)mpc::clamp((int)std::lround(plainValue) - 1, 0, (int)f.presets.size() - 1)];
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d:%d %s", p.bank, p.program, p.name.c_str());
        out = buf;
        return true;
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        for (auto& v : voices_) v.filter.setSampleRate(sampleRate);
        controllers_.prepare(sampleRate, 10.0f);
        volume_.setTime(10.0f, sampleRate);
        volume_.snap(dbToGain(get(kVolume)));
        if (active_ == nullptr)
        {
            const int key = selectedKey();
            active_ = load(key, initialCache_);   // the worker takes over this cache (see workerLoop)
            requested_.store(key);
            current_.store(key);
            ready_.store(true);
        }
    }

    void reset() override
    {
        for (auto& v : voices_) v.active = false;
        sustain_ = false;
        controllers_.reset();
        bend_ = 0.0f;
        volume_.snap(dbToGain(get(kVolume)));
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int key = selectedKey();
        if (key != requested_.load(std::memory_order_relaxed)) requested_.store(key);
        if (retired_.load() == nullptr)
            if (Program* fresh = pending_.exchange(nullptr))
            {
                for (auto& v : voices_) v.active = false;
                retired_.store(active_);
                active_ = fresh;
            }

        // per-block settings
        tune_ = (float)getInt(kTranspose) + get(kFine) * 0.01f;
        velSens_ = get(kVelSens) * 0.01f;
        bendRange_ = (float)getInt(kBendRange);
        brightOct_ = get(kBright) * 0.05f;            // +-5 octaves
        resoAdd_ = get(kReso) * 0.01f;
        atkScale_ = std::pow(10.0f, get(kAttack) * 0.01f);
        relScale_ = std::pow(10.0f, get(kRelease) * 0.01f);
        const int bits = mpc::clamp(getInt(kBits), 4, 16);
        volume_.setTarget(dbToGain(get(kVolume)));

        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : scratchR_;
        std::memset(L, 0, sizeof(float) * (size_t)ctx.numFrames);
        std::memset(R, 0, sizeof(float) * (size_t)ctx.numFrames);

        int pos = 0;
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            if (ev.sampleOffset > pos) { render(L, R, pos, ev.sampleOffset); pos = ev.sampleOffset; }
            handleMidi(ev);
        }
        render(L, R, pos, ctx.numFrames);

        const float q = (float)(1 << (bits - 1));
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            const float g = volume_.process();
            float l = L[i], r = R[i];
            if (bits < 16) { l = std::floor(l * q + 0.5f) / q; r = std::floor(r * q + 0.5f) / q; }
            L[i] = sanitize(l * g);
            R[i] = sanitize(r * g);
        }
    }

private:
    static constexpr int kVoices = 32;
    static constexpr int kCtl = 16;
    static constexpr int kMaxBlock = 4096;
    static constexpr float kHeadroom = 0.5f;   // -6 dB inside: SoundFonts stack many layers and notes

    enum class Stage { Delay, Attack, Hold, Decay, Sustain, Release, Off };

    struct Voice
    {
        bool active = false;
        const sf2::Zone* z = nullptr;
        int note = -1;
        bool held = false, sustained = false;
        unsigned age = 0;
        double pos = 0.0, baseInc = 0.0;
        float pitch = 0.0f, inc = 0.0f;
        float gainL = 1.0f, gainR = 1.0f;
        bool looping = false;
        // envelope (computed every kCtl samples, ramped in between)
        Stage stage = Stage::Off;
        float stageTime = 0.0f;        // seconds spent in this stage
        float levelDb = -100.0f;       // decay/release work in dB
        float attackLin = 0.0f;
        float amp = 0.0f, ampStep = 0.0f;
        float fastRelease = 0.0f;      // > 0: exclusive-class or steal fade (seconds)
        int ctl = 0;
        bool filterOn = false;
        SVF filter;
    };

    int selectedKey() const
    {
        if (fonts_.empty()) return 0;
        const int f = mpc::clamp(getInt(kFile), 0, (int)fonts_.size() - 1);
        const int p = mpc::clamp(getInt(kPreset) - 1, 0, (int)fonts_[(size_t)f].presets.size() - 1);
        return f * 4096 + p;
    }

    // Runs on the worker (or in prepare). 'cache' keeps the last file's samples for reuse.
    Program* load(int key, std::shared_ptr<FontData>& cache)
    {
        const int f = key / 4096, p = key % 4096;
        if (fonts_.empty() || f < 0 || f >= (int)fonts_.size()) return makeFallback(key);
        const FontFile& ff = fonts_[(size_t)f];
        if (!cache || cache->file != f)
        {
            cache.reset();   // free the old file first: keeps peak memory down
            auto fd = std::make_shared<FontData>();
            fd->file = f;
            if (!sf2::readSamples(ff.path, ff.hydra, fd->pcm, kMaxFontBytes)) return makeFallback(key);
            fd->points = (size_t)(ff.hydra.smplBytes / 2);
            cache = fd;
        }
        if (p < 0 || p >= (int)ff.presets.size()) return makeFallback(key);
        Program* prog = new Program();
        prog->key = key;
        prog->font = cache;
        prog->pcm = cache->pcm.data();
        prog->points = cache->points;
        prog->zones = sf2::resolvePreset(ff.hydra, ff.presets[(size_t)p].index, cache->points);
        if (prog->zones.empty()) { delete prog; return makeFallback(key); }
        return prog;
    }

    void workerLoop()
    {
        std::shared_ptr<FontData> cache;
        bool seeded = false;
        while (running_.load())
        {
            delete retired_.exchange(nullptr);
            if (ready_.load())
            {
                if (!seeded) { cache = std::move(initialCache_); seeded = true; }   // ready_ orders this after prepare()
                const int want = requested_.load();
                if (want >= 0 && want != current_.load())
                {
                    Program* p = load(want, cache);
                    if (requested_.load() == want) { delete pending_.exchange(p); current_.store(want); }
                    else delete p;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }

    // ----- MIDI ------------------------------------------------------------
    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn()) noteOn(ev.note(), ev.velocity());
        else if (ev.isNoteOff())
        {
            for (auto& v : voices_)
                if (v.active && v.held && v.note == ev.note())
                {
                    v.held = false;
                    if (sustain_) v.sustained = true; else release(v);
                }
        }
        else if (ev.isAllNotesOff()) { for (auto& v : voices_) if (v.active) { v.held = v.sustained = false; release(v); } }
        else if (ev.isController() && ev.controller() == cc::Sustain)
        {
            sustain_ = ev.controllerValue() >= 64;
            if (!sustain_) for (auto& v : voices_) if (v.active && v.sustained) { v.sustained = false; release(v); }
        }
        else if (ev.isPitchBend()) bend_ = (float)ev.pitchBend() / 8192.0f;
        else controllers_.handle(ev);
    }

    void noteOn(int note, int vel)
    {
        if (active_ == nullptr) return;
        for (const sf2::Zone& z : active_->zones)
        {
            if (note < z.keyLo || note > z.keyHi || vel < z.velLo || vel > z.velHi) continue;
            if (z.exclusiveClass != 0)   // e.g. closed hi-hat cuts the open one
                for (auto& o : voices_)
                    if (o.active && o.z != nullptr && o.z->exclusiveClass == z.exclusiveClass && o.note != note) o.fastRelease = 0.005f, release(o);
            Voice& v = allocate();
            const int k = z.fixedKey >= 0 ? z.fixedKey : note;
            const int vv = z.fixedVel >= 0 ? z.fixedVel : vel;
            v.active = true; v.z = &z; v.note = note; v.held = true; v.sustained = false; v.age = ++ageCounter_;
            v.pos = (double)z.start;
            v.pitch = (float)(k - z.rootKey) * z.scaleTuning + z.tuneSemis;
            v.baseInc = z.rate / sr_;
            v.looping = z.loopMode != 0;
            // standard SF2 velocity curve (40*log10(vel/127) dB), scaled by Velocity
            const float velDb = 40.0f * std::log10(std::max(1, vv) / 127.0f) * velSens_;
            const float g = dbToGain(velDb - z.attenDb) * kHeadroom;
            const float a = (z.pan + 1.0f) * 0.25f * 3.14159265f;
            v.gainL = g * std::cos(a) * 1.41421356f;
            v.gainR = g * std::sin(a) * 1.41421356f;
            v.stage = z.delay > 0.002f ? Stage::Delay : Stage::Attack;
            v.stageTime = 0.0f; v.attackLin = 0.0f; v.levelDb = 0.0f; v.amp = 0.0f; v.ampStep = 0.0f;
            v.fastRelease = 0.0f; v.ctl = 0;
            v.filter.reset();
        }
    }

    Voice& allocate()
    {
        Voice* best = nullptr;
        for (auto& v : voices_) if (!v.active) return v;
        for (auto& v : voices_)   // steal: prefer released voices, then the oldest
        {
            const bool rel = v.stage == Stage::Release;
            if (best == nullptr || (rel && best->stage != Stage::Release) || (rel == (best->stage == Stage::Release) && v.age < best->age)) best = &v;
        }
        return *best;
    }

    void release(Voice& v)
    {
        if (v.stage == Stage::Off || v.stage == Stage::Release) { if (v.fastRelease > 0.0f) v.stageTime = 0.0f; return; }
        v.levelDb = v.amp > 1.0e-5f ? 20.0f * std::log10(v.amp) : -100.0f;
        v.stage = Stage::Release;
        v.stageTime = 0.0f;
        if (v.z != nullptr && v.z->loopMode == 3) v.looping = false;   // play out the tail after the loop
    }

    // Advance the envelope by dt seconds; returns the amplitude at the end of the step.
    float envelope(Voice& v, float dt)
    {
        const sf2::Zone& z = *v.z;
        v.stageTime += dt;
        switch (v.stage)
        {
            case Stage::Delay:
                if (v.stageTime >= z.delay) { v.stage = Stage::Attack; v.stageTime = 0.0f; }
                return 0.0f;
            case Stage::Attack:
            {
                const float t = std::max(0.001f, z.attack * atkScale_);
                v.attackLin = std::min(1.0f, v.stageTime / t);
                if (v.attackLin >= 1.0f) { v.stage = Stage::Hold; v.stageTime = 0.0f; }
                return v.attackLin;
            }
            case Stage::Hold:
                if (v.stageTime >= z.hold) { v.stage = Stage::Decay; v.stageTime = 0.0f; v.levelDb = 0.0f; }
                return 1.0f;
            case Stage::Decay:
                v.levelDb -= 100.0f * dt / std::max(0.001f, z.decay);
                if (v.levelDb <= -z.sustainDb) { v.levelDb = -z.sustainDb; v.stage = Stage::Sustain; }
                return dbToGain(v.levelDb);
            case Stage::Sustain:
                return dbToGain(v.levelDb);
            case Stage::Release:
            {
                const float t = v.fastRelease > 0.0f ? v.fastRelease : std::max(0.001f, z.release * relScale_);
                v.levelDb -= 100.0f * dt / t;
                if (v.levelDb <= -90.0f) { v.stage = Stage::Off; return 0.0f; }
                return dbToGain(v.levelDb);
            }
            case Stage::Off: default: return 0.0f;
        }
    }

    // ----- audio -----------------------------------------------------------
    static inline float at(const int16_t* d, long k, long lo, long hi)
    {
        k = k < lo ? lo : (k > hi ? hi : k);
        return (float)d[k] * (1.0f / 32768.0f);
    }

    void render(float* L, float* R, int from, int to)
    {
        if (active_ == nullptr) return;
        const int16_t* d = active_->pcm;
        const float bendSemis = bend_ * bendRange_;
        for (auto& v : voices_)
        {
            if (!v.active) continue;
            const sf2::Zone& z = *v.z;
            const long lo = (long)z.start, hi = (long)z.end - 1;
            const double loopLen = (double)z.loopEnd - (double)z.loopStart;
            for (int i = from; i < to; ++i)
            {
                if (v.ctl == 0)
                {
                    const float next = envelope(v, (float)kCtl / (float)sr_);
                    v.ampStep = (next - v.amp) / (float)kCtl;
                    v.inc = (float)(v.baseInc * semitonesToRatio(v.pitch + tune_ + bendSemis));
                    const float cutoff = z.filterHz * std::pow(2.0f, brightOct_);
                    const float reso = mpc::clamp(z.filterQDb / 24.0f + resoAdd_, 0.0f, 0.95f);
                    v.filterOn = cutoff < 19000.0f || reso > 0.01f;
                    if (v.filterOn) v.filter.set(mpc::clamp(cutoff, 20.0f, 20000.0f), reso);
                    if (v.stage == Stage::Off) { v.active = false; break; }
                }
                v.ctl = (v.ctl + 1) % kCtl;
                v.amp += v.ampStep;

                const long k = (long)v.pos;
                const float f = (float)(v.pos - (double)k);
                float xm1, x0, x1, x2;
                if (v.looping && k + 2 >= (long)z.loopEnd)
                {
                    auto w = [&](long j) { return j >= (long)z.loopEnd ? j - (long)loopLen : j; };
                    xm1 = at(d, w(k - 1), lo, hi); x0 = at(d, w(k), lo, hi); x1 = at(d, w(k + 1), lo, hi); x2 = at(d, w(k + 2), lo, hi);
                }
                else { xm1 = at(d, k - 1, lo, hi); x0 = at(d, k, lo, hi); x1 = at(d, k + 1, lo, hi); x2 = at(d, k + 2, lo, hi); }
                const float c1 = 0.5f * (x1 - xm1);
                const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
                const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
                float s = ((c3 * f + c2) * f + c1) * f + x0;
                if (v.filterOn) s = v.filter.process(s, 0);
                s *= v.amp;
                L[i] += s * v.gainL;
                R[i] += s * v.gainR;

                v.pos += (double)v.inc;
                if (v.looping) { while (v.pos >= (double)z.loopEnd) v.pos -= loopLen; }
                else if (v.pos >= (double)z.end - 1.0) { v.active = false; break; }
            }
        }
    }

    std::vector<FontFile> fonts_;
    double sr_ = 44100.0;

    Program* active_ = nullptr;
    std::atomic<Program*> pending_{nullptr};
    std::atomic<Program*> retired_{nullptr};
    std::atomic<int> requested_{-1};
    std::atomic<int> current_{-1};
    std::atomic<bool> ready_{false};
    std::atomic<bool> running_{true};
    std::thread worker_;
    std::shared_ptr<FontData> initialCache_;     // written in prepare() before ready_, then owned by the worker

    Voice voices_[kVoices];
    unsigned ageCounter_ = 0;
    bool sustain_ = false;
    float bend_ = 0.0f, bendRange_ = 2.0f, tune_ = 0.0f, velSens_ = 1.0f, brightOct_ = 0.0f, resoAdd_ = 0.0f, atkScale_ = 1.0f, relScale_ = 1.0f;
    MidiControllers controllers_;
    Smoother volume_;
    float scratchR_[kMaxBlock] = {0};
};

MPC_PLUGIN_ENTRY(FontBox)
