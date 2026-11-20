// Vault.cpp - a multisample "rompler" instrument for the MPC by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// The engine is open source. The SOUNDS are not part of it: Vault plays whatever WAV
// files you put in its Content/ folder, described by Content/vault.map (a plain text file
// made by tools/vault_mapper.py). Without a map it plays a built-in saw wave.
//
// Signal flow per voice:
//   sample zone (cubic interpolation, loop points, tune/bend/LFO pitch)
//   -> state-variable filter (cutoff + envelope + LFO + key track, drive)
//   -> amp ADSR * velocity -> summed to mono -> bit depth ("Bits") -> volume -> L + R
//
// Memory: only the selected sound is in RAM. Picking another Bank/Sound loads it on a
// background thread and swaps it in when ready (notes that were playing are stopped).
//
// Kits: sounds marked as kits map one sample per key from C1 (note 36) upward, play
// at their original pitch and ignore note-off (one-shot), like an MPC drum program.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/svf.h"
#include "dsp/adsr.h"
#include "dsp/lfo.h"
#include "dsp/smoother.h"
#include "dsp/voice.h"
#include "dsp/tempo.h"
#include "dsp/midi_cc.h"
#include "dsp/wav.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <sstream>
#include <thread>

using namespace mpc::dsp;

// ---------------------------------------------------------------------------
// Sound map (read once in the constructor; metadata only, no audio)
// ---------------------------------------------------------------------------
struct ZoneInfo { std::string file; float root = 60; int lo = 0, hi = 127; int loopStart = -1, loopEnd = -1; };
struct SoundInfo { std::string name; bool kit = false; std::vector<ZoneInfo> zones; };
struct BankInfo { std::string name, folder; std::vector<SoundInfo> sounds; };

static std::vector<BankInfo> readMap()
{
    std::vector<BankInfo> banks;
    std::string text;
    if (!mpc::readTextFile(mpc::contentPath("vault.map"), text)) return banks;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 3 || line[0] == '#' || line[1] != '\t') continue;
        std::vector<std::string> f;
        size_t p = 2, t;
        while ((t = line.find('\t', p)) != std::string::npos) { f.push_back(line.substr(p, t - p)); p = t + 1; }
        f.push_back(line.substr(p));
        if (line[0] == 'B' && f.size() >= 2) banks.push_back({f[0], f[1], {}});
        else if (line[0] == 'I' && f.size() >= 2 && !banks.empty()) banks.back().sounds.push_back({f[0], f[1] == "1", {}});
        else if (line[0] == 'Z' && f.size() >= 6 && !banks.empty() && !banks.back().sounds.empty())
        {
            ZoneInfo z;
            z.file = f[0]; z.root = std::strtof(f[1].c_str(), nullptr);
            z.lo = std::atoi(f[2].c_str()); z.hi = std::atoi(f[3].c_str());
            z.loopStart = std::atoi(f[4].c_str()); z.loopEnd = std::atoi(f[5].c_str());
            banks.back().sounds.back().zones.push_back(z);
        }
    }
    // drop empty entries so every Bank/Sound the user can pick is playable
    for (auto& b : banks)
    {
        std::vector<SoundInfo> keep;
        for (auto& s : b.sounds) if (!s.zones.empty()) keep.push_back(std::move(s));
        b.sounds = std::move(keep);
    }
    std::vector<BankInfo> keep;
    for (auto& b : banks) if (!b.sounds.empty()) keep.push_back(std::move(b));
    return keep;
}

// ---------------------------------------------------------------------------
// A loaded sound: audio for every zone plus a key -> zone table
// ---------------------------------------------------------------------------
struct Zone
{
    std::vector<float> data;    // mono, -1..1
    double rate = 44100.0;      // the file's sample rate
    float root = 60.0f;         // MIDI note (fractional = fine tuning) at which it plays unshifted
    int loopStart = -1, loopEnd = -1;   // inclusive; -1 = no loop
    bool hasLoop() const { return loopStart >= 0; }
};

struct Sound
{
    int key = -1;               // which Bank/Sound this is (bank * 4096 + sound)
    bool kit = false;
    std::vector<Zone> zones;
    int keymap[128];            // zone index per MIDI note, -1 = silent
};

