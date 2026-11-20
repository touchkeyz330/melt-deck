// sf2.h - a small, defensive SoundFont 2 reader for Font Box.
//
// SPDX-License-Identifier: MIT
//
// Reads the "hydra" (preset/instrument/sample tables) without loading audio, resolves a
// preset into flat playable zones (preset + instrument + global zones combined as the
// SoundFont 2.04 spec describes), and loads the 16-bit sample pool on request.
// Every index read from the file is range-checked: a broken file must never crash the MPC.
// Not implemented: modulators (except the standard velocity curve, applied by the player),
// modulation/vibrato LFOs and envelope, 24-bit sample extension, ROM samples.

#ifndef FONTBOX_SF2_H
#define FONTBOX_SF2_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sf2 {

struct SampleHeader { std::string name; uint32_t start = 0, end = 0, loopStart = 0, loopEnd = 0, rate = 44100; uint8_t root = 60; int8_t corr = 0; uint16_t link = 0, type = 1; };
struct Gen { uint16_t op = 0; int16_t amount = 0; uint8_t lo = 0, hi = 0; };
struct Bag { uint16_t gen = 0, mod = 0; };
struct PresetHeader { std::string name; uint16_t program = 0, bank = 0, bag = 0; };
struct InstHeader { std::string name; uint16_t bag = 0; };

struct Hydra
{
    std::vector<PresetHeader> phdr;
    std::vector<Bag> pbag, ibag;
    std::vector<Gen> pgen, igen;
    std::vector<InstHeader> inst;
    std::vector<SampleHeader> shdr;
    uint64_t smplOffset = 0, smplBytes = 0;   // where the 16-bit sample pool is in the file
};

// A preset as listed to the user.
struct PresetInfo { std::string name; int bank = 0, program = 0, index = 0; };

// One playable layer, fully resolved.
struct Zone
{
    int keyLo = 0, keyHi = 127, velLo = 0, velHi = 127;
    uint32_t start = 0, end = 0, loopStart = 0, loopEnd = 0;
    int loopMode = 0;              // 0 none, 1 continuous, 3 loop while held then play out
    double rate = 44100.0;
    int rootKey = 60;
    float scaleTuning = 1.0f;      // semitones per key (1 = normal)
    float tuneSemis = 0.0f;        // coarse + fine + sample correction
    int fixedKey = -1, fixedVel = -1;
    float attenDb = 0.0f, pan = 0.0f, filterHz = 20000.0f, filterQDb = 0.0f;
    float delay = 0.001f, attack = 0.001f, hold = 0.001f, decay = 0.001f, sustainDb = 0.0f, release = 0.001f;
    int exclusiveClass = 0;
};

enum Op { kStartOfs = 0, kEndOfs = 1, kStartLoopOfs = 2, kEndLoopOfs = 3, kStartCoarse = 4, kFilterFc = 8, kFilterQ = 9,
          kEndCoarse = 12, kPan = 17, kDelayVol = 33, kAttackVol = 34, kHoldVol = 35, kDecayVol = 36, kSustainVol = 37,
          kReleaseVol = 38, kInstrument = 41, kKeyRange = 43, kVelRange = 44, kStartLoopCoarse = 45, kKeynum = 46,
          kVelocity = 47, kAttenuation = 48, kEndLoopCoarse = 50, kCoarseTune = 51, kFineTune = 52, kSampleId = 53,
          kSampleModes = 54, kScaleTuning = 56, kExclusiveClass = 57, kRootKey = 58, kNumOps = 61 };

namespace detail {

inline bool seek(std::FILE* f, uint64_t pos)
{
#if defined(_WIN32)
    return _fseeki64(f, (long long)pos, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)pos, SEEK_SET) == 0;
#endif
}
inline uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t u32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline std::string name20(const uint8_t* p)
{
    std::string s(reinterpret_cast<const char*>(p), 20);
    s = s.substr(0, s.find('\0'));
    while (!s.empty() && (s.back() == ' ')) s.pop_back();
    for (char& c : s) if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '?';
    return s;
}
inline bool readAt(std::FILE* f, uint64_t pos, uint32_t len, std::vector<uint8_t>& out)
{
    out.resize(len);
    return seek(f, pos) && (len == 0 || std::fread(out.data(), 1, len, f) == len);
}

} // namespace detail

