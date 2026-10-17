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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <SDL3/SDL.h>

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");
REXCVAR_DEFINE_BOOL(audio_declick, true, "Audio",
                    "Smooth the output: fade in at start, 20 ms ramps for volume, mute and the "
                    "quit fade, and ease underruns to silence instead of cutting (clicks)");
REXCVAR_DEFINE_STRING(audio_capture_file, "", "Audio",
                      "Debug: write the exact float samples sent to the audio device "
                      "(interleaved, device channel count, 48 kHz) to this raw file");

namespace rex::audio::sdl {

SDLAudioDriver::SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool SDLAudioDriver::Initialize() {
  // Set audio category for proper OS audio handling
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");

  // Set app name for audio device identification
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");

  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXAPU_ERROR("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    return false;
  }
  sdl_initialized_ = true;

  SDL_AudioSpec desired_spec = {};
  SDL_AudioSpec obtained_spec = {};
  desired_spec.freq = frame_frequency_;
  desired_spec.format = SDL_AUDIO_F32LE;
  desired_spec.channels = frame_channels_;
  sdl_device_channels_ = frame_channels_;
  sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                          SDLCallback, this);
  if (!sdl_stream_) {
    REXAPU_ERROR("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }

  SDL_AudioDeviceID sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
  if (!sdl_device) {
    REXAPU_ERROR("SDL_GetAudioStreamDevice() failed: {}", SDL_GetError());
    return false;
  }

  if (!SDL_GetAudioDeviceFormat(sdl_device, &obtained_spec, NULL)) {
    REXAPU_WARN("SDL_GetAudioDeviceFormat() failed: {}", SDL_GetError());
    obtained_spec = desired_spec;
  }

  // A 1-channel device gets the stereo fold too, then SDL collapses to mono.
  // Handing it a 6ch stream instead would use SDL's own downmix.
  if (obtained_spec.channels <= 2) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
    desired_spec.channels = 2;
    sdl_device_channels_ = 2;
    sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                            SDLCallback, this);
    if (!sdl_stream_) {
      REXAPU_ERROR("SDL_OpenAudioDeviceStream() stereo fallback failed: {}", SDL_GetError());
      return false;
    }
    sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
    if (!sdl_device) {
      REXAPU_ERROR("SDL_GetAudioStreamDevice() failed after stereo fallback: {}", SDL_GetError());
      return false;
    }
  }

  // The endpoint layout decides which mix the callback runs, and it is the
  // first thing worth knowing when a report says the balance is wrong on one
  // speaker setup and right on another.
  const char* device_name = SDL_GetAudioDeviceName(sdl_device);
  REXAPU_INFO("audio endpoint '{}': {} ch, {} Hz, format 0x{:04X}; submitting {} ch",
              device_name ? device_name : "?", obtained_spec.channels, obtained_spec.freq,
              static_cast<uint32_t>(obtained_spec.format), static_cast<int>(sdl_device_channels_));

  if (!SDL_ResumeAudioDevice(sdl_device)) {
    REXAPU_ERROR("SDL_ResumeAudioDevice() failed: {}", SDL_GetError());
    return false;
  }

  OutputOpened();
  output_open_ = true;
  return true;
}

void SDLAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, input_frame, frame_samples_ * sizeof(float));

  static uint32_t sdl_submit_count = 0;
  if (sdl_submit_count < 10) {
    REXAPU_DEBUG("SDLAudioDriver::SubmitFrame: frame_ptr={:08X} queued_count={}", frame_ptr,
                 frames_queued_.size() + 1);
    sdl_submit_count++;
  }

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(frames_queued_.size()));
  }
}

