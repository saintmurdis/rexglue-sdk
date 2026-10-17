/**
 * @file        kernel/xam/apps/xmp_streamer.h
 * @brief       Decodes a user playlist ahead on its own thread for capture output.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "xmp_decoder.h"

namespace rex::kernel::xam::apps {

// A decode thread keeps about two seconds of a playlist decoded in a ring; the
// Audio Worker reads 256-frame blocks from it without ever waiting on a file.
// A song's last block is padded with silence and the next song starts with the
// next block. Start (play, next, previous) and Stop drop whatever was decoded.
class XmpStreamer {
 public:
  // Opens the song at this index of the playlist; nullptr skips it.
  using Opener = std::function<std::unique_ptr<XmpSongDecoder>(size_t song)>;

  XmpStreamer();
  ~XmpStreamer();

  // Plays `order` (indices into the playlist) from `position`; at the end it
  // starts over if `repeat`, otherwise it ends.
  void Start(Opener opener, std::vector<size_t> order, size_t position, bool repeat);
  void Stop();

  struct ReadResult {
    bool song_changed = false;  // a new song started in this block
    bool ended = false;         // the playlist ended (no repeat)
  };
  // Fills `frames` frames, silence where nothing is decoded yet. Audio Worker.
  ReadResult Read(float* interleaved, uint32_t frames);

  // The position in the order playing now, the song index there and what its
  // file says (when it has been opened). False when stopped.
  bool Current(size_t* position, size_t* song, XmpSongInfo* info) const;
  size_t order_size() const;
  uint64_t underrun_blocks() const;

 private:
  static constexpr uint64_t kRingFrames = 96000;  // 2 s at 48 kHz
  static constexpr uint32_t kChunkFrames = 4096;
  static constexpr size_t kEnd = ~size_t(0);

  void DecodeThread();
  size_t NextPosition(size_t position) const;  // kEnd past the last song without repeat

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool quit_ = false;
  bool active_ = false;
  uint64_t generation_ = 0;
  Opener opener_;
  std::vector<size_t> order_;
  bool repeat_ = false;
  // Ring of stereo frames, read_ <= write_ absolute frame counts.
  std::vector<float> ring_;
  uint64_t read_ = 0;
  uint64_t write_ = 0;
  // Where each next song starts (absolute frame) and its order position.
  std::deque<std::pair<uint64_t, size_t>> boundaries_;
  size_t play_position_ = 0;
  size_t decode_position_ = 0;
  bool decode_done_ = false;
  bool song_has_audio_ = false;  // underruns count only after a song's first frames
  uint64_t underruns_ = 0;
  std::unordered_map<size_t, XmpSongInfo> infos_;  // by order position, once opened
  std::thread thread_;
};

}  // namespace rex::kernel::xam::apps