// Read the hydra (no audio). Returns false for anything that is not a usable SoundFont 2.
inline bool readHydra(const std::string& path, Hydra& h)
{
    using namespace detail;
    h = Hydra();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    uint8_t hdr[12];
    bool ok = std::fread(hdr, 1, 12, f) == 12 && std::memcmp(hdr, "RIFF", 4) == 0 && std::memcmp(hdr + 8, "sfbk", 4) == 0;
    uint64_t fileEnd = 0;
    if (ok) { seek(f, 0); std::fseek(f, 0, SEEK_END);
#if defined(_WIN32)
        fileEnd = (uint64_t)_ftelli64(f);
#else
        fileEnd = (uint64_t)ftello(f);
#endif
    }
    uint64_t pdtaPos = 0; uint32_t pdtaLen = 0;
    for (uint64_t p = 12; ok && p + 12 <= fileEnd;)
    {
        uint8_t c[12];
        if (!seek(f, p) || std::fread(c, 1, 12, f) != 12) break;
        const uint32_t len = u32(c + 4);
        if (std::memcmp(c, "LIST", 4) == 0)
        {
            if (std::memcmp(c + 8, "sdta", 4) == 0)
            {
                for (uint64_t q = p + 12; q + 8 <= p + 8 + len && q + 8 <= fileEnd;)
                {
                    uint8_t s[8];
                    if (!seek(f, q) || std::fread(s, 1, 8, f) != 8) break;
                    const uint32_t sl = u32(s + 4);
                    if (std::memcmp(s, "smpl", 4) == 0) { h.smplOffset = q + 8; h.smplBytes = std::min<uint64_t>(sl, fileEnd - (q + 8)); }
                    q += 8 + sl + (sl & 1u);
                }
            }
            else if (std::memcmp(c + 8, "pdta", 4) == 0) { pdtaPos = p + 12; pdtaLen = len >= 4 ? len - 4 : 0; }
        }
        p += 8 + (uint64_t)len + (len & 1u);
    }
    std::vector<uint8_t> pd;
    ok = ok && pdtaLen > 0 && pdtaPos + pdtaLen <= fileEnd && readAt(f, pdtaPos, pdtaLen, pd) && h.smplBytes > 0;
    std::fclose(f);
    if (!ok) return false;

    for (size_t q = 0; q + 8 <= pd.size();)
    {
        const uint8_t* c = pd.data() + q;
        const uint32_t len = u32(c + 4);
        if ((uint64_t)q + 8 + len > pd.size()) return false;
        const uint8_t* b = c + 8;
        auto is = [&](const char* id) { return std::memcmp(c, id, 4) == 0; };
        if (is("phdr")) for (uint32_t i = 0; i + 38 <= len; i += 38) h.phdr.push_back({name20(b + i), u16(b + i + 20), u16(b + i + 22), u16(b + i + 24)});
        else if (is("pbag")) for (uint32_t i = 0; i + 4 <= len; i += 4) h.pbag.push_back({u16(b + i), u16(b + i + 2)});
        else if (is("ibag")) for (uint32_t i = 0; i + 4 <= len; i += 4) h.ibag.push_back({u16(b + i), u16(b + i + 2)});
        else if (is("pgen") || is("igen"))
        {
            std::vector<Gen>& g = is("pgen") ? h.pgen : h.igen;
            for (uint32_t i = 0; i + 4 <= len; i += 4) { Gen x; x.op = u16(b + i); x.amount = (int16_t)u16(b + i + 2); x.lo = b[i + 2]; x.hi = b[i + 3]; g.push_back(x); }
        }
        else if (is("inst")) for (uint32_t i = 0; i + 22 <= len; i += 22) h.inst.push_back({name20(b + i), u16(b + i + 20)});
        else if (is("shdr"))
            for (uint32_t i = 0; i + 46 <= len; i += 46)
            {
                SampleHeader s;
                s.name = name20(b + i);
                s.start = u32(b + i + 20); s.end = u32(b + i + 24); s.loopStart = u32(b + i + 28); s.loopEnd = u32(b + i + 32);
                s.rate = u32(b + i + 36); s.root = b[i + 40]; s.corr = (int8_t)b[i + 41]; s.link = u16(b + i + 42); s.type = u16(b + i + 44);
                h.shdr.push_back(s);
            }
        q += 8 + len + (len & 1u);
    }
    // the last record of each table is a terminator
    return h.phdr.size() >= 2 && h.pbag.size() >= 1 && h.inst.size() >= 2 && h.ibag.size() >= 1 && h.shdr.size() >= 2;
}

