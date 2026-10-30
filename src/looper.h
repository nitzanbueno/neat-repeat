#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace neat {

// Records input from the moment it is triggered (up to maxSamples) and plays
// the recording back as a loop of the current length. The first pass through
// the loop is the live input; every later pass replays the capture.
class Looper {
 public:
  // Allocates the capture buffer. Not real-time safe.
  void prepare(int channels, int64_t maxSamples) {
    maxSamples_ = std::max<int64_t>(maxSamples, 1);
    capture_.assign(channels, std::vector<float>(static_cast<size_t>(maxSamples_), 0.0f));
    len_ = std::clamp<int64_t>(len_, 1, maxSamples_);
    active_ = false;
  }

  // Starts a fresh loop at the current sample.
  void trigger() {
    active_ = true;
    pos_ = 0;
    written_ = 0;
  }

  void stop() { active_ = false; }

  // A longer or shorter length takes effect when the playhead reaches it. If
  // the playhead is already past the new length, the loop restarts as if it
  // were triggered right now.
  void setLength(int64_t len) {
    len = std::clamp<int64_t>(len, 1, maxSamples_);
    if (active_ && pos_ >= len) trigger();
    len_ = len;
  }

  bool active() const { return active_; }
  int64_t length() const { return len_; }
  int64_t position() const { return pos_; }

  // Processes samples [begin, end). in and out may point to the same buffers.
  void process(const float* const* in, float* const* out, int channels, uint32_t begin,
               uint32_t end) {
    const int looped = active_ ? std::min<int>(channels, static_cast<int>(capture_.size())) : 0;
    for (int c = looped; c < channels; ++c) {
      if (in[c] != out[c]) std::copy(in[c] + begin, in[c] + end, out[c] + begin);
    }
    if (!active_) return;

    for (uint32_t i = begin; i < end; ++i) {
      const bool recording = written_ < maxSamples_;
      for (int c = 0; c < looped; ++c) {
        float* buf = capture_[c].data();
        if (recording) buf[written_] = in[c][i];
        out[c][i] = buf[pos_];
      }
      if (recording) ++written_;
      if (++pos_ >= len_) pos_ = 0;
    }
  }

 private:
  std::vector<std::vector<float>> capture_;
  int64_t maxSamples_ = 1;
  int64_t len_ = 1;
  int64_t pos_ = 0;      // playhead within the loop
  int64_t written_ = 0;  // samples captured since the trigger
  bool active_ = false;
};

}  // namespace neat
