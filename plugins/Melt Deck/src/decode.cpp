// decode.cpp - the dr_libs decoders (public domain / MIT-0, by David Reid) compiled once.
#define DR_WAV_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#include "third_party/dr_wav.h"
#include "third_party/dr_mp3.h"
#include "third_party/dr_flac.h"
#include "decode.h"
#include <algorithm>
#include <cctype>

namespace flip {

bool decodeFile(const std::string& path, Song& out, double maxSeconds, std::string& err)
{
    std::string ext = path.size() > 4 ? path.substr(path.size() - 4) : "";
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    unsigned int ch = 0, sr = 0; drwav_uint64 frames = 0;
    float* pcm = nullptr;
    if (ext == ".wav") pcm = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &ch, &sr, &frames, nullptr);
    else if (ext == ".mp3") { drmp3_config cfg; drmp3_uint64 f = 0; pcm = drmp3_open_file_and_read_pcm_frames_f32(path.c_str(), &cfg, &f, nullptr); ch = cfg.channels; sr = cfg.sampleRate; frames = f; }
    else if (ext == "flac") pcm = drflac_open_file_and_read_pcm_frames_f32(path.c_str(), &ch, &sr, &frames, nullptr);
    else { err = "not a WAV, MP3 or FLAC file"; return false; }
    if (pcm == nullptr || ch == 0 || sr == 0 || frames == 0) { err = "could not decode"; if (pcm) drwav_free(pcm, nullptr); return false; }
    const drwav_uint64 cap = (drwav_uint64)(maxSeconds * sr);
    const size_t n = (size_t)std::min(frames, cap);
    out.sampleRate = sr;
    out.l.resize(n); out.r.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        out.l[i] = pcm[i * ch];
        out.r[i] = ch > 1 ? pcm[i * ch + 1] : out.l[i];
    }
    out.truncated = frames > cap;
    if (ext == ".wav") drwav_free(pcm, nullptr); else if (ext == ".mp3") drmp3_free(pcm, nullptr); else drflac_free(pcm, nullptr);
    return true;
}

} // namespace flip