// Presets sorted by bank then program (the terminal record excluded).
inline std::vector<PresetInfo> listPresets(const Hydra& h)
{
    std::vector<PresetInfo> v;
    for (size_t i = 0; i + 1 < h.phdr.size(); ++i) v.push_back({h.phdr[i].name, h.phdr[i].bank, h.phdr[i].program, (int)i});
    std::stable_sort(v.begin(), v.end(), [](const PresetInfo& a, const PresetInfo& b) { return a.bank != b.bank ? a.bank < b.bank : a.program < b.program; });
    return v;
}

// Load the 16-bit sample pool. maxBytes guards against files too large for the device.
inline bool readSamples(const std::string& path, const Hydra& h, std::vector<int16_t>& out, uint64_t maxBytes)
{
    out.clear();
    if (h.smplBytes == 0 || h.smplBytes > maxBytes) return false;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    const size_t n = (size_t)(h.smplBytes / 2);
    out.resize(n + 4, 0);   // +4: room for the interpolator to read past the last point
    bool ok = detail::seek(f, h.smplOffset) && std::fread(out.data(), 2, n, f) == n;
    std::fclose(f);
    // the file is little-endian; so are the MPC and desktop CPUs this builds for
    if (!ok) out.clear();
    return ok;
}

namespace detail {

struct GenSet
{
    int v[kNumOps];
    bool set[kNumOps];
    int lo[2] = {0, 0}, hi[2] = {127, 127};   // key, vel ranges
    void clear() { std::memset(v, 0, sizeof(v)); std::memset(set, 0, sizeof(set)); lo[0] = lo[1] = 0; hi[0] = hi[1] = 127; }
    void instrumentDefaults()
    {
        clear();
        v[kFilterFc] = 13500;
        v[kDelayVol] = v[kAttackVol] = v[kHoldVol] = v[kDecayVol] = v[kReleaseVol] = -12000;
        v[kKeynum] = v[kVelocity] = v[kRootKey] = -1;
        v[kScaleTuning] = 100;
    }
    void apply(const std::vector<Gen>& gens, size_t from, size_t to)
    {
        for (size_t i = from; i < to && i < gens.size(); ++i)
        {
            const Gen& g = gens[i];
            if (g.op == kKeyRange) { lo[0] = g.lo; hi[0] = g.hi; }
            else if (g.op == kVelRange) { lo[1] = g.lo; hi[1] = g.hi; }
            else if (g.op < kNumOps) { v[g.op] = g.amount; set[g.op] = true; }
        }
    }
};

inline bool zoneRange(const std::vector<Bag>& bags, size_t bag, size_t& from, size_t& to, size_t numGens)
{
    if (bag + 1 >= bags.size()) return false;
    from = bags[bag].gen; to = bags[bag + 1].gen;
    if (to > numGens) to = numGens;
    return from <= to;
}
inline float timecents(int tc) { return std::pow(2.0f, (float)std::max(-12000, std::min(tc, 8000)) / 1200.0f); }

} // namespace detail

