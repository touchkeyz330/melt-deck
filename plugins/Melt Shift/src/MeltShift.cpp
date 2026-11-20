// MeltShift.cpp - a one-knob pitch shifter for the MPC, by Meltdown Empire.
//
// SPDX-License-Identifier: MIT
//
// One knob: centre = original pitch, left = down, right = up (up to 12 semitones each way).
// The tempo never changes.
//
// How: the live input is time-stretched by the pitch ratio with a phase vocoder (identity
// phase locking: every band keeps its phase relation to its spectral peak; transient phase
// reset so hits stay sharp; stereo-linked phases), then resampled back to the original length,
// which moves the pitch. Nothing in the spectrum is relocated, so chords and close notes stay
// clean. Original code; same vocoder core as Melt Deck's key lock.
//   SMOOTH  2048-point frames: best for melodies, chords, vocals, bass and full mixes.
//   PUNCHY  1024-point frames: tighter on drums and plucks.
// Fixed delay for every setting (the dry path too), reported to the host: the knob, Mode and
// Mix never move anything in time. A tiny feedback loop keeps the delay exact while the knob
// moves.

#include "mpc_plugin.h"
#include "dsp/math.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>

using namespace mpc::dsp;
using cfloat = std::complex<float>;
static constexpr float kTau = 6.28318530717958647692f;

struct FFTPlan
{
    size_t n = 0;
    std::vector<uint32_t> rev;
    std::vector<float> cs, sn;
    void init(size_t size)
    {
        n = size; rev.assign(size, 0); cs.assign(size / 2, 0.0f); sn.assign(size / 2, 0.0f);
        int bits = 0; while ((size_t)1 << bits < n) ++bits;
        for (size_t i = 0; i < n; ++i) { uint32_t r = 0; for (int b = 0; b < bits; ++b) if (i & ((size_t)1 << b)) r |= 1u << (bits - 1 - b); rev[i] = r; }
        for (size_t k = 0; k < n / 2; ++k) { const double a = -2.0 * 3.14159265358979323846 * (double)k / (double)n; cs[k] = (float)std::cos(a); sn[k] = (float)std::sin(a); }
    }
    void run(cfloat* data, bool inverse) const
    {
        float* d = reinterpret_cast<float*>(data);
        for (size_t i = 0; i < n; ++i) if (i < rev[i]) { std::swap(d[2 * i], d[2 * rev[i]]); std::swap(d[2 * i + 1], d[2 * rev[i] + 1]); }
        const float sgn = inverse ? -1.0f : 1.0f;
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const size_t half = len / 2, step = n / len;
            for (size_t i = 0; i < n; i += len)
                for (size_t k = 0; k < half; ++k)
                {
                    const float wr = cs[k * step], wi = sgn * sn[k * step];
                    float* u = d + 2 * (i + k); float* v = d + 2 * (i + k + half);
                    const float tr = v[0] * wr - v[1] * wi, ti = v[0] * wi + v[1] * wr;
                    v[0] = u[0] - tr; v[1] = u[1] - ti;
                    u[0] += tr; u[1] += ti;
                }
        }
        if (inverse) { const float s = 1.0f / (float)n; for (size_t i = 0; i < 2 * n; ++i) d[i] *= s; }
    }
};

// ---------------------------------------------------------------------------
// Live pitch shifter: stretch (phase vocoder reading the input history) + resample.
// ---------------------------------------------------------------------------
template <int N>
class LiveShifter
{
public:
    static constexpr int Hs = N / 4, Bins = N / 2 + 1;
    static constexpr int kRing = 16384;   // input history (power of two)
    static constexpr int kSRing = 8192;   // stretched output (power of two)

    void init()
    {
        plan_.init(N);
        for (int i = 0; i < N; ++i) win_[i] = 0.5f - 0.5f * std::cos(kTau * (float)i / (float)N);
        float s = 0.0f; for (int i = 0; i < N; ++i) s += win_[i] * win_[i];
        norm_ = (float)Hs / s;
    }

    // delay = fixed input-to-output delay in samples
    void reset(int delay, float ratio)
    {
        delay_ = delay;
        std::memset(inL_, 0, sizeof inL_); std::memset(inR_, 0, sizeof inR_);
        std::memset(sL_, 0, sizeof sL_); std::memset(sR_, 0, sizeof sR_);
        std::memset(accL_, 0, sizeof accL_); std::memset(accR_, 0, sizeof accR_);
        std::memset(logPrev_, 0, sizeof logPrev_);
        written_ = 0; t_ = 0;
        fluxMed_ = lastFlux_ = 0.0f;
        const double rate = 1.0 / ratio;
        // the next stretched sample (index 0) is source position -delay
        apos_ = -(double)delay - (N / 2) * rate;
        prevA_ = (long)std::lround(apos_);
        lastRate_ = rate;
        reset_ = true;
        sCount_ = 0;
        for (int k = 0; k < N / Hs; ++k) frame(rate, false);   // prime; discard
        sCount_ = 0;
        rp_ = 0.0;
        underruns = 0;
    }