void SDLAudioDriver::Shutdown() {
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);  // stops the callback
    sdl_stream_ = nullptr;
  }
  if (output_open_) {
    OutputClosed(fade_signalled_);
    output_open_ = false;
  }
  if (capture_) {
    std::fclose(capture_);
    capture_ = nullptr;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void SDLAudioDriver::SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                 [[maybe_unused]] int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback called with nullptr.");
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int sample_count =
      static_cast<int>(channel_samples_ * std::max<uint8_t>(driver->sdl_device_channels_, 1));
  const int len = static_cast<int>(sizeof(float) * sample_count);
  float* data = SDL_stack_alloc(float, sample_count);
  if (!data) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback failed to allocate {} samples", sample_count);
    return;
  }
  // Snapshot once. A change mid-callback would split the frame across two mixes.
  const StereoFold fold = GetStereoFold();
  const SurroundMix mix = GetSurroundMix();
  const float gain = GetOutputGain();
  const bool declick = REXCVAR_GET(audio_declick);
  const bool muted = REXCVAR_GET(audio_mute);
  const bool fading = OutputFadeOutRequested();
  const float target = (muted || fading) ? 0.0f : gain;
  if (!driver->capture_opened_) {
    driver->capture_opened_ = true;
    std::string path = REXCVAR_GET(audio_capture_file);
    if (!path.empty()) {
      // One file per output: a second or re-created driver gets <path>.2, .3, ...
      static std::atomic<int> capture_count{0};
      if (const int n = ++capture_count; n > 1) {
        path += "." + std::to_string(n);
      }
      driver->capture_ = std::fopen(path.c_str(), "wb");
      REXAPU_INFO("audio capture to {}: {}", path, driver->capture_ ? "on" : "can't open the file");
    }
  }
  float peak = 0.0f;
  while (additional_amount > 0) {
    static uint32_t sdl_callback_count = 0;
    std::unique_lock<std::mutex> guard(driver->frames_mutex_);
    float* buffer = nullptr;
    if (!driver->frames_queued_.empty()) {
      buffer = driver->frames_queued_.front();
      driver->frames_queued_.pop();
    } else if (sdl_callback_count < 10) {
      REXAPU_DEBUG("SDLCallback: no frames queued (silence)");
      sdl_callback_count++;
    }
    const int channels = driver->sdl_device_channels_;
    if (buffer && !(muted && !declick)) {
      // With the envelope the gain is applied per sample below, so volume changes
      // ramp, and the clamp comes after it (clamping first would clip loud
      // passages that a lowered volume used to bring back under full scale).
      const float frame_gain = declick ? 1.0f : gain;
      const float clip = declick ? std::numeric_limits<float>::infinity() : 1.0f;
      switch (channels) {
        case 2:
          conversion::sequential_6_BE_to_interleaved_2_LE(data, buffer, channel_samples_, fold,
                                                          frame_gain, clip);
          break;
        case 6:
          conversion::sequential_6_BE_to_interleaved_6_LE(data, buffer, channel_samples_, mix,
                                                          frame_gain, clip);
          break;
        default:
          assert_unhandled_case(driver->sdl_device_channels_);
          break;
      }
    }
    if (declick) {
      peak = std::max(peak, driver->ApplyEnvelope(data, buffer != nullptr, channels, target));
    } else if (!buffer || muted) {
      std::memset(data, 0, len);
    }
    if (!fading) {
      driver->CountUnderrun(buffer == nullptr);
    }
    if (driver->capture_) {
      std::fwrite(data, 1, len, driver->capture_);
    }
    if (!SDL_PutAudioStreamData(stream, data, len)) {
      REXAPU_ERROR("SDL_PutAudioStreamData() failed: {}", SDL_GetError());
      if (buffer) {
        driver->frames_unused_.push(buffer);
      }
      break;
    }
    if (buffer) {
      driver->frames_unused_.push(buffer);
      auto ret = driver->semaphore_->Release(1, nullptr);
      assert_true(ret);
    }
    additional_amount -= len;
  }
  if (driver->capture_) {
    std::fflush(driver->capture_);  // the process hard-exits on quit
  }
  // Quit fade: once the envelope is at zero, count device buffers of silence. CoreAudio
  // keeps 3 queued, so after 4 silent refills everything audible has played out. Without
  // the envelope, report at once (the old hard cut).
  if (fading && (!declick || (driver->env_ == 0.0f && peak < 1e-5f))) {
    if (++driver->silent_callbacks_ >= (declick ? 4u : 1u) && !driver->fade_signalled_) {
      driver->fade_signalled_ = true;
      SignalOutputFadedOut();
      REXAPU_INFO("audio faded out; underruns this session: {}, longest {:.0f} ms, {:.0f} ms total",
                  driver->underrun_episodes_, driver->underrun_longest_ * 5.333,
                  driver->underrun_frames_ * 5.333);
    }
  } else {
    driver->silent_callbacks_ = 0;
  }
  SDL_stack_free(data);
}

float SDLAudioDriver::ApplyEnvelope(float* data, bool frame, int channels, float target) {
  constexpr float kStep = 1.0f / 960.0f;  // 0 -> 1 in 20 ms at 48 kHz
  constexpr float kHoldDecay = 0.995f;    // ~4.2 ms time constant: -100 dB after ~48 ms
  channels = std::clamp(channels, 1, 6);
  if (!frame) {
    if (!underrun_) {
      // Start of an underrun: decay from where the signal was instead of cutting to 0.
      std::copy(last_out_, last_out_ + channels, hold_);
      underrun_ = true;
    }
    env_ = 0.0f;  // the next real frame fades in
  } else {
    underrun_ = false;
  }
  float peak = 0.0f;
  for (uint32_t s = 0; s < channel_samples_; ++s) {
    if (frame) {
      env_ += std::clamp(target - env_, -kStep, kStep);
    }
    for (int c = 0; c < channels; ++c) {
      hold_[c] *= kHoldDecay;
      if (std::fabs(hold_[c]) < 1e-6f) {
        hold_[c] = 0.0f;  // exact silence, no denormals
      }
      float v = hold_[c];
      if (frame) {
        v += data[s * channels + c] * env_;
      }
      // A NaN or infinity from the guest mix would otherwise stay in the held
      // sample and silence the channel for the rest of the session.
      if (!std::isfinite(v)) {
        v = 0.0f;
        hold_[c] = 0.0f;
      }
      v = std::clamp(v, -1.0f, 1.0f);
      data[s * channels + c] = v;
      last_out_[c] = v;
      peak = std::max(peak, std::fabs(v));
    }
  }
  return peak;
}

void SDLAudioDriver::CountUnderrun(bool underrun) {
  if (underrun) {
    ++underrun_run_;
    return;
  }
  if (underrun_run_) {
    ++underrun_episodes_;
    underrun_frames_ += underrun_run_;
    underrun_longest_ = std::max(underrun_longest_, underrun_run_);
    underrun_run_ = 0;
    // At most one line a minute, and only when something happened.
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    if (underrun_episodes_ != reported_episodes_ && now_ms - last_report_ms_ >= 60000) {
      REXAPU_INFO("audio underruns: {} so far, longest {:.0f} ms", underrun_episodes_,
                  underrun_longest_ * 5.333);
      reported_episodes_ = underrun_episodes_;
      last_report_ms_ = now_ms;
    }
  }
}

}  // namespace rex::audio::sdl