// Flatten one preset (index into h.phdr) into playable zones.
inline std::vector<Zone> resolvePreset(const Hydra& h, int presetIndex, size_t numPoints)
{
    using namespace detail;
    std::vector<Zone> zones;
    if (presetIndex < 0 || (size_t)presetIndex + 1 >= h.phdr.size()) return zones;
    const size_t pb0 = h.phdr[(size_t)presetIndex].bag, pb1 = std::min<size_t>(h.phdr[(size_t)presetIndex + 1].bag, h.pbag.size() - 1);

    GenSet pGlobal; pGlobal.clear();
    for (size_t pb = pb0; pb < pb1 && zones.size() < 1024; ++pb)
    {
        size_t g0, g1;
        if (!zoneRange(h.pbag, pb, g0, g1, h.pgen.size())) continue;
        const bool hasInst = g1 > g0 && h.pgen[g1 - 1].op == kInstrument;
        if (!hasInst) { if (pb == pb0) pGlobal.apply(h.pgen, g0, g1); continue; }
        GenSet pz = pGlobal; pz.apply(h.pgen, g0, g1);
        const int instIdx = pz.v[kInstrument];
        if (instIdx < 0 || (size_t)instIdx + 1 >= h.inst.size()) continue;

        const size_t ib0 = h.inst[(size_t)instIdx].bag, ib1 = std::min<size_t>(h.inst[(size_t)instIdx + 1].bag, h.ibag.size() - 1);
        GenSet iGlobal; iGlobal.instrumentDefaults();
        for (size_t ib = ib0; ib < ib1 && zones.size() < 1024; ++ib)
        {
            size_t j0, j1;
            if (!zoneRange(h.ibag, ib, j0, j1, h.igen.size())) continue;
            const bool hasSample = j1 > j0 && h.igen[j1 - 1].op == kSampleId;
            if (!hasSample) { if (ib == ib0) iGlobal.apply(h.igen, j0, j1); continue; }
            GenSet iz = iGlobal; iz.apply(h.igen, j0, j1);
            const int sid = iz.v[kSampleId];
            if (sid < 0 || (size_t)sid + 1 >= h.shdr.size()) continue;
            const SampleHeader& s = h.shdr[(size_t)sid];
            if (s.type & 0x8000) continue;   // ROM sample: not in the file

            Zone z;
            z.keyLo = std::max(iz.lo[0], pz.lo[0]); z.keyHi = std::min(iz.hi[0], pz.hi[0]);
            z.velLo = std::max(iz.lo[1], pz.lo[1]); z.velHi = std::min(iz.hi[1], pz.hi[1]);
            if (z.keyLo > z.keyHi || z.velLo > z.velHi) continue;
            // preset-level generators add to the instrument's (except the non-additive ones)
            auto val = [&](int op) { return iz.v[op] + (pz.set[op] ? pz.v[op] : 0); };

            const int64_t start = (int64_t)s.start + iz.v[kStartOfs] + 32768LL * iz.v[kStartCoarse];
            const int64_t end = (int64_t)s.end + iz.v[kEndOfs] + 32768LL * iz.v[kEndCoarse];
            const int64_t ls = (int64_t)s.loopStart + iz.v[kStartLoopOfs] + 32768LL * iz.v[kStartLoopCoarse];
            const int64_t le = (int64_t)s.loopEnd + iz.v[kEndLoopOfs] + 32768LL * iz.v[kEndLoopCoarse];
            if (start < 0 || end <= start + 1 || (uint64_t)end > numPoints) continue;
            z.start = (uint32_t)start; z.end = (uint32_t)end;
            z.loopMode = iz.v[kSampleModes] & 3;
            if (z.loopMode == 2) z.loopMode = 0;
            if (z.loopMode != 0 && ls >= start && le <= end && le > ls + 1) { z.loopStart = (uint32_t)ls; z.loopEnd = (uint32_t)le; }
            else z.loopMode = 0;

            z.rate = s.rate >= 400 && s.rate <= 384000 ? (double)s.rate : 44100.0;
            z.rootKey = iz.v[kRootKey] >= 0 && iz.v[kRootKey] <= 127 ? iz.v[kRootKey] : (s.root <= 127 ? s.root : 60);
            z.scaleTuning = (float)val(kScaleTuning) / 100.0f;
            z.tuneSemis = (float)val(kCoarseTune) + (float)val(kFineTune) / 100.0f + (float)s.corr / 100.0f;
            z.fixedKey = iz.v[kKeynum] >= 0 && iz.v[kKeynum] <= 127 ? iz.v[kKeynum] : -1;
            z.fixedVel = iz.v[kVelocity] >= 1 && iz.v[kVelocity] <= 127 ? iz.v[kVelocity] : -1;
            z.attenDb = 0.4f * (float)std::max(0, std::min(val(kAttenuation), 1440)) / 10.0f;   // 0.4: the usual E-mu scaling
            z.pan = (float)std::max(-500, std::min(val(kPan), 500)) / 500.0f;
            const int fc = val(kFilterFc);
            z.filterHz = fc >= 13500 ? 20000.0f : std::max(20.0f, std::min(20000.0f, 8.176f * std::pow(2.0f, (float)fc / 1200.0f)));
            z.filterQDb = (float)std::max(0, std::min(val(kFilterQ), 960)) / 10.0f;
            z.delay = timecents(val(kDelayVol)); z.attack = timecents(val(kAttackVol)); z.hold = timecents(val(kHoldVol));
            z.decay = timecents(val(kDecayVol)); z.release = timecents(val(kReleaseVol));
            z.sustainDb = (float)std::max(0, std::min(val(kSustainVol), 1440)) / 10.0f;
            z.exclusiveClass = iz.v[kExclusiveClass];
            zones.push_back(z);
        }
    }
    return zones;
}

} // namespace sf2

#endif // FONTBOX_SF2_H
