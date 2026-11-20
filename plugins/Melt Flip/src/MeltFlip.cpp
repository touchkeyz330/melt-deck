// MeltFlip.cpp - a song chopper for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// Load a song (WAV / MP3 / FLAC in Content/Songs). Melt Flip finds its tempo, bar grid, key and
// hits, and lays 16 chops across your pads:
//   Hits  - the 16 strongest hits in the 8 bars from Start Bar
//   Bars  - one bar per pad, from Start Bar
//   Beats - one beat per pad (4 bars), from Start Bar
//   16ths - one sixteenth per pad (1 bar), from Start Bar
// Key Shift and Tempo Sync re-render the whole song in the background with a phase-vocoder
// time-stretch (pitch and tempo change independently). Pitch is classic MPC re-pitch (speed).
//
// Pads: notes 36-51 = pads 1-16 (higher/lower notes wrap). Keys mode: every note plays the
// Key Pad chop chromatically (C3 = original pitch).
//
// Threads: a worker loads, analyses and renders; the audio thread only swaps finished data in
// (never waits, never allocates). Notes playing during a swap are faded out.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/svf.h"
#include "dsp/smoother.h"
#include "analysis.h"
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

static constexpr double kMaxSeconds = 480.0;   // 8 minutes per song (memory)
static constexpr int kPads = 16;

// ---------------------------------------------------------------------------
// Songs in Content/Songs (names fixed when the plugin loads)
// ---------------------------------------------------------------------------
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
// What the audio thread plays: one song, possibly re-rendered (immutable once published)
// ---------------------------------------------------------------------------
struct Material
{
    int song = -1, keyShift = 0; bool synced = false; double targetBpm = 0;
    std::vector<int16_t> l, r;
    double sr = 44100;
    double bpm = 120, barPhase = 0;      // in this material's timeline
    int keyIndex = -1;
    std::vector<flip::Hit> hits;         // time order
    size_t frames() const { return l.size(); }
};

static int16_t toI16(float v) { v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); return (int16_t)std::lrint(v * 32767.0f); }

// ---------------------------------------------------------------------------
class MeltFlip : public mpc::Plugin
{
public:
    enum Params
    {
        kVolume, kSong, kChop, kStartBar, kTempoSync, kKeyShift, kBpmFix,
        kPlayMode, kChoke, kLength, kAttack, kRelease, kPitch, kFilter,
        kKeysMode, kKeyPad, kBpmShow, kKeyShow, kStatus,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt Flip";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'J', '2', 'Y');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    MeltFlip() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        dir_ = contentPath("Songs");
        songs_ = listSongs(dir_);
        std::vector<std::string> names;
        for (const auto& s : songs_) { std::string n = s.substr(0, s.find_last_of('.')); if (n.size() > 24) n = n.substr(0, 24); names.push_back(n); }
        if (names.empty()) names.push_back("No Songs");
        if (names.size() == 1) names.push_back("-");