static Sound* makeFallbackSound(int key)
{
    // One cycle of a saw, looped, at 440 Hz: the plugin still makes sound with no Content.
    Sound* s = new Sound();
    s->key = key;
    Zone z; z.rate = 44000.0; z.root = 69.0f;
    for (int i = 0; i < 100; ++i) z.data.push_back(0.6f * (2.0f * (float)i / 100.0f - 1.0f));
    z.loopStart = 0; z.loopEnd = 99;
    s->zones.push_back(std::move(z));
    for (int n = 0; n < 128; ++n) s->keymap[n] = 0;
    return s;
}

static Sound* loadSound(const std::vector<BankInfo>& banks, int bank, int sound, int key)
{
    if (bank < 0 || bank >= (int)banks.size()) return makeFallbackSound(key);
    const BankInfo& b = banks[(size_t)bank];
    if (sound < 0 || sound >= (int)b.sounds.size()) return makeFallbackSound(key);
    const SoundInfo& info = b.sounds[(size_t)sound];

    Sound* s = new Sound();
    s->key = key;
    s->kit = info.kit;
    std::vector<int> owner;   // map file zone -> loaded zone index
    for (const ZoneInfo& zi : info.zones)
    {
        dsp::WavData w;
        if (!dsp::loadWav(mpc::contentPath("Banks/" + b.folder + "/" + zi.file), w)) { owner.push_back(-1); continue; }
        Zone z;
        z.rate = w.sampleRate;
        z.root = zi.root;
        const size_t n = w.numFrames();
        z.data.resize(n);
        const int nc = w.numChannels();
        for (size_t i = 0; i < n; ++i)
        {
            float v = 0.0f;
            for (int c = 0; c < nc; ++c) v += w.channels[(size_t)c][i];
            z.data[i] = v / (float)nc;
        }
        if (zi.loopStart >= 0 && zi.loopEnd > zi.loopStart + 4)
        {
            z.loopStart = zi.loopStart;
            z.loopEnd = std::min(zi.loopEnd, (int)n - 1);
            if (z.loopEnd <= z.loopStart + 4) z.loopStart = z.loopEnd = -1;
        }
        owner.push_back((int)s->zones.size());
        s->zones.push_back(std::move(z));
    }
    if (s->zones.empty()) { delete s; return makeFallbackSound(key); }

    for (int n = 0; n < 128; ++n)
    {
        s->keymap[n] = -1;
        for (size_t i = 0; i < info.zones.size(); ++i)
            if (owner[i] >= 0 && n >= info.zones[i].lo && n <= info.zones[i].hi) { s->keymap[n] = owner[i]; break; }
        if (s->keymap[n] < 0 && !s->kit)   // a missing file left a gap: use the nearest zone
        {
            float best = 1e9f;
            for (size_t i = 0; i < s->zones.size(); ++i)
            {
                const float d = std::fabs(s->zones[i].root - (float)n);
                if (d < best) { best = d; s->keymap[n] = (int)i; }
            }
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// The plugin
// ---------------------------------------------------------------------------
class Vault : public mpc::Plugin
{
public:
    // Parameter order and ids are permanent once projects use the plugin. Append new ones at the end.
    enum Params
    {
        kVolume, kBank, kSound, kTune, kFine, kVelSens, kBendRange, kLoops, kStart, kBits,
        kCutoff, kReso, kFilterMode, kEnvAmount, kKeyTrack, kDrive, kFAttack, kFDecay, kFSustain, kFRelease,
        kAttack, kDecay, kSustain, kRelease,
        kLfoShape, kLfoRate, kLfoSync, kLfoDiv, kLfoPitch, kLfoFilter, kWheelVib,
        kNumParams
    };
    static constexpr int kMaxSounds = 512;   // Sound knob range; banks with fewer sounds clamp
#ifndef VAULT_DEFAULT_BANK
  #define VAULT_DEFAULT_BANK 0
#endif
#ifndef VAULT_DEFAULT_SOUND
  #define VAULT_DEFAULT_SOUND 0      // 0 = the first playable (non-kit) sound of the default bank
#endif
    static constexpr int kDefaultBank = VAULT_DEFAULT_BANK;
    int defaultSound() const
    {
        if (VAULT_DEFAULT_SOUND > 0) return VAULT_DEFAULT_SOUND;
        if (kDefaultBank < (int)banks_.size())
        {
            const auto& snd = banks_[(size_t)kDefaultBank].sounds;
            for (size_t i = 0; i < snd.size(); ++i) if (!snd[i].kit) return (int)i + 1;
        }
        return 1;
    }

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Vault";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'C', 'b', '9');   // must match "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Vault() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        banks_ = readMap();
        std::vector<std::string> bankNames;
        for (const auto& b : banks_) bankNames.push_back(b.name);
        if (bankNames.empty()) bankNames.push_back("No Sounds");
        if (bankNames.size() == 1) bankNames.push_back("-");   // a choice needs two entries

        // SOUND page
        addParam(floatParam("vol",     "Volume", -40.0f, 6.0f, -6.0f, "dB", 1));
        addParam(choiceParam("bank",   "Bank", bankNames, kDefaultBank));
        addParam(intParam  ("sound",   "Sound", 1, kMaxSounds, defaultSound()));
        addParam(intParam  ("tune",    "Tune", -24, 24, 0, "st"));
        addParam(floatParam("fine",    "Fine", -50.0f, 50.0f, 0.0f, "ct", 0));
        addParam(floatParam("velsens", "Velocity", 0.0f, 100.0f, 70.0f, "%", 0));
        addParam(intParam  ("bend",    "Bend Range", 0, 24, 2, "st"));
        addParam(choiceParam("loops",  "Loops", {"On", "Off"}, 0));
        addParam(floatParam("start",   "Start", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(intParam  ("bits",    "Bits", 4, 16, 16));
        // FILTER page
        addParam(logParam  ("cutoff",  "Cutoff", 20.0f, 20000.0f, 20000.0f, "Hz", 0));
        addParam(floatParam("reso",    "Resonance", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(choiceParam("fmode",  "Filter Mode", {"LP", "BP", "HP"}, 0));
        addParam(floatParam("envamt",  "Env Amount", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("keytrk",  "Key Track", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("drive",   "Drive", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam  ("fatk",    "Filt Attack", 1.0f, 5000.0f, 1.0f, "ms", 0));
        addParam(logParam  ("fdec",    "Filt Decay", 1.0f, 5000.0f, 400.0f, "ms", 0));
        addParam(floatParam("fsus",    "Filt Sustain", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam  ("frel",    "Filt Release", 1.0f, 5000.0f, 300.0f, "ms", 0));
        // AMP page
        addParam(logParam  ("atk",     "Attack", 1.0f, 5000.0f, 1.0f, "ms", 0));
        addParam(logParam  ("dec",     "Decay", 1.0f, 10000.0f, 1000.0f, "ms", 0));
        addParam(floatParam("sus",     "Sustain", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(logParam  ("rel",     "Release", 1.0f, 5000.0f, 200.0f, "ms", 0));
        // LFO page
        addParam(choiceParam("lfoshape", "LFO Shape", LFO::shapeNames(), 0));
        addParam(logParam  ("lforate", "LFO Rate", 0.05f, 30.0f, 5.0f, "Hz", 2));
        addParam(boolParam ("lfosync", "LFO Sync", false));
        addParam(choiceParam("lfodiv", "LFO Div", divisionNames(), kDivisionQuarter));
        addParam(floatParam("lfopitch", "LFO>Pitch", 0.0f, 12.0f, 0.0f, "st", 2, 0.5f));
        addParam(floatParam("lfofilt", "LFO>Filter", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam("wheelvib", "Wheel Vib", 0.0f, 12.0f, 0.5f, "st", 2, 0.5f));

        if (numParams() != kNumParams) mpc::log("Vault: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        program("Init", {});
        program("Soft Pad", {{kAttack, 600}, {kRelease, 1500}, {kCutoff, 3500}, {kReso, 10}, {kLfoFilter, 15}, {kLfoRate, 0.3f}});
        program("Pluck", {{kCutoff, 700}, {kEnvAmount, 70}, {kKeyTrack, 80}, {kFDecay, 200}, {kDecay, 500}, {kSustain, 0}, {kRelease, 150}});
        program("12-Bit Grit", {{kBits, 12}, {kCutoff, 9000}, {kDrive, 25}});
        program("Lo-Fi 8-Bit", {{kBits, 8}, {kCutoff, 5000}, {kDrive, 40}});

        worker_ = std::thread([this] { workerLoop(); });
    }

    ~Vault() override
    {
        running_.store(false);
        if (worker_.joinable()) worker_.join();
        delete active_;
        delete pending_.exchange(nullptr);
        delete retired_.exchange(nullptr);
    }

    // Value text for Sound shows the sound's name in the selected bank.
    bool formatValue(int index, float plainValue, std::string& out) override
    {
        if (index != kSound || banks_.empty()) return false;
        const BankInfo& b = banks_[(size_t)mpc::clamp(getInt(kBank), 0, (int)banks_.size() - 1)];
        const int s = mpc::clamp((int)std::lround(plainValue) - 1, 0, (int)b.sounds.size() - 1);
        out = b.sounds[(size_t)s].name;
        return true;
    }

    // ----- lifecycle --------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = sampleRate;
        for (auto& v : voices_) { v.filter.setSampleRate(sampleRate); v.ampEnv.setSampleRate(sampleRate); v.filtEnv.setSampleRate(sampleRate); }
        lfo_.setSampleRate(sampleRate);
        controllers_.prepare(sampleRate, 10.0f);
        volume_.setTime(10.0f, sampleRate);
        volume_.snap(dbToGain(get(kVolume)));
        // First use: load the selected sound right here (prepare runs off the audio thread),
        // so the very first note already has it.
        if (active_ == nullptr)
        {
            const int key = selectedKey();
            active_ = loadSound(banks_, key / 4096, key % 4096, key);
            requested_.store(key);
            current_.store(key);
            ready_.store(true);
        }
    }

    void reset() override
    {
        for (auto& v : voices_) v.stop();
        alloc_.reset();
        lfo_.reset();
        controllers_.reset();
        volume_.snap(dbToGain(get(kVolume)));
    }

    // ----- audio ------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        // Ask the worker for a different sound if the user picked one; take it once it is ready.
        const int key = selectedKey();
        if (key != requested_.load(std::memory_order_relaxed)) requested_.store(key);
        if (retired_.load() == nullptr)
        {
            if (Sound* fresh = pending_.exchange(nullptr))
            {
                for (int vi = 0; vi < kNumVoices; ++vi) { voices_[vi].stop(); alloc_.voiceFinished(vi); }
                retired_.store(active_);   // the worker frees it: no deleting on the audio thread
                active_ = fresh;
            }
        }

        updateBlockSettings(ctx);

        int pos = 0;
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            if (ev.sampleOffset > pos) { render(ctx, pos, ev.sampleOffset); pos = ev.sampleOffset; }
            handleMidi(ev);
        }
        render(ctx, pos, ctx.numFrames);

        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        const bool crush = s_.bits < 16;
        const float q = (float)(1 << (s_.bits - 1));
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            float x = L[i];
            if (crush) x = std::floor(x * q + 0.5f) / q;
            x *= volume_.process();
            L[i] = x;
            if (R) R[i] = x;
        }
    }

private:
    static constexpr int kNumVoices = 16;
    static constexpr int kChunk = 64;
    static constexpr int kCtrlInterval = 16;

    struct Voice
    {
        const Zone* zone = nullptr;
        double pos = 0.0;
        double baseInc = 0.0;    // playback step at the zone's root pitch, before tune/bend/LFO
        float  pitch = 0.0f;     // semitones away from the zone root (note - root)
        float  inc = 0.0f;
        bool   looping = false;
        bool   oneShot = false;
        int    note = -1;
        float  velGain = 1.0f;
        int    ctrl = 0;
        SVF filter;
        ADSR ampEnv, filtEnv;
        bool active() const { return zone != nullptr && ampEnv.isActive(); }
        void stop() { zone = nullptr; ampEnv.reset(); filtEnv.reset(); filter.reset(); note = -1; }
    };

    struct Settings
    {
        float tune = 0, velSens = 0.7f, start = 0, cutoff = 20000, reso = 0, envAmt = 0, keyTrack = 0, drive = 0;
        float lfoPitch = 0, lfoFilter = 0, wheelVib = 0;
        int mode = 0, bits = 16;
        bool loops = true, filterOn = false;
    } s_;

    int selectedKey() const
    {
        if (banks_.empty()) return 0;
        const int b = mpc::clamp(getInt(kBank), 0, (int)banks_.size() - 1);
        const int s = mpc::clamp(getInt(kSound) - 1, 0, (int)banks_[(size_t)b].sounds.size() - 1);
        return b * 4096 + s;
    }

    void workerLoop()
    {
        while (running_.load())
        {
            delete retired_.exchange(nullptr);
            const int want = requested_.load();
            if (ready_.load() && want >= 0 && want != current_.load())
            {
                Sound* s = loadSound(banks_, want / 4096, want % 4096, want);
                if (requested_.load() == want) { delete pending_.exchange(s); current_.store(want); }   // drops an unused older load
                else delete s;                                                                         // user moved on: load the newer one
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }

    void updateBlockSettings(mpc::ProcessContext& ctx)
    {
        s_.tune = (float)getInt(kTune) + get(kFine) * 0.01f;
        s_.velSens = get(kVelSens) * 0.01f;
        s_.loops = getInt(kLoops) == 0;
        s_.start = get(kStart) * 0.01f;
        s_.bits = mpc::clamp(getInt(kBits), 4, 16);
        s_.cutoff = get(kCutoff); s_.reso = get(kReso) * 0.01f; s_.mode = getInt(kFilterMode);
        s_.envAmt = get(kEnvAmount) * 0.01f; s_.keyTrack = get(kKeyTrack) * 0.01f; s_.drive = get(kDrive) * 0.01f;
        s_.lfoPitch = get(kLfoPitch); s_.lfoFilter = get(kLfoFilter) * 0.01f; s_.wheelVib = get(kWheelVib);
        // Skip the filter entirely when it would not change the sound (saves CPU on the MPC).
        s_.filterOn = !(s_.mode == 0 && s_.cutoff >= 19500.0f && s_.envAmt == 0.0f && s_.lfoFilter == 0.0f && s_.drive == 0.0f);

        alloc_.setBendRange((float)getInt(kBendRange));
        volume_.setTarget(dbToGain(get(kVolume)));

        lfo_.setShape(getInt(kLfoShape));
        if (getBool(kLfoSync))
        {
            lfo_.setTempoSync(ctx.transport.bpm, divisionBeats(getInt(kLfoDiv)));
            if (ctx.transport.playing && ctx.transport.ppqValid) lfo_.syncToPpq(ctx.transport.ppqPos);
        }
        else lfo_.setFrequency(get(kLfoRate));

        const float fa = get(kFAttack), fd = get(kFDecay), fs = get(kFSustain) * 0.01f, fr = get(kFRelease);
        const float aa = get(kAttack),  ad = get(kDecay),  as = get(kSustain) * 0.01f,  ar = get(kRelease);
        for (auto& v : voices_)
        {
            v.ampEnv.set(aa, ad, as, ar);
            v.filtEnv.set(fa, fd, fs, fr);
            v.filter.setDrive(1.0f + s_.drive * 9.0f);
        }
    }

    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn())
        {
            if (active_ == nullptr) return;
            const int zi = active_->keymap[ev.note() & 127];
            if (zi < 0) return;                       // kit key with no sample
            const int vi = alloc_.noteOn(ev.note(), ev.velocity());
            Voice& v = voices_[vi];
            const Zone& z = active_->zones[(size_t)zi];
            v.zone = &z;
            v.note = ev.note();
            v.pitch = active_->kit ? 0.0f : (float)ev.note() - z.root;
            v.baseInc = z.rate / sr_;
            v.looping = s_.loops && z.hasLoop();
            v.oneShot = active_->kit && !v.looping;
            const double startLimit = v.looping ? (double)z.loopStart : (double)z.data.size() * 0.9;
            v.pos = s_.start * startLimit;
            v.velGain = lerp(1.0f, ev.velocity() / 127.0f, s_.velSens);
            v.filter.reset();
            v.ampEnv.reset(); v.filtEnv.reset();
            v.ampEnv.noteOn(); v.filtEnv.noteOn();
            v.ctrl = 0;
        }
        else if (ev.isNoteOff())
        {
            const int vi = alloc_.noteOff(ev.note());
            if (vi >= 0 && !voices_[vi].oneShot) releaseVoice(vi);
        }
        else if (ev.isAllNotesOff())
        {
            alloc_.allNotesOff([this](int vi) { releaseVoice(vi); });
        }
        else if (ev.isController() && ev.controller() == cc::Sustain)
        {
            controllers_.handle(ev);
            alloc_.setSustain(ev.controllerValue() >= 64, [this](int vi) { if (!voices_[vi].oneShot) releaseVoice(vi); });
        }
        else if (ev.isPitchBend()) alloc_.setPitchBend(ev.pitchBend());
        else controllers_.handle(ev);
    }

    void releaseVoice(int vi) { voices_[vi].ampEnv.noteOff(); voices_[vi].filtEnv.noteOff(); }

    void render(mpc::ProcessContext& ctx, int from, int to)
    {
        float* out = ctx.outputs[0];
        while (from < to)
        {
            const int n = (to - from) < kChunk ? (to - from) : kChunk;
            for (int i = 0; i < n; ++i) { controllers_.tick(); lfoBuf_[i] = lfo_.process(); }
            const float vib = s_.lfoPitch + s_.wheelVib * controllers_.modWheel();
            for (int vi = 0; vi < kNumVoices; ++vi)
            {
                Voice& v = voices_[vi];
                if (v.zone == nullptr) continue;
                renderVoice(v, out + from, n, vib);
                if (!v.active()) { v.stop(); alloc_.voiceFinished(vi); }
            }
            from += n;
        }
    }

    // Sample k of the zone, following the loop when the voice is looping.
    static inline float at(const Zone& z, long k, bool looping)
    {
        const long n = (long)z.data.size();
        if (k < 0) return z.data[0];
        if (looping && k > z.loopEnd) k = z.loopStart + (k - z.loopEnd - 1) % (long)(z.loopEnd - z.loopStart + 1);
        return k < n ? z.data[(size_t)k] : 0.0f;
    }

    void renderVoice(Voice& v, float* out, int n, float vibSemitones)
    {
        const Zone& z = *v.zone;
        const double len = (double)(z.loopEnd - z.loopStart + 1);
        const double end = (double)z.data.size();
        for (int i = 0; i < n; ++i)
        {
            if ((v.ctrl++ % kCtrlInterval) == 0)
            {
                const float lfo = lfoBuf_[i];
                const float semis = v.pitch + s_.tune + alloc_.bendSemitones() + lfo * vibSemitones;
                v.inc = (float)(v.baseInc * semitonesToRatio(semis));
                if (s_.filterOn)
                {
                    const float octaves = s_.envAmt * v.filtEnv.level() * 8.0f + s_.lfoFilter * lfo * 4.0f
                                        + s_.keyTrack * ((float)v.note - 60.0f) / 12.0f;
                    v.filter.set(s_.cutoff * std::pow(2.0f, clamp(octaves, -10.0f, 10.0f)), s_.reso);
                }
            }
            const float aenv = v.ampEnv.process();
            v.filtEnv.process();

            // 4-point cubic (Hermite) interpolation
            const long k = (long)v.pos;
            const float f = (float)(v.pos - (double)k);
            const float xm1 = at(z, k - 1, v.looping), x0 = at(z, k, v.looping);
            const float x1 = at(z, k + 1, v.looping), x2 = at(z, k + 2, v.looping);
            const float c1 = 0.5f * (x1 - xm1);
            const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            float s = ((c3 * f + c2) * f + c1) * f + x0;

            if (s_.filterOn) s = v.filter.process(s, s_.mode);
            out[i] += s * aenv * v.velGain * 0.5f;

            v.pos += (double)v.inc;
            if (v.looping) { while (v.pos > (double)z.loopEnd + 1.0) v.pos -= len; }
            else if (v.pos >= end) { v.ampEnv.reset(); break; }   // sample finished
        }
    }

    std::vector<BankInfo> banks_;
    double sr_ = 44100.0;

    Sound* active_ = nullptr;                    // used by the audio thread only (and prepare)
    std::atomic<Sound*> pending_{nullptr};       // worker -> audio thread
    std::atomic<Sound*> retired_{nullptr};       // audio thread -> worker (to free)
    std::atomic<int> requested_{-1};             // what the knobs ask for
    std::atomic<int> current_{-1};               // what is playing or waiting in pending_
    std::atomic<bool> ready_{false};             // prepare() has loaded the first sound
    std::atomic<bool> running_{true};
    std::thread worker_;

    Voice voices_[kNumVoices];
    VoiceAllocator<kNumVoices> alloc_;
    LFO lfo_;
    MidiControllers controllers_;
    Smoother volume_;
    float lfoBuf_[kChunk] = {0};
};

MPC_PLUGIN_ENTRY(Vault)