    // one input sample in, one shifted sample out
    inline void tick(float xl, float xr, float ratio, float& yl, float& yr)
    {
        inL_[written_ & (kRing - 1)] = xl; inR_[written_ & (kRing - 1)] = xr;
        ++written_;
        const double rate = 1.0 / ratio;
        // make sure the stretched stream reaches the read point (+ interpolation taps)
        while (sCount_ < (long long)std::floor(rp_) + 3)
        {
            if (apos_ + N / 2 > (double)written_) { ++underruns; break; }
            frame(rate, true);
        }
        // cubic read of the stretched stream at rp_
        const long long i0 = (long long)std::floor(rp_);
        const float f = (float)(rp_ - (double)i0);
        auto at = [&](const float* b, long long i) { return (i < 0 || i >= sCount_) ? 0.0f : b[i & (kSRing - 1)]; };
        auto herm = [&](const float* b) {
            const float xm1 = at(b, i0 - 1), x0 = at(b, i0), x1 = at(b, i0 + 1), x2 = at(b, i0 + 2);
            const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            return ((c3 * f + c2) * f + c1) * f + x0; };
        yl = herm(sL_); yr = herm(sR_);
        // keep the delay exact: compare the source position being played with where it should be
        const double src = srcAt(rp_);
        const double err = ((double)t_ - (double)delay_) - src;
        const double trim = std::max(-0.0005, std::min(0.0005, err * 2.0e-6));   // under 1 cent
        rp_ += ratio * (1.0 + trim);
        ++t_;
    }

    long underruns = 0;

private:
    double srcAt(double s) const
    {
        const long long i = (long long)std::floor(s);
        if (i < 0) return (double)t_ - (double)delay_;
        if (i + 1 >= sCount_) return sCount_ > 0 ? src_[(sCount_ - 1) & (kSRing - 1)] : (double)t_ - (double)delay_;
        const double a = src_[i & (kSRing - 1)], b = src_[(i + 1) & (kSRing - 1)];
        return a + (s - (double)i) * (b - a);
    }

