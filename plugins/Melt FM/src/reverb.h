// reverb.h - 8-line FDN reverb (shared with Melt Rack).
// SPDX-License-Identifier: MIT
#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include "dsp/math.h"

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

