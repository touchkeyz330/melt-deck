// decode.h - load WAV, MP3 or FLAC into stereo float (mono files are copied to both sides).
// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <vector>
namespace flip {
struct Song { std::vector<float> l, r; double sampleRate = 44100; bool truncated = false; };
// Background thread only (allocates, reads files). Songs longer than maxSeconds are cut.
bool decodeFile(const std::string& path, Song& out, double maxSeconds, std::string& err);
}
