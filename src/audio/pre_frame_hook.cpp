/**
 * @file        audio/pre_frame_hook.cpp
 * @brief       A function the Audio Worker runs before each guest audio frame.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <memory>
#include <mutex>

#include <rex/audio/pre_frame_hook.h>

namespace rex::audio {
namespace {
std::mutex g_mutex;
std::shared_ptr<PreFrameHook> g_hook;  // guarded by g_mutex
}  // namespace

void SetPreFrameHook(PreFrameHook hook) {
  auto next = hook ? std::make_shared<PreFrameHook>(std::move(hook)) : nullptr;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_hook = std::move(next);
}

void RunPreFrameHook(runtime::FunctionDispatcher* dispatcher, runtime::ThreadState* thread_state) {
  std::shared_ptr<PreFrameHook> hook;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    hook = g_hook;
  }
  if (hook) {
    (*hook)(dispatcher, thread_state);
  }
}

}  // namespace rex::audio
