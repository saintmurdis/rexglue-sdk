/**
 * @file        audio/pre_frame_hook.h
 * @brief       A function the Audio Worker runs before each guest audio frame.
 *
 * @copyright   Copyright (c) 2026 Ashton (github.com/saintmurdis)
 * @license     BSD 3-Clause License
 */

#pragma once

#include <functional>

namespace rex::runtime {
class FunctionDispatcher;
class ThreadState;
}  // namespace rex::runtime

namespace rex::audio {

// Runs on the Audio Worker, a guest-capable thread, right before it calls a
// title's audio-frame callback, so the hook can call guest code at the title's
// own audio pace (one 256-sample frame, 5.33 ms). XMP capture output uses it to
// hand the title one block of music per frame (XMPCaptureOutput). One hook per
// process; an empty function removes it.
using PreFrameHook =
    std::function<void(runtime::FunctionDispatcher* dispatcher, runtime::ThreadState* thread_state)>;

void SetPreFrameHook(PreFrameHook hook);
void RunPreFrameHook(runtime::FunctionDispatcher* dispatcher, runtime::ThreadState* thread_state);

}  // namespace rex::audio
