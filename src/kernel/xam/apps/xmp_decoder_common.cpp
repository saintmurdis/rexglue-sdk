/**
 * @file        kernel/xam/apps/xmp_decoder_common.cpp
 * @brief       Platform-independent song decoders (test tone) and file types.
 *
 * @copyright   Copyright (c) 2026 Ashton (github.com/saintmurdis)
 * @license     BSD 3-Clause License
 */

#include "xmp_decoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace rex::kernel::xam::apps {
namespace {

class ToneDecoder final : public XmpSongDecoder {
 public:
  ToneDecoder(double hz, uint32_t duration_ms, const std::u16string& title)
      : hz_(hz), total_(uint64_t(duration_ms) * kSampleRate / 1000) {
    info_.title = title;
    info_.artist = u"rexglue";
    info_.album = u"Test Tone";
    info_.duration_ms = duration_ms;
    info_.format = "tone";
    info_.source_rate = kSampleRate;
  }

  uint32_t Read(float* interleaved, uint32_t frames) override {
    const uint32_t n = uint32_t(std::min<uint64_t>(frames, total_ - position_));
    for (uint32_t i = 0; i < n; ++i) {
      const double t = double(position_ + i) / kSampleRate;
      const float v = float(0.25 * std::sin(2.0 * 3.14159265358979323846 * hz_ * t));
      interleaved[i * 2] = v;
      interleaved[i * 2 + 1] = v;
    }
    position_ += n;
    return n;
  }

 private:
  double hz_;
  uint64_t total_;
  uint64_t position_ = 0;
};

}  // namespace

bool IsXmpSongFile(const std::filesystem::path& path) {
  static constexpr std::array<std::u16string_view, 11> kExtensions = {
      u".mp3", u".m4a", u".aac", u".mp4", u".alac", u".wav", u".aif", u".aiff", u".aifc", u".flac",
      u".caf"};
  // UTF-16 rather than string(): that converts to the ANSI code page on Windows
  // and throws for names outside it.
  std::u16string ext = path.extension().u16string();
  for (auto& c : ext) {
    if (c >= u'A' && c <= u'Z') {
      c = char16_t(c - u'A' + u'a');
    }
  }
  return std::find(kExtensions.begin(), kExtensions.end(), ext) != kExtensions.end();
}

std::u16string TruncateXmpText(std::u16string text, size_t units) {
  if (text.size() > units) {
    text.resize(units);
    if (!text.empty() && text.back() >= 0xD800 && text.back() <= 0xDBFF) {
      text.pop_back();  // half of a surrogate pair
    }
  }
  return text;
}

std::unique_ptr<XmpSongDecoder> OpenXmpTestTone(double hz, uint32_t duration_ms,
                                                const std::u16string& title) {
  return std::make_unique<ToneDecoder>(hz, duration_ms, title);
}

}  // namespace rex::kernel::xam::apps