        addParam(floatParam("vol",    "Volume", -40.0f, 6.0f, -3.0f, "dB", 1));
        addParam(choiceParam("song",  "Song", names, 0));
        addParam(choiceParam("chop",  "Chop", {"Hits", "Bars", "Beats", "16ths"}, 0));
        addParam(intParam  ("start",  "Start Bar", 1, 256, 1));
        addParam(boolParam ("sync",   "Tempo Sync", false));
        addParam(intParam  ("keysh",  "Key Shift", -12, 12, 0, "st"));
        addParam(choiceParam("bpmfix", "BPM Fix", {"Auto", "x2", "/2"}, 0));
        addParam(choiceParam("mode",  "Play Mode", {"One Shot", "Gate", "Loop"}, 0));
        addParam(choiceParam("choke", "Choke", {"Mono", "Poly"}, 0));
        addParam(floatParam("length", "Length", 5.0f, 100.0f, 100.0f, "%", 0));
        addParam(logParam  ("atk",    "Attack", 0.5f, 500.0f, 1.0f, "ms", 1));
        addParam(logParam  ("rel",    "Release", 3.0f, 2000.0f, 40.0f, "ms", 0));
        addParam(intParam  ("pitch",  "Pitch", -24, 24, 0, "st"));
        addParam(floatParam("filter", "Filter", -100.0f, 100.0f, 0.0f, "%", 0));
        addParam(boolParam ("keys",   "Keys Mode", false));
        addParam(intParam  ("keypad", "Key Pad", 1, kPads, 1));
        Param p1 = floatParam("bpmshow", "Song BPM", 0.0f, 400.0f, 0.0f, "", 1); p1.automatable = false; addParam(p1);
        Param p2 = floatParam("keyshow", "Song Key", -1.0f, 23.0f, -1.0f, "", 0); p2.automatable = false; addParam(p2);
        Param p3 = floatParam("status", "Status", 0.0f, 200.0f, 0.0f, "", 0); p3.automatable = false; addParam(p3);
        if (numParams() != kNumParams) mpc::log("MeltFlip: parameter count mismatch");

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> o)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& p : o) v[(size_t)p.first] = p.second;
            addProgram(name, std::move(v));
        };
        program("Chop Hits", {});
        program("Bar Loops", {{kChop, 1}, {kPlayMode, 2}, {kTempoSync, 1}});
        program("Beat Flip", {{kChop, 2}, {kPlayMode, 1}});
        program("Break Slicer", {{kChop, 3}, {kChoke, 1}, {kRelease, 20}});
        program("Keys Mode", {{kKeysMode, 1}, {kPlayMode, 1}, {kAttack, 5}, {kRelease, 150}, {kChoke, 1}});

        worker_ = std::thread([this] { workerLoop(); });
    }

    ~MeltFlip() override
    {
        running_.store(false);
        cancel_.store(true);
        if (worker_.joinable()) worker_.join();
        delete active_;
        delete pending_.exchange(nullptr);
        delete retired_.exchange(nullptr);
    }

    bool formatValue(int index, float v, std::string& out) override
    {
        static const char* keys[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
        char buf[48];
        if (index == kBpmShow) { if (v <= 0.0f) out = "--"; else { std::snprintf(buf, sizeof buf, "%.1f", v); out = buf; } return true; }
        if (index == kKeyShow) { const int k = (int)std::lround(v); out = k < 0 ? "--" : std::string(keys[k % 12]) + (k < 12 ? " maj" : " min"); return true; }
        if (index == kStatus)
        {
            const int s = (int)std::lround(v);
            if (s == 0) out = songs_.empty() ? "No songs" : "--";
            else if (s >= 1 && s <= 99) { std::snprintf(buf, sizeof buf, "Rendering %d%%", s); out = buf; }
            else if (s == 100) out = "Ready";
            else if (s == 101) out = "Loading";
            else if (s == 102) out = "Analysing";
            else out = "Can't read file";
            return true;
        }
        return false;
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = sampleRate;
        volume_.setTime(15.0f, sampleRate); volume_.snap(dbToGain(get(kVolume)));
        filtL_.setSampleRate(sampleRate); filtR_.setSampleRate(sampleRate);
        publishRequest();
        ready_.store(true);
    }

    void reset() override
    {
        for (auto& v : voices_) v.active = false;
        filtL_.reset(); filtR_.reset();
        volume_.snap(dbToGain(get(kVolume)));
    }

    void process(mpc::ProcessContext& ctx) override
    {
        // tell the worker what we want; take finished material
        if (ctx.transport.tempoValid && ctx.transport.bpm > 20.0) projectBpm_.store(ctx.transport.bpm);
        publishRequest();
        if (retired_.load() == nullptr && oldMat_ == nullptr)   // one hand-over at a time
            if (Material* fresh = pending_.exchange(nullptr))
            {
                for (auto& v : voices_) if (v.active) startFade(v);   // finish quickly on the old material
                oldMat_ = active_;                                      // kept until those fades end
                active_ = fresh;
                cuesDirty_ = true;
            }
        if (oldMat_ != nullptr)
        {
            bool busy = false;
            for (auto& v : voices_) if (v.active && v.mat == oldMat_) busy = true;
            if (!busy && retired_.load() == nullptr) { retired_.store(oldMat_); oldMat_ = nullptr; }
        }
        updateCues();
        // displays
        {
            const float b = (float)showBpm_.load(), k = (float)showKey_.load(), st = (float)status_.load();
            if (b != shown_[0]) { shown_[0] = b; set(kBpmShow, b); }
            if (k != shown_[1]) { shown_[1] = k; set(kKeyShow, k); }
            if (st != shown_[2]) { shown_[2] = st; set(kStatus, st); }
        }

        // settings for this block
        const int mode = getInt(kPlayMode);
        const bool mono = getInt(kChoke) == 0;
        const float atk = std::max(0.5f, get(kAttack)) * 0.001f * (float)sr_, rel = std::max(3.0f, get(kRelease)) * 0.001f * (float)sr_;
        const float pitch = (float)getInt(kPitch);
        const bool keys = getBool(kKeysMode);
        const int keyPad = mpc::clamp(getInt(kKeyPad) - 1, 0, kPads - 1);
        volume_.setTarget(dbToGain(get(kVolume)));
        updateFilter();

        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        for (int i = 0; i < ctx.numFrames; ++i) { L[i] = 0.0f; if (R) R[i] = 0.0f; }

        int pos = 0;
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            if (ev.sampleOffset > pos) { render(L, R, pos, ev.sampleOffset); pos = ev.sampleOffset; }
            if (ev.isNoteOn()) noteOn(ev.note(), ev.velocity(), mode, mono, atk, rel, pitch, keys, keyPad);
            else if (ev.isNoteOff()) noteOff(ev.note(), mode);
            else if (ev.isAllNotesOff()) for (auto& v : voices_) if (v.active) v.releasing = true;
        }
        render(L, R, pos, ctx.numFrames);

        for (int i = 0; i < ctx.numFrames; ++i)
        {
            const float g = volume_.process();
            float l = L[i], r = R ? R[i] : L[i];
            if (filterMode_ != 0) { l = filtL_.process(l, filterMode_ == 1 ? 0 : 2); r = filtR_.process(r, filterMode_ == 1 ? 0 : 2); }
            L[i] = sanitize(l * g);
            if (R) R[i] = sanitize(r * g);
        }
    }

