// MeltDeck.cpp - a two-deck DJ mixer for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// Two decks play songs from Content/Songs (WAV / MP3 / FLAC). Each deck: play/pause, CDJ-style
// cue, 4 hot cues, auto-loop (1/4-32 beats), beat jump, pitch fader with key lock, sync (tempo +
// beat phase, then held in phase), nudge, 3-band kill EQ, DJ filter, beat-synced echo, auto-gain.
// Mixer: crossfader (smooth or scratch cut), master volume, soft limiter.
//
// Pads (16 per deck): Bank A pads (notes 36-51) = deck A, Bank B pads (notes 52-67) = deck B:
//    1 Play/Pause   2 Cue          3 Sync         4 Loop on/off
//    5-8 Hot cues 1-4 (empty: set; set: jump; hold pad 16 + hot cue: delete)
//    9 Jump -4 beats  10 Jump +4 beats  11 Loop /2   12 Loop x2
//   13 Nudge - (hold) 14 Nudge + (hold) 15 Echo throw (hold)   16 Shift (hold)
//
// Threads: a worker decodes, resamples to the host rate and analyses (tempo, beat grid, key,
// loudness); the audio thread only swaps finished tracks in. Nothing on the audio thread waits.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/svf.h"
#include "dsp/biquad.h"
#include "dsp/smoother.h"
#include "rt_stretch.h"
#include "decode.h"
#include "stretch.h"

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
using deck::Track;

static constexpr double kMaxSeconds = 600.0;   // 10 minutes per track (memory)

static std::vector<std::string> listSongs(const std::string& dir)
{
    std::vector<std::string> names, keep;
#if defined(_WIN32)
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "/*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do { names.push_back(fd.cFileName); } while (FindNextFileA(h, &fd)); FindClose(h); }
#else
    if (DIR* d = opendir(dir.c_str())) { while (dirent* e = readdir(d)) names.push_back(e->d_name); closedir(d); }
#endif
    for (const auto& n : names)
    {
        if (n.size() < 5 || n[0] == '.') continue;
        std::string ext = n.substr(n.size() - 4);
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".wav" || ext == ".mp3" || ext == "flac") keep.push_back(n);
    }
    std::sort(keep.begin(), keep.end());
    return keep;
}