    void frame(double rate, bool emit)
    {
        // The overlap-add plays source position (frame centre - 1.5 hops x rate). When the rate
        // changes, move the frame centre so that position - and so the delay - stays continuous.
        if (lastRate_ > 0.0) apos_ += 1.5 * Hs * (rate - lastRate_);
        lastRate_ = rate;
        const long a = (long)std::lround(apos_);
        const long start = a - N / 2;
        for (int i = 0; i < N; ++i)
        {
            const long long j = (long long)start + i;
            const bool ok = j >= 0 && j < written_ && j >= written_ - kRing;
            const float l = ok ? inL_[j & (kRing - 1)] : 0.0f, r = ok ? inR_[j & (kRing - 1)] : 0.0f;
            z_[i] = cfloat(l * win_[i], r * win_[i]);
        }
        plan_.run(z_, false);
        float flux = 0.0f;
        for (int k = 0; k < Bins; ++k)
        {
            const cfloat Zk = z_[k], Zm = z_[(N - k) % N];
            const float ar = Zk.real(), ai = Zk.imag(), br = Zm.real(), bi = -Zm.imag();
            XL_[k] = cfloat(0.5f * (ar + br), 0.5f * (ai + bi));
            XR_[k] = cfloat(0.5f * (ai - bi), -0.5f * (ar - br));
            const float sr = XL_[k].real() + XR_[k].real(), si = XL_[k].imag() + XR_[k].imag();
            const float p2 = sr * sr + si * si;
            mag_[k] = p2;
            phIn_[k] = std::atan2(si, sr);
            const float lm = 0.5f * std::log1p(p2 * 1.0e4f);
            if (lm > logPrev_[k]) flux += lm - logPrev_[k];
            logPrev_[k] = lm;
        }
        // a real hit: a big jump in spectral flux, far above its running level (small wobbles on
        // steady tones must never count, or they would reset the phases mid-note)
        const bool transient = flux > 3.0f * fluxMed_ + 0.04f * (float)Bins && flux > lastFlux_ * 1.5f;
        fluxMed_ += 0.1f * (flux - fluxMed_);
        lastFlux_ = flux;
        const float hopA = (float)(a - prevA_);
        prevA_ = a;
        if (reset_ || transient || hopA <= 0.0f || hopA > 4.0f * N)
        {
            for (int k = 0; k < Bins; ++k) phOut_[k] = phIn_[k];
            reset_ = false;
        }
        else
        {
            int np = 0;
            // local maxima (neighbours only, so notes 2-3 bands apart are both found), ignoring
            // the noise floor far below the loudest band
            float mx = 0.0f; for (int k = 0; k < Bins; ++k) mx = std::max(mx, mag_[k]);
            const float flo = mx * 1.0e-8f;
            for (int k = 1; k < Bins - 1; ++k)
                if (mag_[k] > flo && mag_[k] > mag_[k - 1] && mag_[k] >= mag_[k + 1]) peaks_[np++] = k;
            auto advance = [&](int k) {
                const float omega = kTau * (float)k / (float)N;
                float dev = phIn_[k] - phPrev_[k] - omega * hopA;
                dev -= kTau * std::floor((dev + 3.14159265f) / kTau);
                float ph = phOut_[k] + (omega + dev / hopA) * (float)Hs;
                ph -= kTau * std::floor((ph + 3.14159265f) / kTau);
                phOut_[k] = ph;
            };
            if (np == 0) for (int k = 0; k < Bins; ++k) advance(k);
            else
            {
                for (int i = 0; i < np; ++i) advance(peaks_[i]);
                for (int i = 0; i + 1 < np; ++i)
                {
                    int v = peaks_[i];
                    for (int k = peaks_[i]; k <= peaks_[i + 1]; ++k) if (mag_[k] < mag_[v]) v = k;
                    valley_[i] = v;
                }
                int pi = 0;
                for (int k = 0; k < Bins; ++k)
                {
                    // move to the next peak once past the valley between the two
                    while (pi + 1 < np && k > valley_[pi]) ++pi;
                    const int p = peaks_[pi];
                    if (k != p) phOut_[k] = phOut_[p] + (phIn_[k] - phIn_[p]);
                }
            }
        }
        std::memcpy(phPrev_, phIn_, sizeof phPrev_);
        for (int k = 0; k < Bins; ++k)
        {
            const float rot = phOut_[k] - phIn_[k];
            const float c = std::cos(rot), sn = std::sin(rot);
            const cfloat& a1 = XL_[k]; const cfloat& b1 = XR_[k];
            const float lr = a1.real() * c - a1.imag() * sn, li = a1.real() * sn + a1.imag() * c;
            const float rr = b1.real() * c - b1.imag() * sn, ri = b1.real() * sn + b1.imag() * c;
            z_[k] = cfloat(lr - ri, li + rr);
            if (k > 0 && k < N / 2) z_[N - k] = cfloat(lr + ri, -li + rr);
        }
        plan_.run(z_, true);
        for (int i = 0; i < N; ++i) { accL_[i] += z_[i].real() * win_[i] * norm_; accR_[i] += z_[i].imag() * win_[i] * norm_; }
        if (emit)
        {
            // stretched sample i of this frame plays source position (frame centre - N/2 * rate) + i * rate
            const double base = apos_ - (N / 2) * rate;
            for (int i = 0; i < Hs; ++i)
            {
                const long long w = (sCount_ + i) & (kSRing - 1);
                sL_[w] = accL_[i]; sR_[w] = accR_[i]; src_[w] = base + (double)i * rate;
            }
            sCount_ += Hs;
        }
        std::memmove(accL_, accL_ + Hs, sizeof(float) * (N - Hs)); std::fill(accL_ + (N - Hs), accL_ + N, 0.0f);
        std::memmove(accR_, accR_ + Hs, sizeof(float) * (N - Hs)); std::fill(accR_ + (N - Hs), accR_ + N, 0.0f);
        apos_ += Hs * rate;
    }

    FFTPlan plan_;
    float win_[N] = {}, norm_ = 1.0f;
    cfloat z_[N], XL_[Bins], XR_[Bins];
    float phIn_[Bins] = {}, phPrev_[Bins] = {}, phOut_[Bins] = {}, mag_[Bins] = {}, logPrev_[Bins] = {};
    int peaks_[Bins] = {}, valley_[Bins] = {};
    float accL_[N] = {}, accR_[N] = {};
    float inL_[kRing] = {}, inR_[kRing] = {};
    float sL_[kSRing] = {}, sR_[kSRing] = {};
    double src_[kSRing] = {};
    long long written_ = 0, sCount_ = 0, t_ = 0;
    double apos_ = 0.0, rp_ = 0.0;
    long prevA_ = 0;
    double lastRate_ = 0.0;
    int delay_ = 0;
    bool reset_ = true;
    float fluxMed_ = 0.0f, lastFlux_ = 0.0f;
};

