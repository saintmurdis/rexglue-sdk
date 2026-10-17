/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <mutex>
#include <queue>
#include <stack>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

#include <cstdio>

#include <SDL3/SDL.h>

namespace rex::audio::sdl {

class SDLAudioDriver : public AudioDriver {
 public:
  SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~SDLAudioDriver() override;

  bool Initialize();
  void SubmitFrame(uint32_t frame_ptr) override;
  void Shutdown();

 protected:
  static void SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                          int total_amount);
  // Applies the output envelope to one converted frame (frame == true) or fills an
  // underrun slot (frame == false). Returns the frame's peak after the envelope.
  float ApplyEnvelope(float* data, bool frame, int channels, float target);
  void CountUnderrun(bool underrun);

  // Output envelope (audio_declick): fades in at start, ramps volume, mute and the
  // quit fade over 20 ms, and eases an underrun to silence instead of cutting.
  // Touched only on the SDL device thread.
  float env_ = 0.0f;
  float hold_[6] = {};
  float last_out_[6] = {};
  bool underrun_ = false;
  uint32_t silent_callbacks_ = 0;
  bool fade_signalled_ = false;
  bool output_open_ = false;  // registered with OutputOpened
  // Underrun statistics, in guest frames (5.33 ms each).
  uint32_t underrun_run_ = 0;
  uint64_t underrun_episodes_ = 0;
  uint64_t underrun_frames_ = 0;
  uint32_t underrun_longest_ = 0;
  uint64_t reported_episodes_ = 0;
  int64_t last_report_ms_ = 0;
  FILE* capture_ = nullptr;
  bool capture_opened_ = false;

  rex::thread::Semaphore* semaphore_ = nullptr;

  SDL_AudioStream* sdl_stream_ = nullptr;
  bool sdl_initialized_ = false;
  uint8_t sdl_device_channels_ = 0;

  static const uint32_t frame_frequency_ = 48000;
  static const uint32_t frame_channels_ = 6;
  static const uint32_t channel_samples_ = 256;
  static const uint32_t frame_samples_ = frame_channels_ * channel_samples_;
  static const uint32_t frame_size_ = sizeof(float) * frame_samples_;
  std::queue<float*> frames_queued_ = {};
  std::stack<float*> frames_unused_ = {};
  std::mutex frames_mutex_ = {};
};

}  // namespace rex::audio::sdl