// ---------------------------------------------------------------------------
class MeltDeck : public mpc::Plugin
{
public:
    // Per-deck parameters (deck B = deck A index + kDeckStride)
    enum DeckParam { dSong, dPlay, dPitch, dRange, dKeyLock, dVolume, dLow, dMid, dHigh, dFilter, dEcho, dLoopLen,
                     dShowBpm, dShowKey, dShowTime, dShowStatus, kDeckStride };
    enum MasterParam { mCross = 2 * kDeckStride, mCurve, mMaster, kNumParams };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt Deck";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'z', 'X', 'w');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    MeltDeck() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        dir_ = contentPath("Songs");
        songs_ = listSongs(dir_);
        std::vector<std::string> names{"(empty)"};
        for (const auto& s : songs_) { std::string n = s.substr(0, s.find_last_of('.')); if (n.size() > 24) n = n.substr(0, 24); names.push_back(n); }
        for (int d = 0; d < 2; ++d)
        {
            const std::string X = d == 0 ? "A " : "B ", x = d == 0 ? "a" : "b";
            addParam(choiceParam((x + "song").c_str(), (X + "Track").c_str(), names, 0));
            addParam(boolParam ((x + "play").c_str(), (X + "Play").c_str(), false));
            addParam(floatParam((x + "pitch").c_str(), (X + "Pitch").c_str(), -100.0f, 100.0f, 0.0f, "%", 1));
            addParam(choiceParam((x + "range").c_str(), (X + "Range").c_str(), {"8%", "16%", "50%"}, 0));
            addParam(boolParam ((x + "lock").c_str(), (X + "Key Lock").c_str(), true));
            addParam(floatParam((x + "vol").c_str(), (X + "Volume").c_str(), 0.0f, 100.0f, 80.0f, "%", 0));
            addParam(floatParam((x + "low").c_str(), (X + "Low").c_str(), -100.0f, 100.0f, 0.0f, "", 0));
            addParam(floatParam((x + "mid").c_str(), (X + "Mid").c_str(), -100.0f, 100.0f, 0.0f, "", 0));
            addParam(floatParam((x + "high").c_str(), (X + "High").c_str(), -100.0f, 100.0f, 0.0f, "", 0));
            addParam(floatParam((x + "filter").c_str(), (X + "Filter").c_str(), -100.0f, 100.0f, 0.0f, "", 0));
            addParam(floatParam((x + "echo").c_str(), (X + "Echo").c_str(), 0.0f, 100.0f, 0.0f, "%", 0));
            addParam(choiceParam((x + "looplen").c_str(), (X + "Loop").c_str(), {"1/4", "1/2", "1", "2", "4", "8", "16", "32"}, 4));
            Param b = floatParam((x + "bpm").c_str(), (X + "BPM").c_str(), 0.0f, 400.0f, 0.0f, "", 1); b.automatable = false; addParam(b);
            Param k = floatParam((x + "key").c_str(), (X + "Key").c_str(), -1.0f, 23.0f, -1.0f, "", 0); k.automatable = false; addParam(k);
            Param t = floatParam((x + "time").c_str(), (X + "Remain").c_str(), 0.0f, 3600.0f, 0.0f, "", 0); t.automatable = false; addParam(t);
            Param s = floatParam((x + "status").c_str(), (X + "Status").c_str(), 0.0f, 10.0f, 0.0f, "", 0); s.automatable = false; addParam(s);
        }
        addParam(floatParam("cross", "Crossfader", -100.0f, 100.0f, 0.0f, "", 0));
        addParam(choiceParam("curve", "X Curve", {"Smooth", "Cut"}, 0));
        addParam(floatParam("master", "Master", -40.0f, 6.0f, -3.0f, "dB", 1));
        if (numParams() != kNumParams) mpc::log("MeltDeck: parameter count mismatch");
        addProgram("Init", [this] { std::vector<float> v((size_t)numParams()); for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def; return v; }());
        worker_ = std::thread([this] { workerLoop(); });
    }

    ~MeltDeck() override
    {
        running_.store(false);
        if (worker_.joinable()) worker_.join();
        for (auto& d : decks_) { delete d.track; delete d.pending.exchange(nullptr); delete d.retired.exchange(nullptr); delete d.old; }
    }

    bool formatValue(int index, float v, std::string& out) override
    {
        static const char* keys[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
        const int p = index % kDeckStride;
        if (index >= 2 * kDeckStride) return false;
        char buf[48];
        if (p == dShowBpm) { if (v <= 0) out = "--"; else { std::snprintf(buf, sizeof buf, "%.1f", v); out = buf; } return true; }
        if (p == dShowKey) { const int k = (int)std::lround(v); out = k < 0 ? "--" : std::string(keys[k % 12]) + (k < 12 ? " maj" : " min"); return true; }
        if (p == dShowTime) { const int s = (int)v; std::snprintf(buf, sizeof buf, "-%d:%02d", s / 60, s % 60); out = buf; return true; }
        if (p == dShowStatus) { static const char* st[] = {"Empty", "Loading", "Ready", "Can't read", "Loop", "Synced"}; out = st[mpc::clamp((int)std::lround(v), 0, 5)]; return true; }
        if (p == dLow || p == dMid || p == dHigh) { if (v <= -99.5f) out = "Kill"; else if (v < 0) { std::snprintf(buf, sizeof buf, "%.0f dB", 20.0f * std::log10(1.0f + v / 100.0f)); out = buf; } else { std::snprintf(buf, sizeof buf, "+%.1f dB", 6.0f * v / 100.0f); out = buf; } return true; }
        if (p == dPitch) { std::snprintf(buf, sizeof buf, "%+.2f%%", v * rangeOf(index / kDeckStride) / 100.0f); out = buf; return true; }
        return false;
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        hostSr_.store(sampleRate);
        for (auto& d : decks_)
        {
            d.filtL.setSampleRate(sampleRate); d.filtR.setSampleRate(sampleRate);
            d.echoL.assign((size_t)(sampleRate * 2.2), 0.0f); d.echoR.assign((size_t)(sampleRate * 2.2), 0.0f);
            d.vol.setTime(20, sampleRate); d.gLow.setTime(20, sampleRate); d.gMid.setTime(20, sampleRate); d.gHigh.setTime(20, sampleRate);
            designEq(d);
        }
        master_.setTime(20, sampleRate); xa_.setTime(10, sampleRate); xb_.setTime(10, sampleRate);
        ready_.store(true);
    }

    void reset() override { for (auto& d : decks_) { d.playing = false; d.ghostOn = false; } }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : scratch_;
        for (int dI = 0; dI < 2; ++dI) takeTrack(dI);
        for (int dI = 0; dI < 2; ++dI) readParams(dI);

        // MIDI: pads -> deck functions (at block start: pads are not sample-critical for a DJ)
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            if (!(ev.isNoteOn() || ev.isNoteOff())) continue;
            const int note = ev.note();
            if (note < 36 || note > 67) continue;
            pad(note < 52 ? 0 : 1, (note - 36) % 16, ev.isNoteOn());
        }
        // sync lock: keep a synced deck in phase with the other (tiny tempo corrections)
        for (int dI = 0; dI < 2; ++dI) syncLock(dI);

        for (int dI = 0; dI < 2; ++dI) renderDeck(dI, n);

        // crossfader + master
        const float x = get(mCross) / 100.0f;
        float ga, gb;
        if (getInt(mCurve) == 0) { const float a = (x + 1.0f) * 0.25f * 3.14159265f; ga = std::cos(a) * 1.41421356f; gb = std::sin(a) * 1.41421356f; ga = std::min(ga, 1.0f); gb = std::min(gb, 1.0f); }
        else { ga = x <= 0.9f ? 1.0f : (1.0f - x) * 10.0f; gb = x >= -0.9f ? 1.0f : (1.0f + x) * 10.0f; }   // scratch cut: full until the last 5 %
        xa_.target = ga; xb_.target = gb;
        master_.target = dbToGain(get(mMaster));
        Deck& A = decks_[0]; Deck& B = decks_[1];
        for (int i = 0; i < n; ++i)
        {
            const float a = xa_.next(), b = xb_.next(), m = master_.next();
            float l = (A.bufL[(size_t)i] * a + B.bufL[(size_t)i] * b) * m;
            float r = (A.bufR[(size_t)i] * a + B.bufR[(size_t)i] * b) * m;
            L[i] = sanitize(softLimit(l)); R[i] = sanitize(softLimit(r));
        }
        // displays (only when they change)
        for (int dI = 0; dI < 2; ++dI) showDeck(dI);
    }