// ---------------------------------------------------------------------------
class MeltShift : public mpc::Plugin
{
public:
    enum Params { kPitch = 0, kSnap, kMode, kMix, kNumParams };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Melt Shift";
        i.vendor = "Meltdown Empire";
        i.uniqueId = MPC_FOURCC('M', 'F', 'W', 'p');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        i.latencySamples = kDelay;
        return i;
    }

    MeltShift() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam("pitch", "Pitch", -12.0f, 12.0f, 0.0f, "st", 2));
        addParam(boolParam("snap", "Snap", true));
        addParam(choiceParam("mode", "Mode", {"Smooth", "Punchy"}, 0));
        addParam(floatParam("mix", "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
        smooth_ = new LiveShifter<2048>();
        punchy_ = new LiveShifter<1024>();
        smooth_->init(); punchy_->init();
    }
    ~MeltShift() override { delete smooth_; delete punchy_; }

    bool formatValue(int index, float v, std::string& out) override
    {
        if (index != kPitch) return false;
        const float st = getBool(kSnap) ? std::round(v) : v;
        char b[24];
        if (std::fabs(st) < 0.005f) std::snprintf(b, sizeof b, "0 st");
        else if (getBool(kSnap)) std::snprintf(b, sizeof b, "%+d st", (int)std::lround(st));
        else std::snprintf(b, sizeof b, "%+.2f st", st);
        out = b;
        return true;
    }

    void prepare(double sampleRate, int) override { sr_ = sampleRate; reset(); }

    void reset() override
    {
        std::memset(dl_, 0, sizeof dl_);
        dlPos_ = 0;
        const float st = currentSemis();
        ratio_ = std::pow(2.0f, st / 12.0f);
        smooth_->reset(kDelay, ratio_);
        punchy_->reset(kDelay, ratio_);
        wet_ = std::fabs(st) > 0.005f ? get(kMix) * 0.01f : 0.0f;
        modeW_ = getInt(kMode) == 1 ? 1.0f : 0.0f;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const float st = currentSemis();
        const float target = std::pow(2.0f, st / 12.0f);
        const float wetTarget = std::fabs(st) > 0.005f ? get(kMix) * 0.01f : 0.0f;
        const float modeTarget = getInt(kMode) == 1 ? 1.0f : 0.0f;
        const float k20 = 1.0f - std::exp(-1.0f / (0.020f * (float)sr_));   // 20 ms smoothing
        const int n = ctx.numFrames;
        const float* inL = ctx.inputs && ctx.numInputs > 0 ? ctx.inputs[0] : nullptr;
        const float* inR = ctx.inputs && ctx.numInputs > 1 ? ctx.inputs[1] : inL;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        for (int i = 0; i < n; ++i)
        {
            const float xl = inL ? inL[i] : 0.0f, xr = inR ? inR[i] : xl;
            ratio_ += k20 * (target - ratio_);
            wet_ += k20 * (wetTarget - wet_);
            modeW_ += k20 * (modeTarget - modeW_);
            // dry path, same delay
            dl_[0][dlPos_] = xl; dl_[1][dlPos_] = xr;
            const int rd = (dlPos_ - kDelay) & (kDl - 1);
            const float dryL = dl_[0][rd], dryR = dl_[1][rd];
            dlPos_ = (dlPos_ + 1) & (kDl - 1);
            // both engines always run, so switching is seamless
            float sl, sr, pl, pr;
            smooth_->tick(xl, xr, ratio_, sl, sr);
            punchy_->tick(xl, xr, ratio_, pl, pr);
            const float wl = sl + modeW_ * (pl - sl), wr = sr + modeW_ * (pr - sr);
            const float yl = dryL + wet_ * (wl - dryL), yr = dryR + wet_ * (wr - dryR);
            outL[i] = sanitize(safe(yl));
            if (outR) outR[i] = sanitize(safe(yr));
        }
    }

private:
    static constexpr int kDelay = 4608;   // ~104 ms at 44.1 kHz: covers -12 st with 2048-point frames
    static constexpr int kDl = 8192;

    static float safe(float x)   // transparent below about -2 dBFS; shifting moves peaks
    {
        const float a = std::fabs(x);
        if (a < 0.8f) return x;
        const float y = 0.8f + 0.2f * fastTanh((a - 0.8f) / 0.2f);
        return x < 0 ? -y : y;
    }
    float currentSemis() const
    {
        const float v = mpc::clamp(get(kPitch), -12.0f, 12.0f);
        return getBool(kSnap) ? std::round(v) : v;
    }

    double sr_ = 44100.0;
    LiveShifter<2048>* smooth_ = nullptr;
    LiveShifter<1024>* punchy_ = nullptr;
    float dl_[2][kDl] = {};
    int dlPos_ = 0;
    float ratio_ = 1.0f, wet_ = 0.0f, modeW_ = 0.0f;
};

MPC_PLUGIN_ENTRY(MeltShift)
