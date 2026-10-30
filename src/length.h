#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace neat {

// Longest possible loop. The capture buffer holds this much audio from the
// trigger moment, so the length can grow while a loop is playing.
constexpr double kMaxLoopSeconds = 10.0;
constexpr double kMinLoopMs = 10.0;
constexpr double kMaxLoopMs = kMaxLoopSeconds * 1000.0;

struct SyncDivision {
  const char* name;
  double wholeNotes;
};

// Sorted from shortest to longest. "1 Bar" is a whole note regardless of the
// time signature.
constexpr SyncDivision kSyncDivisions[] = {
    {"1/64", 1.0 / 64},   {"1/32T", 1.0 / 48}, {"1/64D", 3.0 / 128}, {"1/32", 1.0 / 32},
    {"1/16T", 1.0 / 24},  {"1/32D", 3.0 / 64}, {"1/16", 1.0 / 16},   {"1/8T", 1.0 / 12},
    {"1/16D", 3.0 / 32},  {"1/8", 1.0 / 8},    {"1/4T", 1.0 / 6},    {"1/8D", 3.0 / 16},
    {"1/4", 1.0 / 4},     {"1/2T", 1.0 / 3},   {"1/4D", 3.0 / 8},    {"1/2", 1.0 / 2},
    {"1/1T", 2.0 / 3},    {"1/2D", 3.0 / 4},   {"1 Bar", 1.0},
};
constexpr int kNumSyncDivisions = sizeof(kSyncDivisions) / sizeof(kSyncDivisions[0]);

// The Length parameter is a single 0..1 value read two ways:
//  - Sync off: 10 ms .. 10 s on a logarithmic curve.
//  - Sync on: snapped to the nearest entry in kSyncDivisions.
inline double lengthMs(double norm) {
  return kMinLoopMs * std::pow(kMaxLoopMs / kMinLoopMs, std::clamp(norm, 0.0, 1.0));
}

inline double normFromMs(double ms) {
  ms = std::clamp(ms, kMinLoopMs, kMaxLoopMs);
  return std::log(ms / kMinLoopMs) / std::log(kMaxLoopMs / kMinLoopMs);
}

inline int syncIndex(double norm) {
  int i = static_cast<int>(std::lround(std::clamp(norm, 0.0, 1.0) * (kNumSyncDivisions - 1)));
  return std::clamp(i, 0, kNumSyncDivisions - 1);
}

inline double normFromSyncIndex(int index) {
  return static_cast<double>(index) / (kNumSyncDivisions - 1);
}

inline int64_t maxLoopSamples(double sampleRate) {
  return std::llround(kMaxLoopSeconds * sampleRate);
}

inline int64_t lengthSamples(double norm, bool sync, double bpm, double sampleRate) {
  double seconds = sync ? kSyncDivisions[syncIndex(norm)].wholeNotes * 4.0 * 60.0 / bpm
                        : lengthMs(norm) / 1000.0;
  int64_t samples = std::llround(seconds * sampleRate);
  return std::clamp<int64_t>(samples, 1, maxLoopSamples(sampleRate));
}

}  // namespace neat
