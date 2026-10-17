/**
 * @file        kernel/xam/apps/xmp_decoder.h
 * @brief       Song decoding for the XMP app's user playlists.
 *
 * @copyright   Copyright (c) 2026 Ashton (github.com/saintmurdis)
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace rex::kernel::xam::apps {

// What a song file says about itself; empty strings when it has no tag.
struct XmpSongInfo {
  std::u16string title;
  std::u16string artist;
  std::u16string album;
  std::u16string genre;
  uint32_t track_number = 0;
  uint32_t duration_ms = 0;
  std::string format;  // "mp3", "aac", "flac", ... for logs
  double source_rate = 0.0;
};

// One open song: decoded to 48 kHz, stereo, 32-bit float, interleaved.
class XmpSongDecoder {
 public:
  static constexpr uint32_t kSampleRate = 48000;

  virtual ~XmpSongDecoder() = default;
  // Reads up to `frames` frames into `interleaved` (2 floats a frame). Returns
  // how many it read; 0 at the end of the song or on an error.
  virtual uint32_t Read(float* interleaved, uint32_t frames) = 0;
  const XmpSongInfo& info() const { return info_; }

 protected:
  XmpSongInfo info_;
};

// Whether the file name has an extension the platform decoder may handle.
bool IsXmpSongFile(const std::filesystem::path& path);

// A name taken from the file system, in the form titles expect: precomposed
// (NFC) on macOS, where file names are often decomposed ("e" + combining
// accent), which a console font may not draw. Unchanged elsewhere.
std::u16string NormalizeXmpName(const std::u16string& name);

// Cuts `text` to at most `units` UTF-16 code units without splitting a
// surrogate pair.
std::u16string TruncateXmpText(std::u16string text, size_t units);

// Opens a song file with the platform's decoder (AudioToolbox on macOS: MP3,
// AAC/M4A, ALAC, WAV, AIFF, FLAC, CAF). nullptr and a reason on failure (no
// decoder for the format, DRM, damaged file, or no decoder on this platform).
std::unique_ptr<XmpSongDecoder> OpenXmpSongFile(const std::filesystem::path& path,
                                                std::string* error);

// A sine at `hz`, amplitude 0.25 (-12 dBFS), for `duration_ms`.
std::unique_ptr<XmpSongDecoder> OpenXmpTestTone(double hz, uint32_t duration_ms,
                                                const std::u16string& title);

}  // namespace rex::kernel::xam::apps
