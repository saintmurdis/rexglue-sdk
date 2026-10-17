/**
 * @file        kernel/xam/apps/xmp_streamer.cpp
 * @brief       Decodes a user playlist ahead on its own thread for capture output.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include "xmp_streamer.h"

#include <algorithm>
#include <cstring>

namespace rex::kernel::xam::apps {

XmpStreamer::XmpStreamer() : ring_(kRingFrames * 2) {
  thread_ = std::thread([this] { DecodeThread(); });
}

XmpStreamer::~XmpStreamer() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
  }
  cv_.notify_all();
  thread_.join();
}

void XmpStreamer::Start(Opener opener, std::vector<size_t> order, size_t position, bool repeat) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;
    opener_ = std::move(opener);
    order_ = std::move(order);
    repeat_ = repeat;
    read_ = write_ = 0;
    boundaries_.clear();
    infos_.clear();
    play_position_ = decode_position_ = order_.empty() ? 0 : std::min(position, order_.size() - 1);
    decode_done_ = order_.empty();
    song_has_audio_ = false;
    active_ = !order_.empty();
  }
  cv_.notify_all();
}

void XmpStreamer::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;
    active_ = false;
    read_ = write_ = 0;
    boundaries_.clear();
    infos_.clear();
    order_.clear();
    opener_ = nullptr;
  }
  cv_.notify_all();
}

size_t XmpStreamer::NextPosition(size_t position) const {
  if (position + 1 < order_.size()) {
    return position + 1;
  }
  return repeat_ && !order_.empty() ? 0 : kEnd;
}

XmpStreamer::ReadResult XmpStreamer::Read(float* interleaved, uint32_t frames) {
  ReadResult result;
  uint32_t done = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) {
      std::memset(interleaved, 0, size_t(frames) * 2 * sizeof(float));
      return result;
    }
    uint64_t limit = write_;
    if (!boundaries_.empty()) {
      limit = std::min(limit, boundaries_.front().first);
    }
    const uint32_t take = uint32_t(std::min<uint64_t>(frames, limit - read_));
    for (uint32_t i = 0; i < take; ++i) {
      const size_t slot = size_t((read_ + i) % kRingFrames) * 2;
      interleaved[i * 2] = ring_[slot];
      interleaved[i * 2 + 1] = ring_[slot + 1];
    }
    read_ += take;
    done = take;
    if (take) {
      song_has_audio_ = true;
    }
    if (!boundaries_.empty() && read_ == boundaries_.front().first) {
      // The song ended within (or right before) this block: pad, and the next
      // block belongs to the next song.
      play_position_ = boundaries_.front().second;
      boundaries_.pop_front();
      song_has_audio_ = false;
      if (play_position_ == kEnd) {
        active_ = false;
        result.ended = true;
      } else {
        result.song_changed = true;
      }
    } else if (take < frames && song_has_audio_) {
      ++underruns_;
    }
  }
  if (done < frames) {
    std::memset(interleaved + size_t(done) * 2, 0, size_t(frames - done) * 2 * sizeof(float));
  }
  cv_.notify_all();  // room in the ring
  return result;
}

bool XmpStreamer::Current(size_t* position, size_t* song, XmpSongInfo* info) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_ || play_position_ >= order_.size()) {
    return false;
  }
  if (position) {
    *position = play_position_;
  }
  if (song) {
    *song = order_[play_position_];
  }
  if (info) {
    auto it = infos_.find(play_position_);
    *info = it != infos_.end() ? it->second : XmpSongInfo{};
  }
  return true;
}

size_t XmpStreamer::order_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return order_.size();
}

uint64_t XmpStreamer::underrun_blocks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return underruns_;
}

void XmpStreamer::DecodeThread() {
  std::unique_ptr<XmpSongDecoder> decoder;
  uint64_t decoder_generation = ~uint64_t(0);
  size_t failures = 0;  // songs in a row that wouldn't open
  std::vector<float> chunk(size_t(kChunkFrames) * 2);
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    cv_.wait(lock, [this] {
      return quit_ ||
             (active_ && !decode_done_ && kRingFrames - (write_ - read_) >= kChunkFrames);
    });
    if (quit_) {
      return;
    }
    const uint64_t generation = generation_;
    const size_t position = decode_position_;
    if (decoder_generation != generation) {
      decoder.reset();
      decoder_generation = generation;
      failures = 0;
    }
    if (!decoder) {
      const size_t song = order_[position];
      Opener opener = opener_;
      lock.unlock();
      decoder = opener ? opener(song) : nullptr;
      lock.lock();
      if (generation != generation_) {
        decoder.reset();
        continue;
      }
      if (decoder) {
        infos_[position] = decoder->info();
        failures = 0;
      } else if (++failures >= order_.size()) {
        // Nothing in the playlist opens: end it.
        boundaries_.emplace_back(write_, kEnd);
        decode_done_ = true;
        continue;
      } else {
        // Skip it: the next song starts where this one would have.
        const size_t next = NextPosition(position);
        boundaries_.emplace_back(write_, next);
        decode_position_ = next;
        decode_done_ = next == kEnd;
        continue;
      }
    }
    lock.unlock();
    const uint32_t n = decoder->Read(chunk.data(), kChunkFrames);
    lock.lock();
    if (generation != generation_) {
      continue;  // a Start or Stop came in while decoding; drop it
    }
    if (n) {
      for (uint32_t i = 0; i < n; ++i) {
        const size_t slot = size_t((write_ + i) % kRingFrames) * 2;
        ring_[slot] = chunk[i * 2];
        ring_[slot + 1] = chunk[i * 2 + 1];
      }
      write_ += n;
    } else {
      // End of the song.
      decoder.reset();
      const size_t next = NextPosition(position);
      boundaries_.emplace_back(write_, next);
      decode_position_ = next;
      decode_done_ = next == kEnd;
    }
  }
}

}  // namespace rex::kernel::xam::apps