private:
    static constexpr int kMaxBlock = 4096;

    struct OnePoleSmooth { float cur = 0, target = 0, k = 1; void setTime(float ms, double sr) { k = 1.0f - std::exp(-1.0f / (ms * 0.001f * (float)sr)); } float next() { cur += k * (target - cur); return cur; } };

    struct Deck
    {
        // track hand-over
        Track* track = nullptr; Track* old = nullptr;
        std::atomic<Track*> pending{nullptr}, retired{nullptr};
        std::atomic<int> status{0};   // 0 empty 1 loading 2 ready 3 error
        int requested = 0;            // song index + 1 (0 = empty)
        std::atomic<int> want{0};
        // transport
        bool playing = false, paramPlay = false;
        double pos = 0;               // audible position (source samples)
        double cue = 0;
        bool cuePreview = false;
        double hot[4] = {0, 0, 0, 0}; bool hotSet[4] = {false, false, false, false};
        bool loopOn = false; double loopStart = 0, loopEnd = 0; int loopLenIdx = 4;
        bool synced = false; float syncTrim = 0;   // phase-lock correction (fraction)
        int nudge = 0; bool echoThrow = false, shift = false;
        float pitchPct = 0; float range = 8; bool keyLock = true; bool lockWas = true;
        // engines
        deck::KeyLock kl;
        bool klActive = false;
        // ghost head: fades the old sound out over 5 ms after any jump (no clicks)
        bool ghostOn = false; double ghostPos = 0; float ghostGain = 0; const Track* ghostTrack = nullptr;
        float fade = 0;               // pause/play fade
        // tone
        StereoBiquad lo1, lo2, hi1, hi2, mlo1, mlo2, mhi1, mhi2;   // Linkwitz-Riley crossover tree (two Butterworth stages per split)
        int echoActiveTail = 0;
        OnePoleSmooth vol, gLow, gMid, gHigh;
        SVF filtL, filtR; int filterMode = 0; float lastFilter = -1000;
        std::vector<float> echoL, echoR; size_t echoW = 0; float echoMix = 0;
        float bufL[kMaxBlock] = {0}, bufR[kMaxBlock] = {0};
        float shown[4] = {-1, -1, -1, -1};
    };

    float rangeOf(int d) const { static const float r[] = {8, 16, 50}; return r[mpc::clamp(getInt(d * kDeckStride + dRange), 0, 2)]; }
    int P(int d, int p) const { return d * kDeckStride + p; }

    // ---- track hand-over ----
    void takeTrack(int dI)
    {
        Deck& d = decks_[dI];
        if (d.old != nullptr && !(d.ghostOn && d.ghostTrack == d.old) && d.retired.load() == nullptr) { d.retired.store(d.old); d.old = nullptr; }
        if (d.old == nullptr && d.retired.load() == nullptr)
            if (Track* t = d.pending.exchange(nullptr))
            {
                if (d.track && d.playing) startGhost(d);
                d.old = d.track; d.track = t;
                d.playing = false; d.loopOn = false; d.synced = false;
                for (bool& h : d.hotSet) h = false;
                // auto cue: first beat at or after the start
                const double beat = 60.0 / t->bpm * sr_;
                d.cue = std::fmod(t->beatPhase, beat);
                d.pos = d.cue;
                setParamBool(P(dI, dPlay), false, d);
            }
    }

    void setParamBool(int idx, bool v, Deck& d) { if (getBool(idx) != v) set(idx, v ? 1.0f : 0.0f); d.paramPlay = v; }

    void readParams(int dI)
    {
        Deck& d = decks_[dI];
        d.want.store(getInt(P(dI, dSong)));
        // play switch on the screen
        const bool pp = getBool(P(dI, dPlay));
        if (pp != d.paramPlay) { d.paramPlay = pp; if (pp != d.playing) togglePlay(dI); }
        d.range = rangeOf(dI);
        d.pitchPct = get(P(dI, dPitch)) * d.range / 100.0f;
        const bool lock = getBool(P(dI, dKeyLock));
        if (lock != d.keyLock) { d.keyLock = lock; if (d.playing && d.track) restartAt(d, d.pos); }
        d.loopLenIdx = getInt(P(dI, dLoopLen));
        d.vol.target = std::pow(get(P(dI, dVolume)) / 100.0f, 2.0f) * (d.track ? d.track->gain : 1.0f);
        auto band = [](float v) { return v <= -99.5f ? 0.0f : (v < 0 ? 1.0f + v / 100.0f : dbToGain(6.0f * v / 100.0f)); };
        d.gLow.target = band(get(P(dI, dLow))); d.gMid.target = band(get(P(dI, dMid))); d.gHigh.target = band(get(P(dI, dHigh)));
        d.echoMix = get(P(dI, dEcho)) / 100.0f;
        const float f = get(P(dI, dFilter));
        if (f != d.lastFilter)
        {
            d.lastFilter = f;
            const float t = std::fabs(f) / 100.0f;
            if (t < 0.01f) d.filterMode = 0;
            else if (f < 0) { d.filterMode = 1; const float hz = 20000.0f * std::pow(120.0f / 20000.0f, t); d.filtL.set(hz, 0.3f); d.filtR.set(hz, 0.3f); }
            else            { d.filterMode = 2; const float hz = 20.0f * std::pow(4000.0f / 20.0f, t);   d.filtL.set(hz, 0.3f); d.filtR.set(hz, 0.3f); }
        }
    }

    double rate(const Deck& d) const
    {
        const double nudge = d.nudge * 0.04;   // hold: +-4 %
        return (1.0 + d.pitchPct / 100.0) * (1.0 + nudge) * (1.0 + d.syncTrim);
    }
    double beatLen(const Deck& d) const { return d.track ? 60.0 / d.track->bpm * sr_ : sr_ * 0.5; }

    // ---- transport ----
    void togglePlay(int dI)
    {
        Deck& d = decks_[dI];
        if (!d.track) return;
        if (d.playing) { d.playing = false; }               // fade out handled in render
        else { d.playing = true; restartAt(d, d.pos); }
        setParamBool(P(dI, dPlay), d.playing, d);
    }
    void restartAt(Deck& d, double p)
    {
        if (!d.track) return;
        if (d.fade > 0.0f) startGhost(d);
        d.pos = mpc::clamp(p, 0.0, (double)d.track->frames() - 1.0);
        if (d.keyLock) { d.kl.jump(*d.track, d.pos, rate(d)); d.klActive = true; } else d.klActive = false;
        if (d.playing) d.fade = 1.0f;   // the ghost covers the jump; the new sound starts at full level
    }
    void startGhost(Deck& d)
    {
        if (!d.track) return;
        d.ghostOn = true; d.ghostPos = d.pos; d.ghostGain = d.fade; d.ghostTrack = d.track;
    }
    void jump(int dI, double p) { Deck& d = decks_[dI]; if (!d.track) return; if (d.playing) restartAt(d, p); else d.pos = mpc::clamp(p, 0.0, (double)d.track->frames() - 1.0); }

    void pad(int dI, int k, bool on)
    {
        Deck& d = decks_[dI];
        if (k == 15) { d.shift = on; return; }
        if (k == 12) { d.nudge = on ? -1 : 0; return; }
        if (k == 13) { d.nudge = on ? 1 : 0; return; }
        if (k == 14) { d.echoThrow = on; return; }
        if (!d.track) return;
        const double beat = beatLen(d);
        if (k == 1)   // CUE (CDJ style)
        {
            if (on)
            {
                if (d.playing) { startGhost(d); d.playing = false; d.fade = 0; d.pos = d.cue; setParamBool(P(dI, dPlay), false, d); }
                else if (std::fabs(d.pos - d.cue) < 64.0) { d.cuePreview = true; d.playing = true; restartAt(d, d.cue); }   // preview while held
                else { d.cue = snapBeat(d, d.pos); d.pos = d.cue; }
            }
            else if (d.cuePreview) { d.cuePreview = false; d.playing = false; startGhost(d); d.fade = 0; d.pos = d.cue; }
            return;
        }
        if (!on) return;
        if (k == 0) { togglePlay(dI); d.cuePreview = false; }
        else if (k == 2) doSync(dI);
        else if (k == 3)
        {
            if (d.loopOn) d.loopOn = false;
            else { static const double lb[] = {0.25, 0.5, 1, 2, 4, 8, 16, 32}; d.loopStart = snapBeatFloor(d, d.pos); d.loopEnd = d.loopStart + lb[mpc::clamp(d.loopLenIdx, 0, 7)] * beat; d.loopOn = true; }
        }
        else if (k >= 4 && k <= 7)
        {
            const int h = k - 4;
            if (d.shift) d.hotSet[h] = false;
            else if (!d.hotSet[h]) { d.hot[h] = snapBeat(d, d.pos); d.hotSet[h] = true; }
            else { if (!d.playing) { d.playing = true; setParamBool(P(dI, dPlay), true, d); } restartAt(d, d.hot[h]); }
        }
        else if (k == 8 || k == 9) jump(dI, d.pos + (k == 8 ? -4.0 : 4.0) * beat);
        else if (k == 10 || k == 11)
        {
            const int idx = mpc::clamp(getInt(P(dI, dLoopLen)) + (k == 10 ? -1 : 1), 0, 7);
            set(P(dI, dLoopLen), (float)idx); d.loopLenIdx = idx;
            if (d.loopOn) { static const double lb[] = {0.25, 0.5, 1, 2, 4, 8, 16, 32}; d.loopEnd = d.loopStart + lb[idx] * beat; if (d.pos >= d.loopEnd) jump(dI, d.loopStart); }
        }
    }

    double snapBeat(const Deck& d, double p) const   // nearest beat (quantised cues)
    {
        const double b = beatLen(d), ph = d.track->beatPhase;
        return std::max(0.0, ph + std::round((p - ph) / b) * b);
    }
    double snapBeatFloor(const Deck& d, double p) const
    {
        const double b = beatLen(d), ph = d.track->beatPhase;
        const double s = ph + std::floor((p - ph) / b + 0.02) * b;
        return std::max(0.0, s);
    }

    // ---- sync ----
    void doSync(int dI)
    {
        Deck& d = decks_[dI]; Deck& o = decks_[1 - dI];
        if (!d.track || !o.track) return;
        // tempo: match the other deck's playing BPM
        const double target = o.track->bpm * rate(o) / (1.0 + o.syncTrim);
        double pct = (target / d.track->bpm - 1.0) * 100.0;
        // half/double if that is closer (e.g. 70 vs 140)
        for (double m : {0.5, 2.0}) { const double alt = (target * m / d.track->bpm - 1.0) * 100.0; if (std::fabs(alt) < std::fabs(pct)) pct = alt; }
        float range = d.range;
        while (std::fabs(pct) > range && range < 50) { range = range < 16 ? 16.0f : 50.0f; }
        set(P(dI, dRange), range >= 50 ? 2.0f : range >= 16 ? 1.0f : 0.0f);
        d.range = range;
        set(P(dI, dPitch), (float)mpc::clamp(pct / range * 100.0, -100.0, 100.0));
        d.pitchPct = (float)mpc::clamp(pct, -(double)range, (double)range);
        d.synced = true; d.syncTrim = 0;
        // phase: line our beats up with the other deck's
        if (o.playing || true)
        {
            const double err = phaseError(d, o);
            jump(dI, d.pos + err * beatLen(d));
        }
    }
    // fraction of a beat (-0.5..0.5) to move d so its beats land on o's beats
    double phaseError(const Deck& d, const Deck& o) const
    {
        const double bd = beatLen(d), bo = beatLen(o);
        const double fd = (d.pos - d.track->beatPhase) / bd, fo = (o.pos - o.track->beatPhase) / bo;
        double e = (fo - std::floor(fo)) - (fd - std::floor(fd));
        e -= std::round(e);
        return e;
    }
    void syncLock(int dI)
    {
        Deck& d = decks_[dI]; Deck& o = decks_[1 - dI];
        if (!d.synced || !d.track || !o.track || !d.playing || !o.playing || d.nudge != 0 || o.nudge != 0) { d.syncTrim = 0; return; }
        const double e = phaseError(d, o);                   // beats
        d.syncTrim = (float)mpc::clamp(e * 0.02, -0.003, 0.003);   // gentle: at most 0.3 % speed change
    }

    // ---- audio ----
    void renderDeck(int dI, int n)
    {
        Deck& d = decks_[dI];
        float* L = d.bufL; float* R = d.bufR;
        for (int i = 0; i < n; ++i) { L[i] = 0; R[i] = 0; }
        const bool active = d.track && (d.playing || d.fade > 0.0f);
        if (active)
        {
            const Track& t = *d.track;
            const double r = rate(d);
            const float fadeStep = 1.0f / (0.005f * (float)sr_);   // 5 ms play/pause fade
            int done = 0;
            while (done < n && d.track && (d.playing || d.fade > 0.0f))
            {
                // render up to the next loop end / track end exactly (sample-accurate loops)
                int m = n - done;
                const double stop = d.loopOn ? d.loopEnd : (double)t.frames() - 2.0;
                const double left = (stop - d.pos) / r;
                if (left < m) m = std::max(1, (int)std::ceil(left));
                if (d.keyLock && !d.klActive) { d.kl.jump(t, d.pos, r); d.klActive = true; }
                if (d.keyLock) d.kl.pull(t, L + done, R + done, m, r);
                else for (int i = 0; i < m; ++i) readCubic(t, d.pos + i * r, L[done + i], R[done + i]);
                for (int i = 0; i < m; ++i)
                {
                    d.fade = d.playing ? std::min(1.0f, d.fade + fadeStep) : std::max(0.0f, d.fade - fadeStep);
                    L[done + i] *= d.fade; R[done + i] *= d.fade;
                }
                d.pos += r * m;
                done += m;
                if (d.loopOn && d.pos >= d.loopEnd) restartAt(d, d.loopStart + std::fmod(d.pos - d.loopEnd, d.loopEnd - d.loopStart));
                else if (d.pos >= (double)t.frames() - 2.0) { startGhost(d); d.playing = false; d.fade = 0; d.pos = d.cue; setParamBool(P(dI, dPlay), false, d); }
            }
            if (!d.playing && d.fade <= 0.0f) d.klActive = false;
        }
        if (d.ghostOn && d.ghostTrack)
        {
            const float step = 1.0f / (0.005f * (float)sr_);
            const double r = rate(d);
            for (int i = 0; i < n && d.ghostGain > 0.0f; ++i)
            {
                float l, rr; readCubic(*d.ghostTrack, d.ghostPos, l, rr);
                L[i] += l * d.ghostGain; R[i] += rr * d.ghostGain;
                d.ghostPos += r; d.ghostGain -= step;
            }
            if (d.ghostGain <= 0.0f) { d.ghostOn = false; d.ghostTrack = nullptr; }
        }
        tone(d, n);
    }

    static inline void readCubic(const Track& t, double p, float& l, float& r)
    {
        const long k = (long)p;
        const long n = (long)t.frames();
        if (k < 1 || k + 2 >= n) { l = r = 0.0f; return; }
        const float f = (float)(p - (double)k);
        auto cub = [&](const int16_t* x) {
            const float xm1 = x[k - 1], x0 = x[k], x1 = x[k + 1], x2 = x[k + 2];
            const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            return (((c3 * f + c2) * f + c1) * f + x0) * (1.0f / 32768.0f); };
        l = cub(t.l.data()); r = cub(t.r.data());
    }

    void designEq(Deck& d)
    {
        // Linkwitz-Riley 4th-order crossover tree: split at 250 Hz, then split the top at 2.5 kHz.
        // The bands sum back to a flat (all-pass) response; each band can be killed cleanly.
        const BiquadCoeffs lp250 = BiquadCoeffs::lowpass(sr_, 250.0f, 0.7071f), hp250 = BiquadCoeffs::highpass(sr_, 250.0f, 0.7071f);
        const BiquadCoeffs lp2k5 = BiquadCoeffs::lowpass(sr_, 2500.0f, 0.7071f), hp2k5 = BiquadCoeffs::highpass(sr_, 2500.0f, 0.7071f);
        d.lo1.setCoeffs(lp250); d.lo2.setCoeffs(lp250); d.hi1.setCoeffs(hp250); d.hi2.setCoeffs(hp250);
        d.mlo1.setCoeffs(lp2k5); d.mlo2.setCoeffs(lp2k5); d.mhi1.setCoeffs(hp2k5); d.mhi2.setCoeffs(hp2k5);
    }

    void tone(Deck& d, int n)
    {
        // 3-band isolator (DJ mixer style): low | mid | high from a crossover tree, gains, sum
        for (int i = 0; i < n; ++i)
        {
            const float gl = d.gLow.next(), gm = d.gMid.next(), gh = d.gHigh.next(), v = d.vol.next();
            float lowL = d.bufL[i], lowR = d.bufR[i], upL = lowL, upR = lowR;
            d.lo1.process(lowL, lowR); d.lo2.process(lowL, lowR);
            d.hi1.process(upL, upR); d.hi2.process(upL, upR);
            float midL = upL, midR = upR, topL = upL, topR = upR;
            d.mlo1.process(midL, midR); d.mlo2.process(midL, midR);
            d.mhi1.process(topL, topR); d.mhi2.process(topL, topR);
            d.bufL[i] = (gl * lowL + gm * midL + gh * topL) * v;
            d.bufR[i] = (gl * lowR + gm * midR + gh * topR) * v;
        }
        if (d.filterMode != 0)
            for (int i = 0; i < n; ++i) { d.bufL[i] = d.filtL.process(d.bufL[i], d.filterMode == 1 ? 0 : 2); d.bufR[i] = d.filtR.process(d.bufR[i], d.filterMode == 1 ? 0 : 2); }
        // beat-synced echo (3/4 beat), post-fader; "throw" pad sends full while held
        const float send = d.echoThrow ? 1.0f : d.echoMix;
        if (send > 0.0f || d.echoActiveTail > 0)
        {
            const size_t len = d.echoL.size();
            const size_t dl = (size_t)mpc::clamp(0.75 * beatLen(d) / rate(d), 64.0, (double)len - 2);
            for (int i = 0; i < n; ++i)
            {
                const size_t rd = (d.echoW + len - dl) % len;
                const float el = d.echoL[rd], er = d.echoR[rd];
                d.echoL[d.echoW] = d.bufL[i] * send + el * 0.5f;
                d.echoR[d.echoW] = d.bufR[i] * send + er * 0.5f;
                d.echoW = (d.echoW + 1) % len;
                d.bufL[i] += el * 0.7f; d.bufR[i] += er * 0.7f;
            }
            d.echoActiveTail = send > 0.0f ? (int)(sr_ * 4) : std::max(0, d.echoActiveTail - n);
        }
    }

    static inline float softLimit(float x)
    {
        const float a = std::fabs(x);
        if (a < 0.9f) return x;
        const float y = 0.9f + 0.09f * std::tanh((a - 0.9f) / 0.09f);   // never above 0.99
        return x < 0 ? -y : y;
    }

    void showDeck(int dI)
    {
        Deck& d = decks_[dI];
        float v[4];
        const Track* t = d.track;
        v[0] = t ? (float)(t->bpm * rate(d) / (1.0 + d.syncTrim)) : 0.0f;
        int key = t ? t->keyIndex : -1;
        if (t && key >= 0 && !d.keyLock)   // vinyl mode: the key moves with the speed
        {
            const int st = (int)std::lround(12.0 * std::log2(rate(d)));
            key = (key >= 12 ? 12 : 0) + ((key % 12 + st) % 12 + 12) % 12;
        }
        v[1] = (float)key;
        v[2] = t ? (float)std::floor(std::max(0.0, ((double)t->frames() - d.pos) / sr_ / rate(d))) : 0.0f;
        const int st = d.status.load();
        v[3] = (float)(st == 2 ? (d.loopOn ? 4 : d.synced ? 5 : 2) : st);
        for (int k = 0; k < 4; ++k)
            if (v[k] != d.shown[k]) { d.shown[k] = v[k]; set(P(dI, dShowBpm + k), v[k]); }
    }

    // ---- worker: load, resample to the host rate, analyse ----
    void workerLoop()
    {
        int loaded[2] = {0, 0};
        while (running_.load())
        {
            for (auto& d : decks_) delete d.retired.exchange(nullptr);
            if (!ready_.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(15)); continue; }
            bool did = false;
            for (int dI = 0; dI < 2; ++dI)
            {
                Deck& d = decks_[dI];
                const int want = d.want.load();
                if (want == loaded[dI]) continue;
                loaded[dI] = want; did = true;
                if (want <= 0 || want > (int)songs_.size()) { d.status.store(0); continue; }
                d.status.store(1);
                flip::Song s; std::string err;
                if (!flip::decodeFile(dir_ + "/" + songs_[(size_t)want - 1], s, kMaxSeconds, err)) { d.status.store(3); continue; }
                const double hsr = hostSr_.load();
                if (std::fabs(s.sampleRate - hsr) > 0.5)   // play at the host rate so key lock and pitch are exact
                {
                    std::vector<float> l2, r2; const double step = s.sampleRate / hsr; const size_t outLen = (size_t)(s.l.size() / step);
                    flip::detail::resample(s.l, l2, step, outLen); flip::detail::resample(s.r, r2, step, outLen);
                    s.l.swap(l2); s.r.swap(r2); s.sampleRate = hsr;
                }
                std::vector<float> mono(s.l.size()); double e = 0;
                for (size_t i = 0; i < mono.size(); ++i) { mono[i] = 0.5f * (s.l[i] + s.r[i]); e += (double)mono[i] * mono[i]; }
                flip::Analysis an = flip::analyse(mono.data(), mono.size(), s.sampleRate);
                auto* t = new Track();
                t->bpm = an.bpm > 0 ? an.bpm : 120.0;
                t->beatPhase = an.beatPhase;
                t->keyIndex = an.keyIndex;
                const double rms = std::sqrt(e / std::max<size_t>(1, mono.size()));
                t->gain = (float)mpc::clamp(0.2 / std::max(rms, 1e-4), 0.25, 4.0);   // auto-gain toward ~-14 dBFS RMS
                t->l.resize(s.l.size()); t->r.resize(s.r.size());
                for (size_t i = 0; i < s.l.size(); ++i)
                {
                    t->l[i] = (int16_t)std::lrint(mpc::clamp(s.l[i], -1.0f, 1.0f) * 32767.0f);
                    t->r[i] = (int16_t)std::lrint(mpc::clamp(s.r[i], -1.0f, 1.0f) * 32767.0f);
                }
                if (d.want.load() != want) { delete t; loaded[dI] = -1; continue; }   // changed meanwhile: load the new one
                delete d.pending.exchange(t);
                d.status.store(2);
            }
            if (!did) std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }

    std::string dir_;
    std::vector<std::string> songs_;
    double sr_ = 44100;
    std::atomic<double> hostSr_{44100};
    std::atomic<bool> ready_{false}, running_{true};
    std::thread worker_;
    Deck decks_[2];
    OnePoleSmooth master_, xa_, xb_;
    float scratch_[kMaxBlock] = {0};
};

MPC_PLUGIN_ENTRY(MeltDeck)