private:
    static constexpr int kVoices = 24;

    struct Voice
    {
        bool active = false, releasing = false, oneShot = false, loop = false;
        const Material* mat = nullptr;
        int note = -1, pad = -1;
        double pos = 0, start = 0, end = 0, inc = 1;
        float gain = 1, env = 0, atkStep = 1, relStep = 1;
        unsigned age = 0;
    };

    struct Cue { double start = 0, end = 0; };

    // ---- requests to the worker ----
    void publishRequest()
    {
        int song = getInt(kSong);
        if (songs_.empty()) song = -1;
        reqSong_.store(song);
        reqKey_.store(getInt(kKeyShift));
        reqSync_.store(getBool(kTempoSync));
        reqFix_.store(getInt(kBpmFix));
    }

    // ---- cues: computed on the audio thread, no allocation ----
    void updateCues()
    {
        const int chop = getInt(kChop), startBar = getInt(kStartBar) - 1, fix = getInt(kBpmFix);
        const float lenPct = get(kLength);
        if (!cuesDirty_ && chop == lastChop_ && startBar == lastStart_ && fix == lastFix_ && lenPct == lastLen_) return;
        cuesDirty_ = false; lastChop_ = chop; lastStart_ = startBar; lastFix_ = fix; lastLen_ = lenPct;
        const Material* m = active_;
        for (auto& c : cues_) c = Cue();
        if (m == nullptr || m->frames() < 64) return;
        const double total = (double)m->frames();
        const double bpm = m->bpm * (fix == 1 ? 2.0 : fix == 2 ? 0.5 : 1.0);
        const double beat = 60.0 / bpm * m->sr, bar = 4.0 * beat;
        const double origin = std::fmod(m->barPhase, bar);
        auto barAt = [&](int b) { return origin + b * bar; };
        auto snap = [&](double t) {   // pull grid cues onto a real hit within 25 ms (cleaner transients)
            const double tol = 0.025 * m->sr;
            auto it = std::lower_bound(m->hits.begin(), m->hits.end(), t, [](const flip::Hit& h, double x) { return h.pos < x; });
            double best = t, bd = tol;
            if (it != m->hits.end() && std::fabs(it->pos - t) < bd) { best = it->pos; bd = std::fabs(it->pos - t); }
            if (it != m->hits.begin() && std::fabs((it - 1)->pos - t) < bd) best = (it - 1)->pos;
            return best;
        };
        const double slice = chop == 1 ? bar : chop == 2 ? beat : beat / 4.0;
        if (chop >= 1)
        {
            const double first = barAt(startBar);
            for (int p = 0; p < kPads; ++p)
            {
                const double s = first + p * slice;
                cues_[p].start = std::max(0.0, chop == 3 ? s : snap(s));
                cues_[p].end = s + slice;
            }
        }
        else
        {
            // Hits: the 16 strongest hits in the 8 bars from Start Bar, then in time order
            const double from = barAt(startBar) - 0.03 * m->sr, to = barAt(startBar + 8);
            int idx[kPads]; float str[kPads]; int n = 0;
            for (int i = 0; i < (int)m->hits.size(); ++i)
            {
                const auto& h = m->hits[(size_t)i];
                if (h.pos < from) continue;
                if (h.pos >= to) break;
                if (n < kPads) { idx[n] = i; str[n] = h.strength; ++n; continue; }
                int weakest = 0; for (int k = 1; k < kPads; ++k) if (str[k] < str[weakest]) weakest = k;
                if (h.strength > str[weakest]) { idx[weakest] = i; str[weakest] = h.strength; }
            }
            std::sort(idx, idx + n);
            for (int p = 0; p < n; ++p)
            {
                cues_[p].start = m->hits[(size_t)idx[p]].pos;
                cues_[p].end = p + 1 < n ? m->hits[(size_t)idx[p + 1]].pos : cues_[p].start + beat;
            }
            for (int p = n; p < kPads; ++p) { cues_[p].start = barAt(startBar) + p * beat * 0.5; cues_[p].end = cues_[p].start + beat * 0.5; }   // sparse section: fill with 8ths
        }
        for (auto& c : cues_)
        {
            c.start = mpc::clamp(c.start, 0.0, total - 2.0);
            c.end = mpc::clamp(c.start + (c.end - c.start) * lenPct * 0.01, c.start + 64.0, total - 1.0);
        }
    }

    // ---- notes ----
    void noteOn(int note, int vel, int mode, bool mono, float atk, float rel, float pitch, bool keys, int keyPad)
    {
        if (active_ == nullptr || active_->frames() < 64) return;
        const int pad = keys ? keyPad : ((note - 36) % kPads + kPads) % kPads;
        const Cue& c = cues_[pad];
        if (c.end <= c.start) return;
        if (mono) { for (auto& v : voices_) if (v.active) startFade(v); }                 // Mono: a new pad cuts the old one
        else      { for (auto& v : voices_) if (v.active && v.note == note) startFade(v); } // Poly: only a retrigger of the same pad
        Voice& v = allocate();
        v.active = true; v.releasing = false; v.mat = active_;
        v.note = note; v.pad = pad; v.age = ++age_;
        v.start = c.start; v.end = c.end; v.pos = c.start;
        const float semis = pitch + (keys ? (float)(note - 60) : 0.0f);
        v.inc = active_->sr / sr_ * std::pow(2.0, semis / 12.0);
        v.oneShot = mode == 0; v.loop = mode == 2;
        v.gain = 0.25f + 0.75f * (vel / 127.0f) * (vel / 127.0f);
        v.env = 0.0f; v.atkStep = 1.0f / std::max(1.0f, atk); v.relStep = 1.0f / std::max(1.0f, rel);
    }
    void noteOff(int note, int mode)
    {
        if (mode == 0) return;   // One Shot ignores note-off
        for (auto& v : voices_) if (v.active && v.note == note && !v.releasing) v.releasing = true;
    }
    void startFade(Voice& v) { v.releasing = true; v.relStep = std::max(v.relStep, 1.0f / (0.004f * (float)sr_)); }   // 4 ms
    Voice& allocate()
    {
        for (auto& v : voices_) if (!v.active) return v;
        Voice* best = &voices_[0];
        for (auto& v : voices_) if (v.age < best->age) best = &v;
        return *best;
    }

    void render(float* L, float* R, int from, int to)
    {
        for (auto& v : voices_)
        {
            if (!v.active) continue;
            const Material& m = *v.mat;
            const int16_t* dl = m.l.data(); const int16_t* dr = m.r.data();
            const long last = (long)m.frames() - 3;
            const double fadeLen = 0.003 * m.sr * v.inc;   // 3 ms fade before the end of a chop (no clicks)
            for (int i = from; i < to; ++i)
            {
                // envelope
                if (v.releasing) { v.env -= v.relStep; if (v.env <= 0.0f) { v.active = false; break; } }
                else if (v.env < 1.0f) v.env = std::min(1.0f, v.env + v.atkStep);
                float edge = 1.0f;
                if (!v.loop) { const double left = v.end - v.pos; if (left < fadeLen) edge = (float)std::max(0.0, left / fadeLen); }
                const long k = (long)v.pos;
                if (k < 1 || k >= last) { v.active = false; break; }
                const float f = (float)(v.pos - (double)k);
                auto cub = [&](const int16_t* d) {
                    const float xm1 = d[k - 1], x0 = d[k], x1 = d[k + 1], x2 = d[k + 2];
                    const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
                    return (((c3 * f + c2) * f + c1) * f + x0) * (1.0f / 32768.0f);
                };
                const float g = v.env * v.gain * edge;
                L[i] += cub(dl) * g;
                if (R) R[i] += cub(dr) * g;
                v.pos += v.inc;
                if (v.pos >= v.end)
                {
                    if (v.loop && !v.releasing) v.pos = v.start + (v.pos - v.end);
                    else { v.active = false; break; }
                }
            }
        }
    }

    void updateFilter()
    {
        const float f = get(kFilter);
        if (f == lastFilter_) return;
        lastFilter_ = f;
        if (std::fabs(f) < 1.0f) { filterMode_ = 0; return; }
        // DJ filter: left = low-pass sweeping down to 150 Hz, right = high-pass sweeping up to 3 kHz
        const float t = std::fabs(f) / 100.0f;
        if (f < 0) { filterMode_ = 1; const float hz = 20000.0f * std::pow(150.0f / 20000.0f, t); filtL_.set(hz, 0.25f); filtR_.set(hz, 0.25f); }
        else       { filterMode_ = 2; const float hz = 20.0f * std::pow(3000.0f / 20.0f, t);     filtL_.set(hz, 0.25f); filtR_.set(hz, 0.25f); }
    }

    // ---- worker ----
    void workerLoop()
    {
        std::unique_ptr<flip::Song> original;
        flip::Analysis an;
        int loadedSong = -2, madeKey = 0; bool madeSync = false; double madeBpm = 0;
        int wantedSince = 0;
        while (running_.load())
        {
            delete retired_.exchange(nullptr);
            if (!ready_.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(15)); continue; }
            const int song = reqSong_.load();
            if (song != loadedSong)
            {
                loadedSong = song;
                original.reset();
                if (song < 0 || song >= (int)songs_.size()) { status_.store(0); continue; }
                status_.store(101);
                auto s = std::make_unique<flip::Song>();
                std::string err;
                if (!flip::decodeFile(dir_ + "/" + songs_[(size_t)song], *s, kMaxSeconds, err)) { status_.store(103); mpc::log("MeltFlip: %s: %s", songs_[(size_t)song].c_str(), err.c_str()); continue; }
                status_.store(102);
                std::vector<float> mono(s->l.size());
                for (size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5f * (s->l[i] + s->r[i]);
                an = flip::analyse(mono.data(), mono.size(), s->sampleRate);
                if (an.bpm <= 0) an.bpm = 120.0;
                original = std::move(s);
                publish(makeMaterial(*original, an, song, 0, false, 0, 1.0, 1.0));   // playable right away, unstretched
                madeKey = 0; madeSync = false; madeBpm = 0;
                showBpm_.store(an.bpm); showKey_.store(an.keyIndex);
                status_.store(100);
                continue;
            }
            if (!original) { std::this_thread::sleep_for(std::chrono::milliseconds(15)); continue; }
            const int key = reqKey_.load(); const bool sync = reqSync_.load(); const int fix = reqFix_.load();
            const double songBpm = an.bpm * (fix == 1 ? 2.0 : fix == 2 ? 0.5 : 1.0);
            const double target = sync ? std::round(projectBpm_.load() * 100.0) / 100.0 : 0.0;
            if (key != madeKey || sync != madeSync || (sync && std::fabs(target - madeBpm) > 0.005))
            {
                // wait until the knobs rest for ~250 ms before starting a render
                if (++wantedSince < 16) { std::this_thread::sleep_for(std::chrono::milliseconds(15)); continue; }
                wantedSince = 0;
                const double timeRatio = sync ? songBpm / target : 1.0;
                const double pitchRatio = std::pow(2.0, key / 12.0);
                cancel_.store(false);
                flip::Song out;
                status_.store(1);
                const bool ok = flip::render(*original, timeRatio, pitchRatio, out, &cancel_, [&](float p) {
                    status_.store(std::max(1, std::min(99, (int)(p * 100))));
                    // the user moved on (other song / key / sync): stop this render now
                    if (reqSong_.load() != song || reqKey_.load() != key || reqSync_.load() != sync || !running_.load()) cancel_.store(true);
                });
                if (!ok || cancel_.load() || !running_.load()) { status_.store(100); continue; }
                if (reqSong_.load() != song) continue;
                publish(makeMaterial(out, an, song, key, sync, target, timeRatio, pitchRatio));
                madeKey = key; madeSync = sync; madeBpm = target;
                showBpm_.store(an.bpm / timeRatio); showKey_.store(an.keyIndex < 0 ? -1 : (an.keyIndex >= 12 ? 12 : 0) + ((an.keyIndex % 12 + key) % 12 + 12) % 12);
                status_.store(100);
                continue;
            }
            wantedSince = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }

    Material* makeMaterial(const flip::Song& s, const flip::Analysis& an, int song, int key, bool sync, double target, double timeRatio, double)
    {
        auto* m = new Material();
        m->song = song; m->keyShift = key; m->synced = sync; m->targetBpm = target;
        m->sr = s.sampleRate;
        m->l.resize(s.l.size()); m->r.resize(s.r.size());
        for (size_t i = 0; i < s.l.size(); ++i) { m->l[i] = toI16(s.l[i]); m->r[i] = toI16(s.r[i]); }
        m->bpm = an.bpm / timeRatio;
        m->barPhase = an.barPhase * timeRatio;
        m->keyIndex = an.keyIndex;
        m->hits.reserve(an.hits.size());
        for (const auto& h : an.hits) m->hits.push_back({h.pos * timeRatio, h.strength});
        return m;
    }
    void publish(Material* m) { delete pending_.exchange(m); }

    std::string dir_;
    std::vector<std::string> songs_;
    double sr_ = 44100;

    Material* active_ = nullptr;
    Material* oldMat_ = nullptr;
    std::atomic<Material*> pending_{nullptr}, retired_{nullptr};
    std::atomic<int> reqSong_{-1}, reqKey_{0}, reqFix_{0}, status_{0}, showKey_{-1};
    std::atomic<bool> reqSync_{false}, ready_{false}, running_{true}, cancel_{false};
    std::atomic<double> projectBpm_{120.0}, showBpm_{0.0};
    std::thread worker_;

    Cue cues_[kPads];
    bool cuesDirty_ = true;
    int lastChop_ = -1, lastStart_ = -1, lastFix_ = -1;
    float lastLen_ = -1;
    Voice voices_[kVoices];
    unsigned age_ = 0;
    SVF filtL_, filtR_;
    int filterMode_ = 0;
    float lastFilter_ = -1000.0f;
    float shown_[3] = {-1000.0f, -1000.0f, -1000.0f};
    Smoother volume_;
};

MPC_PLUGIN_ENTRY(MeltFlip)
