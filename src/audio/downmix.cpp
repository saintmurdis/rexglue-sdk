/**
 * @file        audio/downmix.cpp
 * @brief       Output-stage mix parameters: 5.1 fold, 5.1 matrix and master gain
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <atomic>
#include <condition_variable>
#include <mutex>

#include <rex/audio/downmix.h>

namespace rex::audio {
namespace {

// One output device exists, so the mix parameters are process state. The
// reader is the SDL device callback, which runs every 5.33 ms, so a plain
// mutex costs nothing and avoids a torn read across the four weights.
std::mutex g_mutex;
StereoFold g_fold = {};
SurroundMix g_mix = {};
float g_gain = 1.0f;

std::atomic<bool> g_fade_requested{false};
std::mutex g_fade_mutex;
std::condition_variable g_fade_cv;
int g_outputs_open = 0;    // guarded by g_fade_mutex
int g_outputs_faded = 0;   // open outputs that have signalled

}  // namespace

void SetStereoFold(const StereoFold& fold) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_fold = fold;
}

StereoFold GetStereoFold() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_fold;
}

void SetSurroundMix(const SurroundMix& mix) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_mix = mix;
}

SurroundMix GetSurroundMix() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_mix;
}

void SetOutputGain(float linear) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_gain = linear;
}

float GetOutputGain() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_gain;
}

void OutputOpened() {
  std::lock_guard<std::mutex> lock(g_fade_mutex);
  ++g_outputs_open;
}

void OutputClosed(bool signalled_fade_out) {
  {
    std::lock_guard<std::mutex> lock(g_fade_mutex);
    --g_outputs_open;
    if (signalled_fade_out) {
      --g_outputs_faded;
    }
  }
  g_fade_cv.notify_all();
}

void BeginOutputFadeOut() {
  g_fade_requested.store(true, std::memory_order_release);
}

bool OutputFadeOutRequested() {
  return g_fade_requested.load(std::memory_order_acquire);
}

void SignalOutputFadedOut() {
  {
    std::lock_guard<std::mutex> lock(g_fade_mutex);
    ++g_outputs_faded;
  }
  g_fade_cv.notify_all();
}

bool WaitOutputFadedOut(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(g_fade_mutex);
  return g_fade_cv.wait_for(lock, timeout, [] { return g_outputs_faded >= g_outputs_open; });
}

}  // namespace rex::audio
